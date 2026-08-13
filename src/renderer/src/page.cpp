#include "neko/renderer/page.h"

#include <fstream>
#include <string>
#include <string_view>

#include "neko/base/logging.h"
#include "neko/base/status.h"
#include "neko/html/parser.h"
#include "neko/paint/painter.h"

namespace neko::renderer {

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
  layout::LayoutEngine engine(styles_);
  root_ = engine.BuildLayoutTree(*document_, viewport_width);
}

paint::Rasterizer Page::Rasterize(int width, int height) const {
  paint::Rasterizer image(width, height);
  image.Clear(css::Color{255, 255, 255, 255});
  if (root_ == nullptr) {
    return image;
  }
  const paint::Painter painter(root_.get());
  image.Rasterize(painter.Paint());
  return image;
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
