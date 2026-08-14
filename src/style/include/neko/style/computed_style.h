#pragma once

#include "neko/css/color.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace neko::style {

// Display values.  flex/inline-flex are laid out by the flex layout
// algorithm; grid by the grid layout algorithm.  Table display values are
// laid out by the table layout algorithm.
enum class Display
{
  kBlock,
  kInline,
  kInlineBlock, // inline-level block container (CSS2.2 9.2.2.1)
  kNone,
  kFlex,       // block-level flex container (CSS Flexbox 1)
  kInlineFlex, // inline-level flex container
  kGrid,       // block-level grid container (CSS Grid Layout 1)
  kTable,
  kTableRowGroup, // thead/tbody/tfoot
  kTableRow,      // tr
  kTableCell,     // td/th
  kTableCaption,  // caption
};

// position.  Layout currently honors static and relative; absolute/fixed are
// stored but treated as static (documented limitation).
enum class Position
{
  kStatic,
  kRelative,
  kAbsolute,
  kFixed
};

// float (CSS 2.2 §9.5): drops the box out of normal flow and floats it to one
// side of its containing block; following line boxes wrap around it.
enum class Float
{
  kNone,
  kLeft,
  kRight
};

enum class TextAlign
{
  kLeft,
  kCenter,
  kRight,
  kJustify
};

// object-fit (CSS Images 3 §4.5): how replaced content fits its box.
enum class ObjectFit
{
  kFill,
  kContain,
  kCover,
  kNone,
  kScaleDown
};

// vertical-align (CSS 2.2 §10.8): inline-level box alignment within a line.
enum class VerticalAlign
{
  kBaseline,
  kMiddle,
  kTop,
  kBottom,
  kTextTop,
  kTextBottom
};

enum class BorderStyle
{
  kNone,
  kSolid,
  kDashed,
  kDotted
};

// Flex layout properties (CSS Flexible Box Layout Module Level 1).
enum class FlexDirection
{
  kRow,
  kRowReverse,
  kColumn,
  kColumnReverse
};
enum class FlexWrap
{
  kNoWrap,
  kWrap,
  kWrapReverse
};
enum class JustifyContent
{
  kFlexStart,
  kFlexEnd,
  kCenter,
  kSpaceBetween,
  kSpaceAround,
  kSpaceEvenly,
};
enum class AlignItems
{
  kStretch,
  kFlexStart,
  kFlexEnd,
  kCenter,
  kBaseline
};
enum class AlignContent
{
  kStretch,
  kFlexStart,
  kFlexEnd,
  kCenter,
  kSpaceBetween,
  kSpaceAround,
};

// appearance (CSS-UI-4 §7.2): whether the element keeps a native (UA) widget
// look.  Supported values: none (plain CSS box; author background/border
// apply), auto (native look for elements with a definite appearance —
// currently <button>) and button (force the button look on any element).
// Other compat values (checkbox, radio, textfield, ...) are not implemented;
// their declarations are ignored.
enum class Appearance
{
  kNone,
  kAuto,
  kButton
};

// A length that may be a percentage (resolved against the containing block).
struct SizeSpec
{
  float value = 0;
  bool percent = false;
};

// Grid layout (CSS Grid Layout 1).  One grid track definition.
struct GridTrack
{
  enum class Kind
  {
    kFixed,      // length (px) or percentage
    kFr,         // "1fr" — shares the leftover space
    kAuto,       // sizes to the content of the items in the track
    kMinContent, // sizes to the min-content of the items in the track
    kMaxContent, // sizes to the max-content of the items in the track
  };
  Kind kind = Kind::kAuto;
  float length = 0;  // kFixed: px length
  float percent = 0; // kFixed: percentage (0 = not a percentage)
  float fr = 0;      // kFr: the flex factor
};

// A grid item's placement on one axis (grid-column / grid-row).
struct GridPlacement
{
  enum class Kind
  {
    kAuto, // auto-placement
    kLine, // explicit 1-based line
    kSpan, // spans |span| tracks (end only)
  };
  Kind kind = Kind::kAuto;
  int line = 0; // kLine: 1-based line number
  int span = 1; // kSpan: number of tracks spanned
};

// Fully resolved style for one element (px values unless noted).
struct ComputedStyle
{
  Display display = Display::kInline; // CSS initial value
  Position position = Position::kStatic;
  Float floating = Float::kNone; // float: left/right/none

  std::optional<SizeSpec> width;
  std::optional<SizeSpec> height;

  // min/max constraints (CSS 2.2 §10.4).  Percentages resolve against the
  // containing block like width/height.  nullopt = auto (no constraint).
  std::optional<SizeSpec> min_width;
  std::optional<SizeSpec> max_width;
  std::optional<SizeSpec> min_height;
  std::optional<SizeSpec> max_height;

  // Box insets (percentages resolve against the containing block width).
  // Each margin carries an *auto flag: margin: auto (CSS 2.2 §10.3.4) leaves
  // the resolved value at 0 in normal flow but lets flex layout distribute
  // free space between the auto margins (CSS Flexbox 1 §8.1).
  SizeSpec margin_top;
  SizeSpec margin_right;
  SizeSpec margin_bottom;
  SizeSpec margin_left;
  bool margin_top_auto = false;
  bool margin_right_auto = false;
  bool margin_bottom_auto = false;
  bool margin_left_auto = false;
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

  // Native widget look (CSS-UI-4 §7.2).  Initial value: none.
  Appearance appearance = Appearance::kNone;

  // Replaced content fitting (img).
  ObjectFit object_fit = ObjectFit::kFill;
  VerticalAlign vertical_align = VerticalAlign::kBaseline;

  // Font (inherited).
  float font_size = 16;
  float line_height = 19.2f; // 1.2 * default font-size
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

  // Flex layout (CSS Flexbox 1).  Flex-direction/flex-wrap/justify-content/
  // align-items/align-content/gap live on the container; flex-grow/
  // flex-shrink/flex-basis live on the items (computed per element).
  FlexDirection flex_direction = FlexDirection::kRow;
  FlexWrap flex_wrap = FlexWrap::kNoWrap;
  JustifyContent justify_content = JustifyContent::kFlexStart;
  AlignItems align_items = AlignItems::kStretch;
  AlignContent align_content = AlignContent::kStretch;
  float flex_grow = 0;
  float flex_shrink = 1;
  std::optional<SizeSpec> flex_basis; // nullopt = auto (content-based)
  float row_gap = 0;
  float column_gap = 0;

  // Flex item ordering and per-item cross-axis alignment (CSS Flexbox 1 §5.3
  // and §8.3).  align_self overrides the container's align-items when set.
  int order = 0;
  std::optional<AlignItems> align_self;

  // Grid layout (CSS Grid Layout 1).  Track templates and row/column gaps
  // live on the container; the item's placement lives on the item.  Only the
  // row-major auto flow is supported (grid-auto-flow: row).
  std::vector<GridTrack> grid_template_columns;
  std::vector<GridTrack> grid_template_rows;
  GridPlacement grid_column_start;
  GridPlacement grid_column_end;
  GridPlacement grid_row_start;
  GridPlacement grid_row_end;

  // CSS custom properties (CSS Custom Properties for Cascading Variables
  // Level 1).  Values are the raw declaration text with var() references
  // already resolved.  Custom properties inherit, so the map is copied from
  // the parent and extended by declarations on the element itself.
  std::map<std::string, std::string> custom_properties;
};

} // namespace neko::style
