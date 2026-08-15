// Human-readable string forms of ComputedStyle enums and SizeSpec, for
// DevTools and debug output.  These are labels, not CSS serialization.

#include "neko/style/computed_style.h"

#include <cstdio>

namespace neko::style {

std::string_view ToString(Display display)
{
  switch (display) {
  case Display::kBlock:
    return "block";
  case Display::kInline:
    return "inline";
  case Display::kInlineBlock:
    return "inline-block";
  case Display::kNone:
    return "none";
  case Display::kFlex:
    return "flex";
  case Display::kInlineFlex:
    return "inline-flex";
  case Display::kGrid:
    return "grid";
  case Display::kTable:
    return "table";
  case Display::kTableRowGroup:
    return "table-row-group";
  case Display::kTableRow:
    return "table-row";
  case Display::kTableCell:
    return "table-cell";
  case Display::kTableCaption:
    return "table-caption";
  }
  return "unknown";
}

std::string_view ToString(Position position)
{
  switch (position) {
  case Position::kStatic:
    return "static";
  case Position::kRelative:
    return "relative";
  case Position::kAbsolute:
    return "absolute";
  case Position::kFixed:
    return "fixed";
  }
  return "unknown";
}

std::string_view ToString(TextAlign align)
{
  switch (align) {
  case TextAlign::kLeft:
    return "left";
  case TextAlign::kCenter:
    return "center";
  case TextAlign::kRight:
    return "right";
  case TextAlign::kJustify:
    return "justify";
  }
  return "unknown";
}

std::string_view ToString(FlexDirection direction)
{
  switch (direction) {
  case FlexDirection::kRow:
    return "row";
  case FlexDirection::kRowReverse:
    return "row-reverse";
  case FlexDirection::kColumn:
    return "column";
  case FlexDirection::kColumnReverse:
    return "column-reverse";
  }
  return "unknown";
}

std::string ToString(const SizeSpec& spec)
{
  if (spec.is_extremum || spec.is_clamp) {
    return spec.is_clamp ? "clamp(...)" : (spec.extremum_is_max ? "max(...)" : "min(...)");
  }
  if (spec.is_calc) {
    return std::to_string(spec.calc.percent) + "% + " + std::to_string(spec.calc.offset) + "px";
  }
  if (spec.percent) {
    return std::to_string(spec.value) + "%";
  }
  return std::to_string(spec.value) + "px";
}

std::string ToString(const css::Color& color)
{
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", color.r, color.g, color.b);
  if (color.a != 255) {
    std::snprintf(buffer + 7, sizeof(buffer) - 7, "%02X", color.a);
    return std::string(buffer);
  }
  return std::string(buffer);
}

} // namespace neko::style
