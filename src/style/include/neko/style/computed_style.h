#pragma once

#include <optional>
#include <string>

#include "neko/css/color.h"

namespace neko::style {

// Display values.  flex/grid are parsed but NOT yet implemented: elements
// declaring them are laid out as blocks (documented limitation).
enum class Display { kBlock, kInline, kNone };

// position.  Layout currently honors static and relative; absolute/fixed are
// stored but treated as static (documented limitation).
enum class Position { kStatic, kRelative, kAbsolute, kFixed };

enum class TextAlign { kLeft, kCenter, kRight, kJustify };

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

  // Font (inherited).
  float font_size = 16;
  float line_height = 19.2f;  // 1.2 * default font-size
  std::string font_family = "sans-serif";
  int font_weight = 400;

  // Text (inherited).
  TextAlign text_align = TextAlign::kLeft;
  std::optional<css::Color> color;
  bool text_decoration_underline = false;

  // Offsets (used by position: relative).
  float left = 0;
  float top = 0;
};

}  // namespace neko::style
