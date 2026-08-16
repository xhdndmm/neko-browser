#pragma once

#include "neko/css/color.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neko::style {

// display: table etc. laid out by the table layout algorithm.
enum class Display
{
  kBlock,
  kInline,
  kInlineBlock, // inline-level block container (CSS2.2 9.2.2.1)
  kNone,
  kFlex,       // block-level flex container (CSS Flexbox 1)
  kInlineFlex, // inline-level flex container
  kGrid,       // block-level grid container (CSS Grid Layout 1)
  kInlineGrid, // inline-level grid container
  kTable,
  kTableRowGroup, // thead/tbody/tfoot
  kTableRow,      // tr
  kTableCell,     // td/th
  kTableCaption,  // caption
  kListItem,      // li (CSS2.2 9.2.1.3): block container with a marker box
};

// list-style-type (CSS Lists 3 §4.1): the marker glyph/numbering for a
// display:list-item element.  disc/circle/square are the bullet types for
// <ul>; decimal/lower-alpha/upper-alpha/lower-roman/upper-roman number <ol>
// items.  kNone suppresses the marker.
enum class ListStyleType
{
  kNone,
  kDisc,
  kCircle,
  kSquare,
  kDecimal,
  kLowerAlpha,
  kUpperAlpha,
  kLowerRoman,
  kUpperRoman,
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

// box-sizing (CSS-UI-3 §6): whether a specified width/height (and the
// min/max constraints) refer to the content box or the border box.  Real
// pages almost universally reset `* { box-sizing: border-box }`, so without
// this every sized box is wider than intended and layouts collapse.
enum class BoxSizing
{
  kContentBox,
  kBorderBox,
};

// white-space (CSS Text 3 §3): how whitespace and wrapping behave.  Only
// kNormal (collapse + wrap) and kNowrap (collapse + no wrap) are honored by
// layout; pre/pre-wrap/pre-line are parsed but treated as normal
// (documented limitation — no preformatted text runs).
enum class WhiteSpace
{
  kNormal,
  kNowrap,
  kPre,
  kPreWrap,
  kPreLine,
};

// overflow (CSS Overflow 3): what to do with content that overflows the
// box.  kVisible paints overflow; kHidden/kAuto/kScroll clip it to the
// padding box (no scrollable overflow handling yet — kAuto/kScroll behave
// like kHidden).  Inherits? No.
enum class Overflow
{
  kVisible,
  kHidden,
  kAuto,
  kScroll,
};

// A calc() term: `percent% of the containing block + offset px`.  min()/max()
// and clamp() arguments are also linear combinations of this form.
struct CalcTerm
{
  float offset = 0;  // px component
  float percent = 0; // percentage coefficient
};

// A length that may be a percentage (resolved against the containing block),
// a calc() linear combination, or an extremum (min/max/clamp) over
// linear-combination arguments.  All forms resolve against the containing
// block at layout time (see layout::ResolveSize).
struct SizeSpec
{
  float value = 0;
  bool percent = false;
  // calc(expr): the resolved length is calc.percent% of the containing block
  // plus calc.offset px.
  bool is_calc = false;
  CalcTerm calc;
  // min()/max()/clamp(): an extremum over the linear-combination arguments.
  bool is_extremum = false;
  bool extremum_is_max = false; // max() vs min()
  bool is_clamp = false;        // clamp(MIN, VAL, MAX) = max(MIN, min(VAL, MAX))
  std::vector<CalcTerm> extremum_args;
};

// grid-auto-flow (CSS Grid Layout 1 §7.1): the placement direction for
// auto-placed items, plus the dense backfill flag.
enum class GridAutoFlow
{
  kRow,        // sparse, rows first (initial value)
  kColumn,     // sparse, columns first
  kRowDense,   // dense, rows first
  kColumnDense // dense, columns first
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

  // minmax() bounds (CSS Grid Layout 1 §5.1).  min_size/max_size are length
  // or percentage bounds; the *_content flags mark a bound given as
  // min-content / max-content (resolved against the track's items at layout
  // time).  Layout clamps the resolved track size into [min, max].
  std::optional<SizeSpec> min_size;
  std::optional<SizeSpec> max_size;
  bool min_is_min_content = false;
  bool min_is_max_content = false;
  bool max_is_max_content = false;
};

// A grid item's placement on one axis (grid-column / grid-row).
struct GridPlacement
{
  enum class Kind
  {
    kAuto, // auto-placement
    kLine, // explicit 1-based line (negative counts from the end)
    kSpan, // spans |span| tracks (end only)
  };
  Kind kind = Kind::kAuto;
  int line = 0; // kLine: 1-based line number
  int span = 1; // kSpan: number of tracks spanned
  // Custom-ident reference (CSS Grid Layout 1 §7.3): kLine resolves to the
  // first line with this name (a grid-template-areas name matches the area's
  // edge); kSpan spans to the next line with this name.  Empty = numeric.
  std::string name;
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

  // CSS background-image: the resolved URL (relative URLs resolved against the
  // page base by the browser layer when fetching).  Empty / unset = none.
  // Images are decoded by the browser and injected keyed by the element, the
  // same mechanism as <img>; layout/paint render it over the box's background.
  std::optional<std::string> background_image;

  // aspect-ratio (CSS Box Sizing 4): width / height ratio (e.g. 1 for a
  // square, 16/9 for 16:9).  With a definite width (or height) and the other
  // axis auto, layout derives the auto axis from the ratio.  nullopt = auto.
  std::optional<float> aspect_ratio;

  // border-radius: a single radius applied to all corners (a percentage
  // resolves against the box width at paint time).  nullopt = square corners.
  std::optional<SizeSpec> border_radius;

  // Native widget look (CSS-UI-4 §7.2).  Initial value: none.
  Appearance appearance = Appearance::kNone;

  // box-sizing (CSS-UI-3 §6).  Initial value: content-box.
  BoxSizing box_sizing = BoxSizing::kContentBox;

  // white-space (CSS Text 3 §3).  Initial value: normal.
  WhiteSpace white_space = WhiteSpace::kNormal;

  // overflow (CSS Overflow 3).  Initial value: visible.
  Overflow overflow = Overflow::kVisible;

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

  // List marker (CSS Lists 3 §4.1).  Only meaningful for display:list-item.
  // Inherited, so a <ul>/<ol> sets the marker type for its <li> descendants.
  ListStyleType list_style_type = ListStyleType::kNone;

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
  // live on the container; the item's placement lives on the item.
  std::vector<GridTrack> grid_template_columns;
  std::vector<GridTrack> grid_template_rows;
  // Named grid lines (CSS Grid Layout 1 §7.2): grid_column_lines[i] names
  // the line before track i, so the vector has tracks + 1 entries (an empty
  // name list for anonymous lines).  Implicit <name>-start/<name>-end lines
  // from grid-template-areas are added at layout time.
  std::vector<std::vector<std::string>> grid_column_lines;
  std::vector<std::vector<std::string>> grid_row_lines;
  // grid-template-areas (CSS Grid Layout 1 §7.3): rows of cell names;
  // "." marks an empty cell.  Empty vector = none.
  std::vector<std::vector<std::string>> grid_template_areas;
  GridAutoFlow grid_auto_flow = GridAutoFlow::kRow;
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

// String forms of the style enums / SizeSpec, used by DevTools and debug
// output.  Not CSS serialization — human-readable labels.
std::string_view ToString(Display display);
std::string_view ToString(Position position);
std::string_view ToString(TextAlign align);
std::string_view ToString(FlexDirection direction);
std::string ToString(const SizeSpec& spec);
std::string ToString(const css::Color& color);

} // namespace neko::style
