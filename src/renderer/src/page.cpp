#include "neko/renderer/page.h"

#include "neko/base/logging.h"
#include "neko/base/status.h"
#include "neko/base/thread_pool.h"
#include "neko/css/color.h"
#include "neko/html/parser.h"
#include "neko/paint/painter.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace neko::renderer {
namespace {

// Depth-first search for the first layout box owned by |target| (block-level
// boxes and atomic inline boxes carry the element's own geometry).
const layout::LayoutBox* FindElementBox(const layout::LayoutBox& box,
                                        const dom::Element* target)
{
  if (box.element == target) {
    return &box;
  }
  for (const auto& child : box.children) {
    if (const layout::LayoutBox* hit = FindElementBox(*child, target)) {
      return hit;
    }
  }
  for (const layout::Line& line : box.lines) {
    for (const layout::InlineBox& ib : line.boxes) {
      if (ib.block_box != nullptr) {
        if (const layout::LayoutBox* hit = FindElementBox(*ib.block_box, target)) {
          return hit;
        }
      }
    }
  }
  for (const auto& child : box.positioned_children) {
    if (const layout::LayoutBox* hit = FindElementBox(*child, target)) {
      return hit;
    }
  }
  for (const auto& f : box.floats) {
    if (const layout::LayoutBox* hit = FindElementBox(*f, target)) {
      return hit;
    }
  }
  return nullptr;
}

// Aggregates the laid-out fragments of an element that has no box of its own
// (inline text runs and replaced inline atoms such as <img>), returning the
// union rectangle in document coordinates.
bool CollectFragmentRect(const layout::LayoutBox& box,
                         const dom::Element* target,
                         float& min_x,
                         float& min_y,
                         float& max_right,
                         float& max_bottom)
{
  bool found = false;
  const auto add = [&](float x, float y, float w, float h) {
    const float right = x + w;
    const float bottom = y + h;
    if (!found) {
      min_x = x;
      min_y = y;
      max_right = right;
      max_bottom = bottom;
      found = true;
    } else {
      min_x = std::min(min_x, x);
      min_y = std::min(min_y, y);
      max_right = std::max(max_right, right);
      max_bottom = std::max(max_bottom, bottom);
    }
  };
  for (const layout::Line& line : box.lines) {
    for (const layout::TextRun& run : line.runs) {
      if (run.element == target) {
        const float w = run.width > 0
                            ? run.width
                            : static_cast<float>(run.text.size()) * run.font_size;
        add(run.x, run.y, w, run.font_size);
      }
    }
    for (const layout::InlineBox& ib : line.boxes) {
      if (ib.image != nullptr && ib.element == target) {
        add(ib.x, ib.y, ib.width, ib.height);
      }
      if (ib.block_box != nullptr &&
          CollectFragmentRect(*ib.block_box, target, min_x, min_y, max_right, max_bottom)) {
        found = true;
      }
    }
  }
  for (const auto& child : box.children) {
    if (CollectFragmentRect(*child, target, min_x, min_y, max_right, max_bottom)) {
      found = true;
    }
  }
  for (const auto& child : box.positioned_children) {
    if (CollectFragmentRect(*child, target, min_x, min_y, max_right, max_bottom)) {
      found = true;
    }
  }
  for (const auto& f : box.floats) {
    if (CollectFragmentRect(*f, target, min_x, min_y, max_right, max_bottom)) {
      found = true;
    }
  }
  return found;
}

// Depth-first hit-test over the layout tree.  Returns the innermost element
// whose content contains (x, y): block children and inline runs are searched
// before the box's own border box so deeper content wins.
const dom::Element* ElementAt(const layout::LayoutBox& box, float x, float y)
{
  for (const auto& child : box.children) {
    if (const dom::Element* hit = ElementAt(*child, x, y)) {
      return hit;
    }
  }
  for (const layout::Line& line : box.lines) {
    // Atomic inline boxes (replaced <img>, inline-block) sit in line.boxes;
    // an inline-block's inner box is translated to absolute coordinates, so
    // recurse into it to resolve clicks on its content too.
    for (const layout::InlineBox& ib : line.boxes) {
      if (ib.block_box != nullptr) {
        const layout::LayoutBox& bb = *ib.block_box;
        if (x >= bb.x && x < bb.x + bb.width && y >= bb.y && y < bb.y + bb.height) {
          if (const dom::Element* hit = ElementAt(bb, x, y)) {
            return hit;
          }
          return ib.element;
        }
      } else if (ib.image != nullptr) {
        if (x >= ib.x && x < ib.x + ib.width && y >= ib.y && y < ib.y + ib.height) {
          return ib.element;
        }
      }
    }
    for (const layout::TextRun& run : line.runs) {
      // Prefer the measured width; fall back to the monospace model for runs
      // built without a font (width stays 0 only in that case).
      const float width =
          run.width > 0 ? run.width : static_cast<float>(run.text.size()) * run.font_size;
      if (x >= run.x && x < run.x + width && y >= run.y && y < run.y + run.font_size) {
        return run.element;
      }
    }
  }
  // A click on the box's own border box (padding/background, or an empty
  // block-level element) resolves to the box itself.
  if (x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height) {
    return box.element;
  }
  return nullptr;
}

} // namespace

