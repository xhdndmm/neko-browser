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

namespace neko::layout {

// A positioned text run within a line box.
struct TextRun {
  std::string text;
  std::string font_family;  // CSS font-family the run was measured with
  float x = 0;
  float y = 0;  // top of the run (glyph ascent area)
  float font_size = 16;
  float width = 0;  // measured advance width (real font or monospace fallback)
  css::Color color{0, 0, 0, 255};
  bool underline = false;
  const dom::Element* element = nullptr;  // source element (for hit-testing)
};

// A line of inline content.
struct Line {
  std::vector<TextRun> runs;
  float height = 0;
  float baseline_offset = 0;  // distance from line top to run top
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
  // null a monospace fallback (font_size per character) is used.
  explicit LayoutEngine(const style::StyleEngine& styles,
                        const graphics::FontRegistry* registry = nullptr)
      : styles_(styles), registry_(registry) {}

  std::unique_ptr<LayoutBox> BuildLayoutTree(dom::Document& document, float viewport_width);

 private:
  const style::StyleEngine& styles_;
  const graphics::FontRegistry* registry_ = nullptr;
};

}  // namespace neko::layout
