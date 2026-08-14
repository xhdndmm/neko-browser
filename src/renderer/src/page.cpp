#include "neko/renderer/page.h"

#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include "neko/base/logging.h"
#include "neko/base/status.h"
#include "neko/css/color.h"
#include "neko/html/parser.h"
#include "neko/paint/painter.h"

namespace neko::renderer {
namespace {

// Depth-first hit-test over the layout tree.  Returns the innermost element
// whose content contains (x, y): block children and inline runs are searched
// before the box's own border box so deeper content wins.
const dom::Element* ElementAt(const layout::LayoutBox& box, float x, float y) {
  for (const auto& child : box.children) {
    if (const dom::Element* hit = ElementAt(*child, x, y)) {
      return hit;
    }
  }
  for (const layout::Line& line : box.lines) {
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

}  // namespace

Page::Page() {
  // Touch the default sans-serif selector so the first layout/paint pass does
  // not pay the font-discovery cost; failure is fine (8x8 fallback).
  fonts_.SelectorFor("sans-serif");
}

base::Result<void> Page::LoadHtml(std::string_view html) {
  std::lock_guard<std::mutex> lock(mutex_);
  document_ = html::Parser(html).Parse();
  styles_.ApplyStyles(*document_);
  root_.reset();
  return base::Ok();
}

void Page::ReapplyStyles() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (document_ == nullptr) {
    return;
  }
  styles_.ApplyStyles(*document_);
  root_.reset();
}

base::Result<void> Page::LoadFile(std::string_view path) {
  std::ifstream in(std::string(path), std::ios::binary);
  if (!in.is_open()) {
    return base::Err(base::Error::Io("cannot open file: " + std::string(path)));
  }
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return LoadHtml(content);
}

void Page::Layout(float viewport_width) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (document_ == nullptr) {
    return;
  }
  viewport_width_ = viewport_width;
  layout::LayoutEngine engine(styles_, &fonts_, this);
  root_ = engine.BuildLayoutTree(*document_, viewport_width);
}

void Page::SetElementImage(const dom::Element& element, image::Image image) {
  std::lock_guard<std::mutex> lock(mutex_);
  images_[&element] = std::move(image);
  root_.reset();  // the replaced box's intrinsic size may have changed
}

// NOTE: called only from LayoutEngine while Layout() holds the mutex, so this
// must not lock again (would deadlock).
const image::Image* Page::Find(const dom::Element& element) const {
  const auto it = images_.find(&element);
  return it != images_.end() ? &it->second : nullptr;
}

paint::Rasterizer Page::Rasterize(int width, int height, float y_offset) const {
  std::lock_guard<std::mutex> lock(mutex_);
  paint::Rasterizer image(width, height);
  image.SetFontRegistry(&fonts_);
  image.Clear(CanvasBackgroundColor());
  if (root_ == nullptr) {
    return image;
  }
  image.SetScrollOffset(y_offset);
  const paint::Painter painter(root_.get());
  image.Rasterize(painter.Paint());
  return image;
}

css::Color Page::CanvasBackgroundColor() const {
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

float Page::ContentHeight() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (root_ == nullptr) {
    return 0;
  }
  // The root box spans the full laid-out content.
  return root_->height;
}

const dom::Element* Page::ElementAt(float x, float y) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (root_ == nullptr) {
    return nullptr;
  }
  return renderer::ElementAt(*root_, x, y);
}

std::string Page::DumpDom() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return document_ != nullptr ? document_->ToString() : std::string();
}

std::string Page::DumpLayoutTree() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string out;
  struct Printer {
    std::string& out;
    void Print(const layout::LayoutBox& box, int depth) {
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

}  // namespace neko::renderer