Page::Page()
{
  // Touch the default sans-serif selector so the first layout/paint pass does
  // not pay the font-discovery cost; failure is fine (8x8 fallback).
  fonts_.SelectorFor("sans-serif");
}

void Page::LoadHtmlImpl(std::string_view bytes, base::encoding::Charset charset)
{
  std::lock_guard<std::mutex> lock(mutex_);
  // The HTML tokenizer consumes UTF-8; transcode the raw bytes (per WHATWG
  // the BOM, if present, overrides the label) before parsing.
  const std::string utf8 = base::encoding::DecodeToUtf8(bytes, charset);
  document_ = html::Parser(utf8).Parse();
  // The old document is gone: stale hover/active pointers must not survive
  // into the next cascade pass (they would dangle and be dereferenced while
  // matching :hover/:active).
  styles_.SetHoveredElement(nullptr);
  styles_.SetActiveElement(nullptr);
  focused_element_ = nullptr;
  styles_.ApplyStyles(*document_);
  root_.reset();
  // The old DOM is gone; image entries keyed by element address are stale.
  images_.clear();
  display_list_.reset();
  BumpVersion();
}

base::Result<void> Page::LoadHtml(std::string_view html)
{
  const base::encoding::Charset detected = base::encoding::DetectHtmlCharset(html, std::nullopt);
  LoadHtmlImpl(html, detected);
  return base::Ok();
}

base::Result<void> Page::LoadHtml(std::string_view bytes, base::encoding::Charset http_hint)
{
  const base::encoding::Charset detected = base::encoding::DetectHtmlCharset(bytes, http_hint);
  LoadHtmlImpl(bytes, detected);
  return base::Ok();
}

void Page::ReapplyStyles()
{
  std::lock_guard<std::mutex> lock(mutex_);
  ReapplyStylesLocked();
}

void Page::SetHoveredElement(const dom::Element* element)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (document_ == nullptr) {
    return;
  }
  styles_.SetHoveredElement(element);
  // Rebuild the layout immediately (rather than only invalidating root_) so
  // the layout tree stays valid across a hover change.  Leaving root_ null
  // would make the UI's Refresh() treat the page as freshly loaded and reset
  // the scroll position to the top.
  styles_.ApplyStyles(*document_);
  LayoutLocked(viewport_width_, viewport_height_);
}

void Page::SetActiveElement(const dom::Element* element)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (document_ == nullptr) {
    return;
  }
  styles_.SetActiveElement(element);
  styles_.ApplyStyles(*document_);
  LayoutLocked(viewport_width_, viewport_height_);
}

void Page::SetFocusedElement(const dom::Element* element)
{
  std::lock_guard<std::mutex> lock(mutex_);
  focused_element_ = element;
}

const dom::Element* Page::FocusedElement() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return focused_element_;
}

