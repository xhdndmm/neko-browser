#pragma once

#include <memory>
#include <string>
#include <vector>

#include "neko/css/color.h"
#include "neko/dom/element.h"
#include "neko/style/computed_style.h"
#include "neko/style/style_engine.h"

namespace neko::graphics {
class FontRegistry;
}
namespace neko::image {
struct Image;
}

namespace neko::layout {

// Forward declaration: InlineBox holds a unique_ptr<LayoutBox> for inline-block
// content, but LayoutBox is defined below.
struct LayoutBox;

// Lookup of decoded images for replaced elements (implemented by the renderer
// page; the layout engine is network/image-free).
class ImageProvider {
 public:
  virtual ~ImageProvider() = default;
  // Image for |element| (an <img>), or nullptr when not loaded yet.
  virtual const image::Image* Find(const dom::Element& element) const = 0;
};

// A positioned text run within a line box.
struct TextRun {
  std::string text;
  std::string font_family;  // CSS font-family the run was measured with
  int font_weight = 400;
  bool font_italic = false;
  float x = 0;
  float y = 0;  // top of the run (glyph ascent area)
  float font_size = 16;
  float width = 0;  // measured advance width (real font or monospace fallback)
  css::Color color{0, 0, 0, 255};
  bool underline = false;
  const dom::Element* element = nullptr;  // source element (for hit-testing)
};

// A positioned atomic inline box within a line.  It is either a replaced
// <img> (|image| set) or an inline-block (|block_box| set); the box's width/
// height on the line is |width|/|height|.
struct InlineBox {
  const dom::Element* element = nullptr;
  const image::Image* image = nullptr;
  style::ComputedStyle style;
  float x = 0;
  float y = 0;
  float width = 0;
  float height = 0;
  // Baseline offset from this atomic box's border-box top (for vertical-align:
  // baseline).  For an inline-block this is the baseline of its inner block
  // (last in-flow line box); for a replaced <img> it is its bottom edge.
  float baseline_offset = 0;
  // For an inline-block: the laid-out inner block box (padding/border edges
  // included).  Its origin is local to this atomic box (0,0 = this box's
  // border-box top-left); |x|/|y| give its position on the line.
  std::unique_ptr<LayoutBox> block_box = nullptr;
};

// A line of inline content.
struct Line {
  std::vector<TextRun> runs;
  std::vector<InlineBox> boxes;  // atomic inline boxes (replaced <img>)
  float height = 0;
  float baseline_offset = 0;  // distance from line top to run top
  float baseline = 0;         // distance from line top to the text baseline
};

// A laid-out box.
//
// Coordinates: (x, y) is the border-box top-left; width/height are the
// border-box size.  Margins are stored separately and sit outside the border
// box.  Children and lines are positioned relative to this box's border box.
struct LayoutBox {
  // Owning element (null for anonymous/root handling).
  const dom::Element* element = nullptr;
  style::ComputedStyle style;

  // Decoded image for a replaced <img> box (set by layout from ImageProvider).
  const image::Image* image = nullptr;

  float x = 0;
  float y = 0;
  float width = 0;
  float height = 0;

  float margin_top = 0;
  float margin_right = 0;
  float margin_bottom = 0;
  float margin_left = 0;
  float border_top = 0;
  float border_right = 0;
  float border_bottom = 0;
  float border_left = 0;
  float padding_top = 0;
  float padding_right = 0;
  float padding_bottom = 0;
  float padding_left = 0;

  std::vector<std::unique_ptr<LayoutBox>> children;  // block-level children
  std::vector<std::unique_ptr<LayoutBox>> positioned_children;  // position:absolute/fixed
  std::vector<Line> lines;                           // inline content

  float content_x() const {
    return x + border_left + padding_left;
  }
  float content_y() const {
    return y + border_top + padding_top;
  }
  float content_width() const {
    return width - border_left - border_right - padding_left - padding_right;
  }
  float content_height() const {
    return height - border_top - border_bottom - padding_top - padding_bottom;
  }
};

// Builds the layout tree for a styled document.
//
// The document must have been processed by StyleEngine first (otherwise a
// default style is used for every element).  Viewport width drives the root
// box width; block boxes fill their containing block, inline content wraps at
// word boundaries.  See docs/design/layout.md for the supported scope.
class LayoutEngine {
 public:
  // |styles| must outlive the engine; it holds the computed styles for the
  // documents being laid out.  |registry| (optional) provides real glyph
  // advances (with per-character font fallback) for text measurement; when
  // null a monospace fallback (font_size per character) is used.  |images|
  // (optional) provides decoded images for <img> replaced boxes.
  explicit LayoutEngine(const style::StyleEngine& styles,
                        const graphics::FontRegistry* registry = nullptr,
                        const ImageProvider* images = nullptr)
      : styles_(styles), registry_(registry), images_(images) {}

  std::unique_ptr<LayoutBox> BuildLayoutTree(dom::Document& document, float viewport_width);

 private:
  const style::StyleEngine& styles_;
  const graphics::FontRegistry* registry_ = nullptr;
  const ImageProvider* images_ = nullptr;
};

}  // namespace neko::layout
