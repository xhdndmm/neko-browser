#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "neko/base/status.h"
#include "neko/dom/element.h"
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
  Page() = default;

  // Parses |html| into a DOM document and computes styles.
  base::Result<void> LoadHtml(std::string_view html);

  // Reads a UTF-8 file and loads it as HTML.
  base::Result<void> LoadFile(std::string_view path);

  // Builds the layout tree at the given viewport width.
  void Layout(float viewport_width);

  // Rasterizes the laid-out page into a |width| x |height| image.
  paint::Rasterizer Rasterize(int width, int height) const;

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
};

}  // namespace neko::renderer