void Page::ReapplyStylesLocked()
{
  if (document_ == nullptr) {
    return;
  }
  styles_.ApplyStyles(*document_);
  // Rebuild the layout tree right away so the document/root stays consistent
  // for hit-testing and geometry queries even before the UI repaints (which
  // would otherwise see a null root and defer everything to its own pass).
  LayoutLocked(viewport_width_, viewport_height_);
  display_list_.reset();
  BumpVersion();
}

void Page::SetExternalStylesheets(std::vector<css::StyleSheet> sheets)
{
  std::lock_guard<std::mutex> lock(mutex_);
  styles_.SetExternalStylesheets(std::move(sheets));
  if (document_ == nullptr) {
    return;
  }
  styles_.ApplyStyles(*document_);
  root_.reset();
  display_list_.reset();
  BumpVersion();
}

base::Result<void> Page::LoadFile(std::string_view path)
{
  std::ifstream in(std::string(path), std::ios::binary);
  if (!in.is_open()) {
    return base::Err(base::Error::Io("cannot open file: " + std::string(path)));
  }
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return LoadHtml(content);
}

void Page::Layout(float viewport_width, float viewport_height)
{
  std::lock_guard<std::mutex> lock(mutex_);
  LayoutLocked(viewport_width, viewport_height);
}

void Page::LayoutLocked(float viewport_width, float viewport_height)
{
  if (document_ == nullptr) {
    return;
  }
  viewport_width_ = viewport_width;
  viewport_height_ = viewport_height;
  layout::LayoutEngine engine(styles_, &fonts_, this);
  root_ = engine.BuildLayoutTree(*document_, viewport_width, viewport_height);
  display_list_.reset();
  BumpVersion();
}

void Page::SetElementImage(const dom::Element& element, image::Image image)
{
  std::lock_guard<std::mutex> lock(mutex_);
  images_[&element] = std::move(image);
  root_.reset(); // the replaced box's intrinsic size may have changed
  display_list_.reset();
  BumpVersion();
}

// NOTE: called only from LayoutEngine while Layout() holds the mutex, so this
// must not lock again (would deadlock).
const image::Image* Page::Find(const dom::Element& element) const
{
  const auto it = images_.find(&element);
  return it != images_.end() ? &it->second : nullptr;
}

const paint::DisplayList& Page::EnsureDisplayList() const
{
  if (!display_list_.has_value() || display_list_version_ != version_) {
    const paint::Painter painter(root_.get());
    display_list_ = painter.Paint();
    display_list_version_ = version_;
  }
  return *display_list_;
}

paint::Rasterizer
Page::Rasterize(int width, int height, float y_offset, base::ThreadPool* pool) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  paint::Rasterizer image(width, height);
  image.SetFontRegistry(&fonts_);
  image.Clear(CanvasBackgroundColor());
  if (root_ == nullptr) {
    return image;
  }
  const paint::DisplayList& list = EnsureDisplayList();
  image.SetScrollOffset(y_offset);
  if (pool != nullptr) {
    image.RasterizeParallel(list, *pool);
  } else {
    image.Rasterize(list);
  }
  return image;
}

void Page::RasterizeFull(paint::Rasterizer& raster, float y_offset, base::ThreadPool* pool) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  raster.SetFontRegistry(&fonts_);
  raster.Clear(CanvasBackgroundColor());
  if (root_ == nullptr) {
    return;
  }
  const paint::DisplayList& list = EnsureDisplayList();
  raster.SetScrollOffset(y_offset);
  if (pool != nullptr) {
    raster.RasterizeParallel(list, *pool);
  } else {
    raster.Rasterize(list);
  }
}

void Page::RasterizeInto(paint::Rasterizer& raster, int band_y0, int band_y1, float y_offset) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (root_ == nullptr) {
    return;
  }
  const css::Color background = CanvasBackgroundColor();
  raster.ClearBand(band_y0, band_y1, background);
  raster.SetVisibleBand(band_y0, band_y1);
  raster.SetScrollOffset(y_offset);
  raster.Rasterize(EnsureDisplayList());
  raster.ResetVisibleBand();
}

