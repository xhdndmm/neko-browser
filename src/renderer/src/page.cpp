#include "neko/renderer/page.h"

#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include "neko/base/logging.h"
#include "neko/base/status.h"
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
  document_ = html::Parser(html).Parse();
  styles_.ApplyStyles(*document_);
  root_.reset();
  return base::Ok();
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
  if (document_ == nullptr) {
    return;
  }
  viewport_width_ = viewport_width;
  layout::LayoutEngine engine(styles_, &fonts_);
  root_ = engine.BuildLayoutTree(*document_, viewport_width);
}

paint::Rasterizer Page::Rasterize(int width, int height, float y_offset) const {
  paint::Rasterizer image(width, height);
  image.SetFontRegistry(&fonts_);
  image.Clear(css::Color{255, 255, 255, 255});
  if (root_ == nullptr) {
    return image;
  }
  image.SetScrollOffset(y_offset);
  const paint::Painter painter(root_.get());
  image.Rasterize(painter.Paint());
  return image;
}

float Page::ContentHeight() const {
  if (root_ == nullptr) {
    return 0;
  }
  // The root box spans the full laid-out content.
  return root_->height;
}

const dom::Element* Page::ElementAt(float x, float y) const {
  if (root_ == nullptr) {
    return nullptr;
  }
  return renderer::ElementAt(*root_, x, y);
}

std::string Page::DumpDom() const {
  return document_ != nullptr ? document_->ToString() : std::string();
}

std::string Page::DumpLayoutTree() const {
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
