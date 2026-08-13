#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "neko/base/status.h"
#include "neko/dom/element.h"
#include "neko/graphics/font_library.h"
#include "neko/layout/layout_tree.h"
#include "neko/paint/rasterizer.h"
#include "neko/style/style_engine.h"

namespace neko::renderer {

// The minimal page pipeline: HTML -> DOM -> style -> layout -> paint.
//
// Lifecycle: LoadHtml() -> Layout(viewport) -> Rasterize(w, h).
// Headless by design: network fetching happens in the browser application.
class Page {
 public:
  Page();

  // Parses |html| into a DOM document and computes styles.
  base::Result<void> LoadHtml(std::string_view html);

  // Reads a UTF-8 file and loads it as HTML.
  base::Result<void> LoadFile(std::string_view path);

  // Builds the layout tree at the given viewport width.
  void Layout(float viewport_width);

  // Rasterizes the laid-out page into a |width| x |height| image.  |y_offset|
  // scrolls the visible region (see paint::Rasterizer::SetScrollOffset).
  paint::Rasterizer Rasterize(int width, int height, float y_offset = 0) const;

  // Total content height in px after Layout(); 0 before Layout().
  float ContentHeight() const;

  // Returns the innermost element whose laid-out content (inline text run or
  // border box) contains the point |x|,|y| in document coordinates (before
  // scroll).  Returns nullptr before Layout() or when the point is outside the
  // laid-out content.  Used for link hit-testing.
  const dom::Element* ElementAt(float x, float y) const;

  dom::Document* document() { return document_.get(); }
  const layout::LayoutBox* layout_root() const { return root_.get(); }
  const style::StyleEngine& styles() const { return styles_; }

  std::string DumpDom() const;
  std::string DumpLayoutTree() const;

 private:
  std::unique_ptr<dom::Document> document_;
  style::StyleEngine styles_;
  std::unique_ptr<layout::LayoutBox> root_;
  float viewport_width_ = 800;

  graphics::FontLibrary fonts_;
  const graphics::FontFace* default_font_ = nullptr;  // system sans, may be null
};

}  // namespace neko::renderer