std::uint64_t Page::layout_version() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return version_;
}

css::Color Page::CanvasBackgroundColor() const
{
  const dom::Element* html = document_ != nullptr ? document_->document_element() : nullptr;
  if (html != nullptr) {
    const style::ComputedStyle& html_style = styles_.StyleFor(*html);
    if (html_style.background_color.has_value()) {
      return html_style.background_color.value();
    }
    // CSS background propagation (HTML spec, canvas background): when <html>
    // has no background, a <body> background paints the whole canvas.
    for (const dom::Node* child : html->ChildNodes()) {
      if (child->node_type() != dom::NodeType::kElement) {
        continue;
      }
      const auto* element = static_cast<const dom::Element*>(child);
      if (element->tag_name() != "body") {
        continue;
      }
      const style::ComputedStyle& body_style = styles_.StyleFor(*element);
      if (body_style.background_color.has_value()) {
        return body_style.background_color.value();
      }
      break;
    }
  }
  return css::Color{255, 255, 255, 255};
}

float Page::ContentHeight() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (root_ == nullptr) {
    return 0;
  }
  // The root box spans the full laid-out content.
  return root_->height;
}

const dom::Element* Page::ElementAt(float x, float y) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (root_ == nullptr) {
    return nullptr;
  }
  return renderer::ElementAt(*root_, x, y);
}

std::optional<ElementGeometry> Page::ElementBoxGeometry(const dom::Element& element)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (document_ == nullptr) {
    return std::nullopt;
  }
  if (root_ == nullptr) {
    // No layout yet (e.g. page scripts run before the UI lays out): build one
    // at the last viewport size so geometry queries have real values.
    LayoutLocked(viewport_width_ > 0 ? viewport_width_ : 800, viewport_height_);
  }
  if (root_ == nullptr) {
    return std::nullopt;
  }
  // An element with its own layout box: report the box geometry directly.
  if (const layout::LayoutBox* box = FindElementBox(*root_, &element)) {
    ElementGeometry g;
    g.x = box->x;
    g.y = box->y;
    g.width = box->width;
    g.height = box->height;
    g.border_top = box->border_top;
    g.border_left = box->border_left;
    g.client_width = std::max(0.0f, box->width - box->border_left - box->border_right);
    g.client_height = std::max(0.0f, box->height - box->border_top - box->border_bottom);
    return g;
  }
  // Inline text and replaced elements without their own box: aggregate their
  // fragments (no borders, so the padding box equals the border box).
  float min_x = 0;
  float min_y = 0;
  float max_right = 0;
  float max_bottom = 0;
  if (CollectFragmentRect(*root_, &element, min_x, min_y, max_right, max_bottom)) {
    ElementGeometry g;
    g.x = min_x;
    g.y = min_y;
    g.width = max_right - min_x;
    g.height = max_bottom - min_y;
    g.client_width = g.width;
    g.client_height = g.height;
    return g;
  }
  return std::nullopt;
}

std::string Page::DumpDom() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return document_ != nullptr ? document_->ToString() : std::string();
}

std::string Page::DumpLayoutTree() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::string out;
  struct Printer
  {
    std::string& out;
    void Print(const layout::LayoutBox& box, int depth)
    {
      out.append(static_cast<std::size_t>(depth) * 2, ' ');
      out += '<';
      out += box.element != nullptr ? box.element->tag_name() : "anonymous";
      out += "> [";
      out += std::to_string(static_cast<int>(box.x));
      out += ",";
      out += std::to_string(static_cast<int>(box.y));
      out += " ";
      out += std::to_string(static_cast<int>(box.width));
      out += "x";
      out += std::to_string(static_cast<int>(box.height));
      out += "]";
      if (!box.lines.empty()) {
        out += " lines=";
        out += std::to_string(box.lines.size());
      }
      out += "\n";
      for (const auto& child : box.children) {
        Print(*child, depth + 1);
      }
    }
  };
  if (root_ != nullptr) {
    Printer printer{out};
    printer.Print(*root_, 0);
  }
  return out;
}

} // namespace neko::renderer
