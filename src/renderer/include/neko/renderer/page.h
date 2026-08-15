#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "neko/base/status.h"
#include "neko/dom/element.h"
#include "neko/graphics/font_registry.h"
#include "neko/image/image.h"
#include "neko/layout/layout_tree.h"
#include "neko/paint/rasterizer.h"
#include "neko/style/style_engine.h"

namespace neko::renderer {

// The minimal page pipeline: HTML -> DOM -> style -> layout -> paint.
//
// Lifecycle: LoadHtml() -> Layout(viewport) -> Rasterize(w, h).
// Headless by design: network fetching happens in the browser application
// (which injects decoded <img> data via SetElementImage).
class Page : public layout::ImageProvider {
 public:
  Page();

  // Parses |html| into a DOM document and computes styles.
  base::Result<void> LoadHtml(std::string_view html);

  // Re-runs the style cascade over the document.  Needed after page scripts
  // mutate the DOM (attribute/style changes, node insertion) so the layout
  // reflects the new state.
  void ReapplyStyles();

  // Registers parsed external stylesheets (<link rel=stylesheet> content
  // fetched and parsed by the browser application layer) and re-runs the
  // cascade so layout reflects them.
  void SetExternalStylesheets(std::vector<css::StyleSheet> sheets);

  // Reads a UTF-8 file and loads it as HTML.
  base::Result<void> LoadFile(std::string_view path);

  // Builds the layout tree at the given viewport width.
  void Layout(float viewport_width);

  // Rasterizes the laid-out page into a |width| x |height| image.  |y_offset|
  // scrolls the visible region (see paint::Rasterizer::SetScrollOffset).
  paint::Rasterizer Rasterize(int width, int height, float y_offset = 0) const;

  // Total content height in px after Layout(); 0 before Layout().
  float ContentHeight() const;

  // Attaches a decoded image to an <img> element and invalidates the layout
  // so the replaced box picks up its intrinsic size.
  void SetElementImage(const dom::Element& element, image::Image image);

  // layout::ImageProvider.
  const image::Image* Find(const dom::Element& element) const override;

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
  // Canvas background per CSS propagation: <html> background, else a <body>
  // background, else white.  Paints the whole viewport.
  css::Color CanvasBackgroundColor() const;

  std::unique_ptr<dom::Document> document_;
  style::StyleEngine styles_;
  std::unique_ptr<layout::LayoutBox> root_;
  float viewport_width_ = 800;

  graphics::FontRegistry fonts_;
  std::unordered_map<const dom::Element*, image::Image> images_;

  // Guards document_/styles_/root_/images_ across the GUI (paint, hit-test)
  // and worker (navigation, image injection) threads.
  mutable std::mutex mutex_;
};

}  // namespace neko::renderer
