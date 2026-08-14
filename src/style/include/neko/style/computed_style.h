#pragma once

#include <optional>
#include <string>

#include "neko/css/color.h"

namespace neko::style {

// Display values.  flex/grid are parsed but NOT yet implemented: elements
// declaring them are laid out as blocks (documented limitation).  Table
// display values are laid out by the table layout algorithm.
enum class Display {
  kBlock,
  kInline,
  kInlineBlock,  // inline-level block container (CSS2.2 9.2.2.1)
  kNone,
  kTable,
  kTableRowGroup,  // thead/tbody/tfoot
  kTableRow,       // tr
  kTableCell,      // td/th
  kTableCaption,   // caption
};

// position.  Layout currently honors static and relative; absolute/fixed are
// stored but treated as static (documented limitation).
enum class Position { kStatic, kRelative, kAbsolute, kFixed };

// float (CSS 2.2 §9.5): drops the box out of normal flow and floats it to one
// side of its containing block; following line boxes wrap around it.
enum class Float { kNone, kLeft, kRight };

enum class TextAlign { kLeft, kCenter, kRight, kJustify };

// object-fit (CSS Images 3 §4.5): how replaced content fits its box.
enum class ObjectFit { kFill, kContain, kCover, kNone, kScaleDown };

// vertical-align (CSS 2.2 §10.8): inline-level box alignment within a line.
enum class VerticalAlign { kBaseline, kMiddle, kTop, kBottom, kTextTop, kTextBottom };

enum class BorderStyle { kNone, kSolid, kDashed, kDotted };

// A length that may be a percentage (resolved against the containing block).
struct SizeSpec {
  float value = 0;
  bool percent = false;
};

// Fully resolved style for one element (px values unless noted).
struct ComputedStyle {
  Display display = Display::kInline;  // CSS initial value
  Position position = Position::kStatic;
  Float floating = Float::kNone;  // float: left/right/none

  std::optional<SizeSpec> width;
  std::optional<SizeSpec> height;

  // Box insets (percentages resolve against the containing block width).
  SizeSpec margin_top;
  SizeSpec margin_right;
  SizeSpec margin_bottom;
  SizeSpec margin_left;
  SizeSpec padding_top;
  SizeSpec padding_right;
  SizeSpec padding_bottom;
  SizeSpec padding_left;
  SizeSpec border_top;
  SizeSpec border_right;
  SizeSpec border_bottom;
  SizeSpec border_left;
  BorderStyle border_style = BorderStyle::kNone;
  std::optional<css::Color> border_color;

  std::optional<css::Color> background_color;

  // Replaced content fitting (img).
  ObjectFit object_fit = ObjectFit::kFill;
  VerticalAlign vertical_align = VerticalAlign::kBaseline;

  // Font (inherited).
  float font_size = 16;
  float line_height = 19.2f;  // 1.2 * default font-size
  std::string font_family = "sans-serif";
  int font_weight = 400;
  bool font_italic = false;

  // Text (inherited).
  TextAlign text_align = TextAlign::kLeft;
  std::optional<css::Color> color;
  bool text_decoration_underline = false;

  // Offsets (used by position: relative / absolute).  *_auto records whether
  // the offset was left at its initial value (auto), which absolute
  // positioning must distinguish from an explicit 0.
  float left = 0;
  float top = 0;
  float right = 0;
  float bottom = 0;
  bool left_auto = true;
  bool top_auto = true;
  bool right_auto = true;
  bool bottom_auto = true;
};

}  // namespace neko::style
