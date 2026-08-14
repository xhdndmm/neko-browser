#include "neko/base/utf8.h"
#include "neko/dom/element.h"
#include "neko/graphics/font_registry.h"
#include "neko/graphics/font_selector.h"
#include "neko/image/image.h"
#include "neko/layout/layout_tree.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neko::layout {
namespace {

// Empty float list for block formatting contexts that do not inherit floats
// (inline-block, table cells, floats, absolute boxes start a fresh BFC).
const std::vector<const LayoutBox*> kNoFloats;

// A unit of inline content (a text chunk with its style).
struct InlineItem
{
  std::string text;
  const style::ComputedStyle* style;
  const dom::Element* element; // source element (null for block-level text)
  bool line_break = false;     // <br>: force a line break
  bool atomic = false;         // atomic inline box (replaced <img> / inline-block)
  const image::Image* image = nullptr;
  float width = 0;
  float height = 0;
  float baseline_offset = 0; // baseline from this atomic box's top (for vertical-align)
  // For an inline-block atomic box: its inner block layout.
  std::unique_ptr<LayoutBox> block_box = nullptr;
};

// True when |c| is an ASCII whitespace character used for word breaking.
bool IsWordBreak(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// True when |cp| is an East Asian ideograph (CJK).  Such characters may break
// between any two of them (CSS text: "CJK text can be broken anywhere"), so a
// long CJK run can wrap inside a narrowed (float-avoiding) line.
bool IsCjkCodePoint(char32_t cp)
{
  return (cp >= 0x2E80 && cp <= 0x2FFF) || // CJK radicals / Kangxi
         (cp >= 0x3000 && cp <= 0x303F) || // CJK punctuation
         (cp >= 0x3040 && cp <= 0x30FF) || // Hiragana / Katakana
         (cp >= 0x3400 && cp <= 0x4DBF) || // CJK ext A
         (cp >= 0x4E00 && cp <= 0x9FFF) || // CJK unified
         (cp >= 0xF900 && cp <= 0xFAFF) || // CJK compat
         (cp >= 0xFE30 && cp <= 0xFE4F) || // CJK compat forms
         (cp >= 0xFF00 && cp <= 0xFFEF) || // fullwidth forms (，！？：；etc.)
         (cp >= 0x20000 && cp <= 0x2FA1F); // CJK ext B..F
}

// Measures the advance width of |text| at |font_size| using the font selector
// for |family| when a registry is available; falls back to the monospace model
// (font_size per character).
float MeasureTextWidth(const graphics::FontRegistry* registry,
                       std::string_view family,
                       int weight,
                       bool italic,
                       std::string_view text,
                       float font_size)
{
  if (registry == nullptr) {
    return static_cast<float>(text.size()) * font_size;
  }
  return registry->SelectorFor(std::string(family), weight, italic)->TextWidth(text, font_size);
}

// Width of the widest space-separated word in |text| (the min-content width).
float WidestWordWidth(const graphics::FontRegistry* registry,
                      std::string_view family,
                      int weight,
                      bool italic,
                      std::string_view text,
                      float font_size)
{
  float widest = 0;
  std::size_t start = 0;
  while (start < text.size()) {
    while (start < text.size() && IsWordBreak(text[start])) {
      ++start;
    }
    if (start >= text.size()) {
      break;
    }
    const std::size_t end = text.find_first_of(" \t\n\r", start);
    const std::string_view word =
        text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
    widest = std::max(widest, MeasureTextWidth(registry, family, weight, italic, word, font_size));
    start = end == std::string_view::npos ? text.size() : end;
  }
  return widest;
}

// Collapses a whitespace run per CSS white-space: normal: leading/trailing
// whitespace is dropped and interior runs collapse to a single space.  Used by
// intrinsic width measurement so source indentation does not inflate width.
std::string CollapseWhitespace(std::string_view text)
{
  std::string out;
  bool pending_space = false;
  bool started = false;
  for (const char c : text) {
    if (IsWordBreak(c)) {
      if (started) {
        pending_space = true;
      }
      continue;
    }
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    out.push_back(c);
    started = true;
  }
  return out;
}

float ResolveSize(const style::SizeSpec& spec, float containing)
{
  if (spec.percent) {
    return containing * spec.value / 100.0f;
  }
  return spec.value;
}

void CollectText(std::string_view text,
                 const style::ComputedStyle& style,
                 const dom::Element* element,
                 std::vector<InlineItem>& items)
{
  if (!text.empty()) {
    items.push_back(InlineItem{std::string(text), &style, element});
  }
}

// Computes the content-box size of a replaced <img> (CSS Images 3 §4.3):
// explicit CSS width/height, else the presentational width/height attributes,
// else intrinsic, preserving the aspect ratio when only one axis is given.
// Defined after ParseNonNegativeInt; declared here for CollectInline.
void ComputeReplacedSize(const style::ComputedStyle& style,
                         const dom::Element& element,
                         const image::Image* img,
                         float containing_width,
                         float& out_w,
                         float& out_h);

// CollectInline is implemented as a Builder method (see BuildLayoutTree) so it
// can lay out nested inline-blocks on demand; see the Builder struct below.

// Defined below; used by LayoutLines when placing an inline-block's inner box.
void TranslateBox(LayoutBox& box, float dx, float dy);

// Breaks inline items into wrapped lines and fills |out_lines|.  Positions are
// relative to the box (origin_x/origin_y are the content box origin).  Word
// widths come from |registry| when provided (real advances with per-character
// font fallback); otherwise the monospace fallback is used.  |container_style|
// supplies the imaginary-strut font metrics and line-height (CSS2.2 10.8).
void LayoutLines(std::vector<InlineItem>& items,
                 float available_width,
                 float origin_x,
                 float origin_y,
                 const graphics::FontRegistry* registry,
                 const style::ComputedStyle& container_style,
                 const std::vector<const LayoutBox*>& floats,
                 std::vector<Line>& out_lines,
                 float& total_height)
{
  Line line;
  float x = 0;
  float line_top = 0;

  // Current line start (left edge offset due to a left float) and usable
  // width (reduced by floats overlapping the line's vertical extent).
  float line_left = 0;
  float line_right = available_width;
  float line_width = available_width;
  // Recompute the line's horizontal bounds for the vertical extent
  // [line_top, line_top + line.height-so-far].  Left floats push the left edge
  // right; right floats push the right edge left; a line at the same height as
  // a float wraps around it (CSS2.2 9.5).
  auto recompute_line_bounds = [&]() {
    // A line box at least fills the strut line-height even before any run
    // contributes height, so overlap with a float is judged against that.
    const float line_y2 = line_top + std::max(line.height, container_style.line_height);
    float left = 0;
    float right = available_width;
    for (const auto& f : floats) {
      // float coordinates are absolute; bring them into the line's local
      // (origin_*) coordinate space.
      const float fy = f->y - origin_y;
      const float fy2 = fy + f->height;
      if (line_top < fy2 && line_y2 > fy) {
        const float fx = f->x - origin_x;
        const float fx2 = fx + f->width;
        if (f->style.floating == style::Float::kLeft) {
          left = std::max(left, fx2);
        } else {
          right = std::min(right, fx);
        }
      }
    }
    line_left = left;
    line_right = right;
    line_width = std::max(0.0f, right - left);
  };

  // Advances the line below every float that currently overlaps it, so an
  // item that cannot fit in the squeezed line at its start can be placed on a
  // full-width line below the floats (CSS2.2 9.5: a float does not overlap an
  // in-flow line box).  Returns true if the line was moved.
  auto push_past_floats = [&]() -> bool {
    bool moved = false;
    for (;;) {
      recompute_line_bounds();
      float bottom_max = 0;
      bool pushed = false;
      for (const auto& f : floats) {
        const float fy = f->y - origin_y;
        const float fy2 = fy + f->height;
        if (line_top < fy2) {
          bottom_max = std::max(bottom_max, fy2);
          pushed = true;
        }
      }
      if (!pushed || bottom_max <= line_top) {
        break;
      }
      line_top = bottom_max;
      moved = true;
    }
    recompute_line_bounds();
    x = line_left;
    return moved;
  };
  recompute_line_bounds();
  x = line_left;

  // Strut ascent/descent (baseline offsets) of the block container's font.
  const auto strut_metrics = [&](float& asc, float& desc) {
    if (registry != nullptr) {
      const graphics::FontSelector* sel = registry->SelectorFor(
          container_style.font_family, container_style.font_weight, container_style.font_italic);
      if (sel != nullptr) {
        asc = sel->Ascent(container_style.font_size);
        desc = sel->Descent(container_style.font_size);
        if (asc + desc > 0.0f) {
          return;
        }
      }
    }
    asc = container_style.font_size * 0.8f;
    desc = container_style.font_size * 0.2f;
  };
  float strut_asc = 0, strut_desc = 0;
  strut_metrics(strut_asc, strut_desc);
  const float half_leading =
      std::max(0.0f, (container_style.line_height - (strut_asc + strut_desc)) / 2.0f);
  const float strut_up = strut_asc + half_leading;    // baseline to line top
  const float strut_down = strut_desc + half_leading; // baseline to line bottom

  // Per-run metrics (ascent above baseline, descent below).
  const auto run_metrics = [&](const TextRun& run, float& asc, float& desc) {
    if (registry != nullptr) {
      const graphics::FontSelector* sel =
          registry->SelectorFor(run.font_family, run.font_weight, run.font_italic);
      if (sel != nullptr) {
        asc = sel->Ascent(run.font_size);
        desc = sel->Descent(run.font_size);
        if (asc + desc > 0.0f) {
          return;
        }
      }
    }
    asc = run.font_size * 0.8f;
    desc = run.font_size * 0.2f;
  };

  auto flush_line = [&]() {
    if (line.runs.empty() && line.boxes.empty() && line.height <= 0) {
      // Nothing to emit: no content and no height (e.g. leading whitespace).
      return;
    }

    // Baseline offset (line top to baseline) is set by the strut and any
    // baseline-aligned participant; descent offset (baseline to line bottom)
    // by the strut.  top / bottom / middle boxes only demand that the line be
    // tall enough (or, for middle, symmetrically place its centre on the line
    // centre), never distorting the baseline.
    float baseline_off = strut_up;
    float descent_off = strut_down;
    for (const TextRun& run : line.runs) {
      float asc = 0, desc = 0;
      run_metrics(run, asc, desc);
      baseline_off = std::max(baseline_off, asc);
      descent_off = std::max(descent_off, desc);
    }
    for (const InlineBox& b : line.boxes) {
      if (b.style.vertical_align == style::VerticalAlign::kBaseline) {
        baseline_off = std::max(baseline_off, b.baseline_offset);
        descent_off = std::max(descent_off, b.height - b.baseline_offset);
      }
    }

    // When a line holds baseline-aligned inline-blocks, preserve the strut's
    // "line-height: normal" leading below the baseline so wrapped inline-block
    // rows keep a small visible gap (browsers show ~1-2px at 16px) instead of
    // touching.  The gap is separate from the box's own height, which occupies
    // the baseline span.
    bool has_baseline_box = false;
    for (const InlineBox& b : line.boxes) {
      if (b.style.vertical_align == style::VerticalAlign::kBaseline) {
        has_baseline_box = true;
        break;
      }
    }
    if (has_baseline_box) {
      const float leading_gap = container_style.line_height - container_style.font_size;
      descent_off = std::max(descent_off, strut_desc + leading_gap);
    }

    // The line height is at least the baseline span; top / bottom boxes fit
    // because the line is at least as tall as they are.
    float row_height = baseline_off + descent_off;
    for (const InlineBox& b : line.boxes) {
      switch (b.style.vertical_align) {
      case style::VerticalAlign::kTop:
      case style::VerticalAlign::kTextTop:
      case style::VerticalAlign::kBottom:
      case style::VerticalAlign::kTextBottom:
        row_height = std::max(row_height, b.height);
        break;
      case style::VerticalAlign::kMiddle:
        baseline_off = std::max(baseline_off, b.height / 2.0f);
        descent_off = std::max(descent_off, b.height / 2.0f);
        row_height = std::max(row_height, baseline_off + descent_off);
        break;
      case style::VerticalAlign::kBaseline:
      default:
        break;
      }
    }
    line.height = row_height;

    // Baseline absolute within the line.
    const float baseline = origin_y + line_top + baseline_off;
    line.baseline = baseline;
    // Position text runs: their bottom sits on the baseline.
    for (TextRun& run : line.runs) {
      float asc = 0, desc = 0;
      run_metrics(run, asc, desc);
      run.y = baseline - asc;
      run.x = origin_x + run.x;
    }

    // Position atomic inline boxes per vertical-align.
    const float line_top_abs = origin_y + line_top;
    const float line_bottom_abs = line_top_abs + line.height;
    for (InlineBox& b : line.boxes) {
      float y = 0;
      switch (b.style.vertical_align) {
      case style::VerticalAlign::kTop:
      case style::VerticalAlign::kTextTop:
        y = line_top_abs;
        break;
      case style::VerticalAlign::kBottom:
      case style::VerticalAlign::kTextBottom:
        y = line_bottom_abs - b.height;
        break;
      case style::VerticalAlign::kMiddle:
        y = line_top_abs + (line.height - b.height) / 2.0f;
        break;
      case style::VerticalAlign::kBaseline:
      default:
        y = baseline - b.baseline_offset; // its baseline sits on the line baseline
        break;
      }
      b.y = y;
      b.x = origin_x + b.x;
      // The inline-block's inner block was laid out at a local origin; move it
      // into the line's coordinate space with the positioned atomic box.
      if (b.block_box != nullptr) {
        TranslateBox(*b.block_box, b.x, b.y);
      }
    }
    out_lines.push_back(std::move(line));
    line = Line{};
    line_top += out_lines.back().height;
    // Track the actual line bottom (not just the sum of heights): a line that
    // was pushed below a float (via push_past_floats) advances line_top by the
    // float's height, which must count toward the container's content height.
    total_height = line_top;
    recompute_line_bounds();
    x = line_left;
  };

  auto add_word = [&](std::string_view word,
                      const style::ComputedStyle& style,
                      const dom::Element* element,
                      const graphics::FontSelector* selector) {
    const float word_width = selector != nullptr
                                 ? selector->TextWidth(word, style.font_size)
                                 : static_cast<float>(word.size()) * style.font_size;
    // Ensure the word fits on the current (or a lower) line; a word that
    // cannot fit even at a line start is pushed below any overlapping floats.
    // An item wider than the block itself is placed as-is (never dropped).
    while (x + word_width > line_right) {
      const float prev_top = line_top;
      if (x > line_left) {
        flush_line(); // normal wrap to a new line
      } else if (!push_past_floats()) {
        break; // no float to move past: place the (over-wide) word
      }
      if (line_top == prev_top) {
        break; // safety: nothing advanced (e.g. an empty line flush)
      }
    }
    TextRun run;
    run.text = std::string(word);
    run.font_family = style.font_family;
    run.font_weight = style.font_weight;
    run.font_italic = style.font_italic;
    run.x = x;
    run.font_size = style.font_size;
    run.width = word_width;
    run.color = style.color.value_or(css::Color{0, 0, 0, 255});
    run.underline = style.text_decoration_underline;
    run.element = element;
    line.runs.push_back(std::move(run));
    line.height = std::max(line.height, style.line_height);
    x += word_width;
  };

  for (InlineItem& item : items) {
    const style::ComputedStyle& style = *item.style;
    if (item.line_break) {
      // <br>: end the current line (content or a previous empty break line),
      // then start a new empty line that carries the break's line height.
      if (!line.runs.empty() || line.height > 0) {
        flush_line();
      }
      line.height = std::max(line.height, style.line_height);
      continue;
    }
    if (item.atomic) {
      // Atomic inline box (<img> or inline-block): an indivisible box flowing
      // on the current line.
      while (x + item.width > line_right) {
        const float prev_top = line_top;
        if (x > line_left) {
          flush_line();
        } else if (!push_past_floats()) {
          break;
        }
        if (line_top == prev_top) {
          break; // safety: nothing advanced
        }
      }
      InlineBox box;
      box.element = item.element;
      box.image = item.image;
      box.style = style;
      box.x = x;
      box.width = item.width;
      box.height = item.height;
      box.baseline_offset = item.baseline_offset;
      box.block_box = std::move(item.block_box);
      line.boxes.push_back(std::move(box));
      line.height = std::max(line.height, item.height);
      x += item.width;
      continue;
    }
    const graphics::FontSelector* selector =
        registry != nullptr
            ? registry->SelectorFor(style.font_family, style.font_weight, style.font_italic)
            : nullptr;
    std::size_t start = 0;
    const std::string& text = item.text;
    while (start < text.size()) {
      // Collapse a whitespace run (CSS white-space: normal) into a single
      // space; leading whitespace at a line start collapses to nothing (the
      // HTML source indentation around an <img> must not shift it).
      if (IsWordBreak(text[start])) {
        while (start < text.size() && IsWordBreak(text[start])) {
          ++start;
        }
        const bool at_line_start = x == line_left && line.runs.empty() && line.boxes.empty();
        if (!at_line_start) {
          const float space_width = selector != nullptr ? selector->Advance(' ', style.font_size)
                                                        : style.font_size * 0.5f;
          if (x + space_width > line_right && x > line_left) {
            flush_line();
          }
          x += space_width;
        }
      }
      if (start >= text.size()) {
        break;
      }
      // A word is a run up to the next whitespace, except that CJK characters
      // break anywhere: each CJK character is its own word so a long CJK run
      // can wrap inside a narrowed (float-avoiding) line.
      std::size_t end;
      {
        std::size_t pos = start;
        char32_t cp = 0;
        if (base::DecodeUtf8Next(text, pos, cp) && IsCjkCodePoint(cp)) {
          end = pos; // this single CJK character is one word
        } else {
          end = text.find_first_of(" \t\n\r", start);
          end = end == std::string::npos ? text.size() : end;
        }
      }
      const std::string_view word = std::string_view(text).substr(start, end - start);
      add_word(word, style, item.element, selector);
      start = end;
    }
  }
  flush_line();
}

// ---------------------------------------------------------------------------
// Table layout support
// ---------------------------------------------------------------------------

// Intrinsic content width of a box (CSS2.1 "preferred width").
struct IntrinsicWidths
{
  float min = 0; // widest unbreakable unit (word / replaced element)
  float max = 0; // widest line when laid out on a single line
};

// An element's left+right padding and border (added to intrinsic content).
// Measures the min/max intrinsic content width of |element|, recursing through
// the inline + block content model.  Replaced elements (img/input/...) use
// their explicit width (zero when auto).  Percentages, floats and positioning
// are out of scope for this measurement.  Text is measured through |registry|
// (real advances with per-character fallback) when provided, else with the
// monospace fallback.
IntrinsicWidths MeasureContent(const dom::Element& element,
                               const style::StyleEngine& styles,
                               const graphics::FontRegistry* registry)
{
  const style::ComputedStyle& style = styles.StyleFor(element);
  const bool replaced = element.tag_name() == "img" || element.tag_name() == "input" ||
                        element.tag_name() == "textarea" || element.tag_name() == "select";
  if (replaced) {
    IntrinsicWidths w;
    if (style.width.has_value() && !style.width.value().percent) {
      w.min = w.max = style.width.value().value;
    }
    return w;
  }

  IntrinsicWidths out;
  float line_min = 0;
  float line_max = 0;
  auto flush_line = [&]() {
    out.min = std::max(out.min, line_min);
    out.max = std::max(out.max, line_max);
    line_min = 0;
    line_max = 0;
  };

  for (const dom::Node* child : element.ChildNodes()) {
    if (child->node_type() == dom::NodeType::kText) {
      const std::string& text = static_cast<const dom::Text&>(*child).data();
      line_min = std::max(line_min,
                          WidestWordWidth(registry,
                                          style.font_family,
                                          style.font_weight,
                                          style.font_italic,
                                          text,
                                          style.font_size));
      line_max += MeasureTextWidth(registry,
                                   style.font_family,
                                   style.font_weight,
                                   style.font_italic,
                                   CollapseWhitespace(text),
                                   style.font_size);
      continue;
    }
    if (child->node_type() != dom::NodeType::kElement) {
      continue;
    }
    const dom::Element& child_el = static_cast<const dom::Element&>(*child);
    const style::ComputedStyle& child_style = styles.StyleFor(child_el);
    if (child_style.display == style::Display::kNone) {
      continue;
    }
    if (child_el.tag_name() == "br") {
      // <br> ends the current line: max-content is the widest segment.
      flush_line();
      continue;
    }
    const bool block_level = child_style.display == style::Display::kBlock ||
                             child_style.display == style::Display::kTable ||
                             child_style.display == style::Display::kFlex ||
                             child_style.display == style::Display::kTableRowGroup ||
                             child_style.display == style::Display::kTableRow ||
                             child_style.display == style::Display::kTableCell ||
                             child_style.display == style::Display::kTableCaption;
    if (block_level) {
      // Block-level content starts a new line.
      flush_line();
      const IntrinsicWidths cw = MeasureContent(child_el, styles, registry);
      out.min = std::max(out.min, cw.min);
      out.max = std::max(out.max, cw.max);
    } else {
      // Inline content flows onto the current line.
      const IntrinsicWidths cw = MeasureContent(child_el, styles, registry);
      line_min = std::max(line_min, cw.min);
      line_max += cw.max;
    }
  }
  flush_line();
  return out;
}

// One table cell (td/th) with its grid coordinates and laid-out box.
struct CellInfo
{
  dom::Element* element = nullptr;
  style::ComputedStyle style;
  int row = 0;
  int col = 0;
  int colspan = 1;
  int rowspan = 1;
  bool grows_downward = false; // rowspan="0": spans to the end of the row group
  float min_width = 0;
  float max_width = 0;
  std::unique_ptr<LayoutBox> box;
};

bool IsAsciiWhitespace(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

// Parses an HTML attribute value as a "non-negative integer" (WHATWG HTML
// common-microsyntaxes): leading ASCII whitespace is skipped, then a run of
// digits is read (trailing non-digits are ignored); anything else before the
// first digit is an error.  Returns nullopt when the attribute is absent or the
// value is not a valid non-negative integer.
std::optional<std::int64_t> ParseNonNegativeInt(const dom::Element& element, std::string_view name)
{
  const std::optional<std::string_view> attr = element.GetAttribute(name);
  if (!attr.has_value()) {
    return std::nullopt;
  }
  const std::string_view value = *attr;
  std::size_t i = 0;
  while (i < value.size() && IsAsciiWhitespace(value[i])) {
    ++i;
  }
  if (i >= value.size() || value[i] < '0' || value[i] > '9') {
    return std::nullopt;
  }
  std::int64_t result = 0;
  for (; i < value.size(); ++i) {
    const char c = value[i];
    if (c < '0' || c > '9') {
      break;
    }
    result = result * 10 + (c - '0');
    if (result > 65535) {
      return 65535; // callers clamp to <= 1000 / <= 65534 anyway
    }
  }
  return result;
}

// Resolves a colspan attribute per the table model: parse as a non-negative
// integer; zero / failure / absent yields 1; values above 1000 clamp to 1000.
int ResolveColspan(const dom::Element& element)
{
  const std::optional<std::int64_t> v = ParseNonNegativeInt(element, "colspan");
  if (!v.has_value() || *v == 0) {
    return 1;
  }
  return static_cast<int>(std::min<std::int64_t>(*v, 1000));
}

// Resolves a rowspan attribute per the table model: parse as a non-negative
// integer; failure / absent yields 1; values above 65534 clamp to 65534.  Sets
// |grows_downward| when the value is zero, meaning the cell spans the remaining
// rows of its row group.
int ResolveRowspan(const dom::Element& element, bool& grows_downward)
{
  const std::optional<std::int64_t> v = ParseNonNegativeInt(element, "rowspan");
  grows_downward = false;
  if (!v.has_value()) {
    return 1;
  }
  if (*v == 0) {
    grows_downward = true;
    return 1;
  }
  return static_cast<int>(std::min<std::int64_t>(*v, 65534));
}

void ComputeReplacedSize(const style::ComputedStyle& style,
                         const dom::Element& element,
                         const image::Image* img,
                         float containing_width,
                         float& out_w,
                         float& out_h)
{
  const float intrinsic_w = img != nullptr ? static_cast<float>(img->width) : 0.0f;
  const float intrinsic_h = img != nullptr ? static_cast<float>(img->height) : 0.0f;

  // Specified size: CSS width/height win over the width/height attributes
  // (presentational hints, HTML spec).
  std::optional<float> spec_w;
  std::optional<float> spec_h;
  if (style.width.has_value()) {
    spec_w = ResolveSize(style.width.value(), containing_width);
  } else if (const std::optional<std::int64_t> attr = ParseNonNegativeInt(element, "width")) {
    spec_w = static_cast<float>(attr.value());
  }
  if (style.height.has_value()) {
    spec_h = ResolveSize(style.height.value(), containing_width);
  } else if (const std::optional<std::int64_t> attr = ParseNonNegativeInt(element, "height")) {
    spec_h = static_cast<float>(attr.value());
  }

  if (spec_w.has_value() && spec_h.has_value()) {
    out_w = spec_w.value();
    out_h = spec_h.value();
  } else if (spec_w.has_value()) {
    out_w = spec_w.value();
    out_h = intrinsic_w > 0 ? out_w * intrinsic_h / intrinsic_w : 0.0f;
  } else if (spec_h.has_value()) {
    out_h = spec_h.value();
    out_w = intrinsic_h > 0 ? out_h * intrinsic_w / intrinsic_h : 0.0f;
  } else {
    out_w = intrinsic_w;
    out_h = intrinsic_h;
  }
}

// Shifts a laid-out box (and its descendants and text runs) by (dx, dy).  Used
// to move a cell's content from its local (0,0) origin into its grid slot.
void TranslateBox(LayoutBox& box, float dx, float dy)
{
  box.x += dx;
  box.y += dy;
  for (Line& line : box.lines) {
    for (TextRun& run : line.runs) {
      run.x += dx;
      run.y += dy;
    }
    for (InlineBox& inline_box : line.boxes) {
      inline_box.x += dx;
      inline_box.y += dy;
      if (inline_box.block_box != nullptr) {
        TranslateBox(*inline_box.block_box, dx, dy);
      }
    }
  }
  for (auto& child : box.children) {
    TranslateBox(*child, dx, dy);
  }
  for (auto& child : box.positioned_children) {
    TranslateBox(*child, dx, dy);
  }
  for (auto& child : box.floats) {
    TranslateBox(*child, dx, dy);
  }
}

// Resolves per-column content widths for a table.  Columns carrying a cell
// with an explicit width are fixed; the remaining table width is distributed
// across auto columns proportionally to their measured max-content width.
std::vector<float>
ComputeColumnWidths(const std::vector<CellInfo>& cells, int ncols, float table_width)
{
  const std::size_t n = static_cast<std::size_t>(std::max(ncols, 0));
  std::vector<float> fixed(n, -1.0f);
  std::vector<float> auto_max(n, 0.0f);
  for (const CellInfo& cell : cells) {
    if (cell.colspan != 1) {
      continue; // a spanning cell's width is the sum of its columns
    }
    const std::size_t c = static_cast<std::size_t>(cell.col);
    if (cell.style.width.has_value()) {
      const float w = cell.style.width.value().percent
                          ? cell.style.width.value().value / 100.0f * table_width
                          : cell.style.width.value().value;
      fixed[c] = std::max(fixed[c], w);
    } else {
      auto_max[c] = std::max(auto_max[c], cell.max_width);
    }
  }
  float fixed_sum = 0;
  for (const float w : fixed) {
    if (w >= 0) {
      fixed_sum += w;
    }
  }
  const float remainder = std::max(0.0f, table_width - fixed_sum);
  float auto_total = 0;
  std::size_t auto_count = 0;
  for (std::size_t c = 0; c < n; ++c) {
    if (fixed[c] < 0) {
      auto_total += auto_max[c];
      ++auto_count;
    }
  }
  std::vector<float> widths(n, 0.0f);
  for (std::size_t c = 0; c < n; ++c) {
    if (fixed[c] >= 0) {
      widths[c] = fixed[c];
    } else if (auto_count > 0) {
      widths[c] = auto_total > 0 ? remainder * auto_max[c] / auto_total
                                 : remainder / static_cast<float>(auto_count);
    }
  }
  return widths;
}

// Sums |count| column widths starting at |start| (clamped to the column set).
float SumColumns(const std::vector<float>& widths, int start, int count)
{
  float sum = 0;
  const std::size_t begin = static_cast<std::size_t>(std::max(start, 0));
  const std::size_t end =
      std::min(begin + static_cast<std::size_t>(std::max(count, 0)), widths.size());
  for (std::size_t i = begin; i < end; ++i) {
    sum += widths[i];
  }
  return sum;
}

} // namespace

std::unique_ptr<LayoutBox> LayoutEngine::BuildLayoutTree(dom::Document& document,
                                                         float viewport_width)
{
  // Coordinates are absolute (viewport space).  BuildBlock lays out |element|
  // inside a containing block whose content box starts at |origin_x|/|origin_y|.
  struct Builder
  {
    const style::StyleEngine& styles;
    const graphics::FontRegistry* registry; // may be null (monospace fallback)
    const ImageProvider* images;            // may be null (no decoded images)

    // Resolves the box-model edges (margins, borders, padding) of |box|.
    void ResolveBoxEdges(LayoutBox& box, float containing_width)
    {
      box.margin_top = ResolveSize(box.style.margin_top, containing_width);
      box.margin_right = ResolveSize(box.style.margin_right, containing_width);
      box.margin_bottom = ResolveSize(box.style.margin_bottom, containing_width);
      box.margin_left = ResolveSize(box.style.margin_left, containing_width);
      box.border_top = ResolveSize(box.style.border_top, containing_width);
      box.border_right = ResolveSize(box.style.border_right, containing_width);
      box.border_bottom = ResolveSize(box.style.border_bottom, containing_width);
      box.border_left = ResolveSize(box.style.border_left, containing_width);
      box.padding_top = ResolveSize(box.style.padding_top, containing_width);
      box.padding_right = ResolveSize(box.style.padding_right, containing_width);
      box.padding_bottom = ResolveSize(box.style.padding_bottom, containing_width);
      box.padding_left = ResolveSize(box.style.padding_left, containing_width);
    }

    // Lays out an inline-block (display:inline-block, CSS2.2 9.2.2.1) as an
    // atomic block-level box at a local origin.  Width is the explicit value,
    // else shrink-to-fit min(max(min-content, available), preferred)
    // (CSS2.2 10.3.9), with the containing block width used as "available".
    // The box is positioned at its margin edge (border-box top-left at x=y=0
    // plus margins), so its four borders stay on-screen.
    std::unique_ptr<LayoutBox> BuildInlineBlock(dom::Element& element, float containing_width)
    {
      auto box = std::make_unique<LayoutBox>();
      box->element = &element;
      box->style = styles.StyleFor(element);
      ResolveBoxEdges(*box, containing_width);

      float content_width;
      if (box->style.width.has_value()) {
        content_width = ResolveSize(box->style.width.value(), containing_width);
      } else {
        const float extras = box->margin_left + box->margin_right + box->border_left +
                             box->border_right + box->padding_left + box->padding_right;
        const float available = std::max(0.0f, containing_width - extras);
        const IntrinsicWidths w = MeasureContent(element, styles, registry);
        content_width = std::min(std::max(w.min, available), w.max);
      }
      box->width = content_width + box->border_left + box->border_right + box->padding_left +
                   box->padding_right;

      // Border-box origin at the margin edge; content is placed at
      // content_x()/content_y() = +border+padding.
      box->x = box->margin_left;
      box->y = box->margin_top;

      const float avail_width = box->width - box->border_left - box->border_right -
                                box->padding_left - box->padding_right;
      std::vector<dom::Element*> absolute_children;
      float content_height = LayoutBlockContent(
          *box, element, avail_width, 0, 0, avail_width, 0, kNoFloats, absolute_children);
      if (box->style.height.has_value() && !box->style.height.value().percent) {
        content_height = box->style.height.value().value; // specified height wins (10.6.2)
      }
      box->height = content_height + box->border_top + box->border_bottom + box->padding_top +
                    box->padding_bottom;
      const float child_cb_h = box->height - box->border_top - box->border_bottom;
      for (dom::Element* child : absolute_children) {
        box->positioned_children.push_back(
            BuildAbsolute(*child, 0, 0, avail_width, child_cb_h, kNoFloats));
      }
      return box;
    }

    // Lays out a float (float:left/right, CSS2.2 5.5) as an out-of-flow box at
    // the given vertical position |float_y| within the containing block |
    // |containing_width| wide.  Width is the explicit value, else shrink-to-fit
    // (CSS2.2 10.3.5).  A left float hugs the left edge; a right float the
    // right edge.  Line boxes later wrap around it.
    std::unique_ptr<LayoutBox>
    BuildFloat(dom::Element& element, float containing_width, float left_edge, float float_y)
    {
      auto box = std::make_unique<LayoutBox>();
      box->element = &element;
      box->style = styles.StyleFor(element);
      ResolveBoxEdges(*box, containing_width);

      float content_width;
      if (box->style.width.has_value()) {
        content_width = ResolveSize(box->style.width.value(), containing_width);
      } else {
        const float extras = box->margin_left + box->margin_right + box->border_left +
                             box->border_right + box->padding_left + box->padding_right;
        const float available = std::max(0.0f, containing_width - extras);
        const IntrinsicWidths w = MeasureContent(element, styles, registry);
        content_width = std::min(std::max(w.min, available), w.max);
      }
      box->width = content_width + box->border_left + box->border_right + box->padding_left +
                   box->padding_right;

      // Horizontal placement: left float at the containing block's left edge,
      // right float at the right edge (aligned so its right margin box meets
      // the right edge).  Set x/y before laying out content so the inner
      // lines/runs are positioned at their final absolute coordinates.
      if (box->style.floating == style::Float::kLeft) {
        box->x = left_edge + box->margin_left - box->border_left - box->padding_left;
      } else {
        box->x = left_edge + containing_width - box->margin_left - box->margin_right - box->width;
      }
      box->y = float_y + box->margin_top - box->border_top - box->padding_top;

      const float avail_width = box->width - box->border_left - box->border_right -
                                box->padding_left - box->padding_right;
      std::vector<dom::Element*> absolute_children;
      float content_height = LayoutBlockContent(*box,
                                                element,
                                                avail_width,
                                                box->border_left,
                                                box->border_top,
                                                avail_width,
                                                0,
                                                kNoFloats,
                                                absolute_children);
      if (box->style.height.has_value() && !box->style.height.value().percent) {
        content_height = box->style.height.value().value;
      }
      box->height = content_height + box->border_top + box->border_bottom + box->padding_top +
                    box->padding_bottom;
      return box;
    }

    // Collects inline content under |node| into |items|: text, <br>, atomic
    // replaced <img> boxes, inline-blocks (atomic block boxes), and inline
    // element recursion.  |style|/|element| apply to |node|'s text children.
    // |containing_width| is the block container's content width.
    void CollectInline(dom::Node& node,
                       const style::ComputedStyle& style,
                       const dom::Element* element,
                       float containing_width,
                       std::vector<InlineItem>& items)
    {
      if (node.node_type() == dom::NodeType::kText) {
        CollectText(static_cast<const dom::Text&>(node).data(), style, element, items);
        return;
      }
      if (node.node_type() != dom::NodeType::kElement) {
        return;
      }
      dom::Element& child_element = static_cast<dom::Element&>(node);
      const style::ComputedStyle& child_style = styles.StyleFor(child_element);
      if (child_element.tag_name() == "br") {
        items.push_back(InlineItem{{}, &child_style, &child_element, /*line_break=*/true});
        return;
      }
      if (child_element.tag_name() == "img") {
        const image::Image* img = images != nullptr ? images->Find(child_element) : nullptr;
        float w = 0;
        float h = 0;
        ComputeReplacedSize(child_style, child_element, img, containing_width, w, h);
        // A replaced <img>'s baseline is its bottom edge.
        items.push_back(InlineItem{{},
                                   &child_style,
                                   &child_element,
                                   /*line_break=*/false,
                                   /*atomic=*/true,
                                   img,
                                   w,
                                   h,
                                   /*baseline_offset=*/h});
        return;
      }
      if (child_style.display == style::Display::kInlineBlock &&
          child_style.position == style::Position::kStatic) {
        auto block_box = BuildInlineBlock(child_element, containing_width);
        InlineItem item;
        item.style = &child_style;
        item.element = &child_element;
        item.atomic = true;
        item.width = block_box->margin_left + block_box->width + block_box->margin_right;
        item.height = block_box->margin_top + block_box->height + block_box->margin_bottom;
        item.baseline_offset = block_box->lines.empty() ? block_box->margin_top + block_box->height
                                                        : block_box->lines.back().baseline;
        item.block_box = std::move(block_box);
        items.push_back(std::move(item));
        return;
      }
      if (child_style.display == style::Display::kInlineFlex &&
          child_style.position == style::Position::kStatic) {
        // Inline-level flex container: an atomic inline box whose content is
        // a flex container (handled by LayoutFlexContent inside
        // BuildInlineBlock's LayoutBlockContent dispatch).
        auto block_box = BuildInlineBlock(child_element, containing_width);
        InlineItem item;
        item.style = &child_style;
        item.element = &child_element;
        item.atomic = true;
        item.width = block_box->margin_left + block_box->width + block_box->margin_right;
        item.height = block_box->margin_top + block_box->height + block_box->margin_bottom;
        item.baseline_offset = block_box->lines.empty() ? block_box->margin_top + block_box->height
                                                        : block_box->lines.back().baseline;
        item.block_box = std::move(block_box);
        items.push_back(std::move(item));
        return;
      }
      for (dom::Node* child : node.ChildNodes()) {
        CollectInline(*child, child_style, &child_element, containing_width, items);
      }
    }

    // Lays out the block-level children and inline content of |element| into
    // |box| (which already has its width and content origin set).  Fills
    // box.children and box.lines; returns the total content height.
    // |cb_*| is the containing block (padding box) of |element|; absolutely
    // positioned children are collected into |absolute_children| (not laid out
    // here) because their bottom/right offsets need the finished box height.
    // Lays out the block-level children and inline content of |element| into
    // |box|.  |parent_floats| are floats from the enclosing block formatting
    // context; they keep affecting this block's line boxes (and those of its
    // block children) until their vertical extent ends (CSS2.2 9.5).
    float LayoutBlockContent(LayoutBox& box,
                             dom::Element& element,
                             float avail_width,
                             float cb_x,
                             float cb_y,
                             float cb_w,
                             float cb_h,
                             const std::vector<const LayoutBox*>& parent_floats,
                             std::vector<dom::Element*>& absolute_children)
    {
      // A flex container's children are flex items, not normal-flow content.
      if (box.style.display == style::Display::kFlex ||
          box.style.display == style::Display::kInlineFlex) {
        return LayoutFlexContent(
            box, element, avail_width, cb_x, cb_y, cb_w, cb_h, absolute_children);
      }
      float cursor_y = 0;
      std::vector<InlineItem> inline_items;
      for (dom::Node* child : element.ChildNodes()) {
        if (child->node_type() == dom::NodeType::kText) {
          CollectText(static_cast<dom::Text*>(child)->data(), box.style, &element, inline_items);
          continue;
        }
        if (child->node_type() != dom::NodeType::kElement) {
          continue;
        }
        dom::Element& child_element = *static_cast<dom::Element*>(child);
        const style::ComputedStyle& child_style = styles.StyleFor(child_element);
        if (child_style.display == style::Display::kNone) {
          continue;
        }
        if (child_style.position == style::Position::kAbsolute ||
            child_style.position == style::Position::kFixed) {
          // Out of flow: positioned relative to the containing block.
          absolute_children.push_back(&child_element);
          continue;
        }
        if (child_style.floating != style::Float::kNone &&
            child_style.position == style::Position::kStatic) {
          // float: left/right -- out of flow, placed at the block's side at
          // its source vertical position; line boxes wrap around it.
          float f_y = box.content_y() + cursor_y;
          box.floats.push_back(BuildFloat(child_element, avail_width, box.content_x(), f_y));
          continue;
        }
        if (child_style.display == style::Display::kBlock ||
            child_style.display == style::Display::kFlex) {
          // Floats declared before this block in the source still affect its
          // line boxes, so pass the parent's plus this box's own floats.
          std::vector<const LayoutBox*> cur = parent_floats;
          for (const auto& f : box.floats) {
            cur.push_back(f.get());
          }
          std::unique_ptr<LayoutBox> child_box = BuildBlock(child_element,
                                                            avail_width,
                                                            box.content_x(),
                                                            box.content_y() + cursor_y,
                                                            cb_x,
                                                            cb_y,
                                                            cb_w,
                                                            cb_h,
                                                            cur);
          cursor_y += child_box->margin_top + child_box->height + child_box->margin_bottom;
          box.children.push_back(std::move(child_box));
        } else if (child_style.display == style::Display::kTable) {
          std::unique_ptr<LayoutBox> child_box = BuildTable(child_element,
                                                            avail_width,
                                                            box.content_x(),
                                                            box.content_y() + cursor_y,
                                                            cb_x,
                                                            cb_y,
                                                            cb_w,
                                                            cb_h);
          cursor_y += child_box->margin_top + child_box->height + child_box->margin_bottom;
          box.children.push_back(std::move(child_box));
        } else {
          // Inline element: its text (and atomic <img>/inline-block boxes)
          // flows into this box's lines.
          CollectInline(child_element, child_style, &child_element, avail_width, inline_items);
        }
      }

      // Active floats for this block's line boxes: the block's own floats plus
      // those inherited from the enclosing BFC.
      std::vector<const LayoutBox*> all_floats = parent_floats;
      for (const auto& f : box.floats) {
        all_floats.push_back(f.get());
      }
      float lines_height = 0;
      LayoutLines(inline_items,
                  avail_width,
                  box.content_x(),
                  box.content_y() + cursor_y,
                  registry,
                  box.style,
                  all_floats,
                  box.lines,
                  lines_height);
      // Floats expand the containing block: its height reaches at least the
      // bottom of every float it holds (the float is out of flow, so its
      // height is not otherwise counted here).
      float bottom = cursor_y + lines_height;
      for (const auto& f : box.floats) {
        bottom = std::max(bottom, (f->y - box.content_y()) + f->height);
      }
      return bottom;
    }

    std::unique_ptr<LayoutBox> BuildBlock(dom::Element& element,
                                          float containing_width,
                                          float origin_x,
                                          float origin_y,
                                          float cb_x,
                                          float cb_y,
                                          float cb_w,
                                          float cb_h,
                                          const std::vector<const LayoutBox*>& parent_floats)
    {
      auto box = std::make_unique<LayoutBox>();
      box->element = &element;
      box->style = styles.StyleFor(element);
      ResolveBoxEdges(*box, containing_width);

      // Width: explicit (px or %) or fill the containing block.
      float content_width;
      if (box->style.width.has_value()) {
        content_width = ResolveSize(box->style.width.value(), containing_width);
      } else {
        content_width = containing_width - box->margin_left - box->margin_right - box->border_left -
                        box->border_right - box->padding_left - box->padding_right;
        if (content_width < 0) {
          content_width = 0;
        }
      }
      box->width = content_width + box->border_left + box->border_right + box->padding_left +
                   box->padding_right;

      // Border-box position, including the relative offset.  |origin_x| is the
      // content-box origin of the parent; this box's own margin pushes it out.
      float rel_x = 0;
      float rel_y = 0;
      if (box->style.position == style::Position::kRelative) {
        rel_x = box->style.left;
        rel_y = box->style.top;
      }
      box->x = origin_x + box->margin_left - box->border_left - box->padding_left + rel_x;
      box->y = origin_y + box->margin_top - box->border_top - box->padding_top + rel_y;

      const float avail_width = box->width - box->border_left - box->border_right -
                                box->padding_left - box->padding_right;

      // Containing block for descendants: this box's padding box when it is a
      // positioning ancestor, else the inherited containing block.  x/y/w are
      // known now; h is only needed for absolute descendants' bottom offset
      // and is computed after the content height is known.
      float child_cb_x = cb_x;
      float child_cb_y = cb_y;
      float child_cb_w = cb_w;
      if (box->style.position != style::Position::kStatic) {
        child_cb_x = box->x + box->border_left;
        child_cb_y = box->y + box->border_top;
        child_cb_w = box->width - box->border_left - box->border_right;
      }

      std::vector<dom::Element*> absolute_children;
      float content_height = LayoutBlockContent(*box,
                                                element,
                                                avail_width,
                                                child_cb_x,
                                                child_cb_y,
                                                child_cb_w,
                                                /*cb_h*/ 0.0f,
                                                parent_floats,
                                                absolute_children);

      if (box->style.height.has_value() && !box->style.height.value().percent) {
        content_height = std::max(content_height, box->style.height.value().value);
      }
      box->height = content_height + box->border_top + box->border_bottom + box->padding_top +
                    box->padding_bottom;

      const float child_cb_h = box->style.position != style::Position::kStatic
                                   ? box->height - box->border_top - box->border_bottom
                                   : cb_h;
      for (dom::Element* child : absolute_children) {
        box->positioned_children.push_back(
            BuildAbsolute(*child, child_cb_x, child_cb_y, child_cb_w, child_cb_h, kNoFloats));
      }
      return box;
    }

    // Builds one flex item's box.  For a row container |forced_content_width|
    // is the item's resolved flex main size; for a column container
    // |forced_content_height| is the main size and the width comes from the
    // item's own style or fills the container (stretch).  The box is laid out
    // with its margin-box origin at (0,0); the flex algorithm translates it
    // into place afterwards.
    std::unique_ptr<LayoutBox> BuildFlexItem(dom::Element& element,
                                             std::optional<float> forced_content_width,
                                             std::optional<float> forced_content_height,
                                             float containing_width,
                                             float cb_x,
                                             float cb_y,
                                             float cb_w,
                                             float cb_h)
    {
      auto box = std::make_unique<LayoutBox>();
      box->element = &element;
      box->style = styles.StyleFor(element);
      ResolveBoxEdges(*box, containing_width);

      float content_width;
      if (forced_content_width.has_value()) {
        content_width = forced_content_width.value();
      } else if (box->style.width.has_value()) {
        const style::SizeSpec& width = box->style.width.value();
        content_width = ResolveSize(width, containing_width);
      } else {
        content_width = containing_width - box->margin_left - box->margin_right - box->border_left -
                        box->border_right - box->padding_left - box->padding_right;
        if (content_width < 0) {
          content_width = 0;
        }
      }
      box->width = content_width + box->border_left + box->border_right + box->padding_left +
                   box->padding_right;
      // Margin-box origin at (0,0); content is placed at +border+padding.
      box->x = box->margin_left - box->border_left - box->padding_left;
      box->y = box->margin_top - box->border_top - box->padding_top;

      const float avail = box->width - box->border_left - box->border_right - box->padding_left -
                          box->padding_right;
      std::vector<dom::Element*> absolute_children;
      float content_height = LayoutBlockContent(
          *box, element, avail, cb_x, cb_y, cb_w, cb_h, kNoFloats, absolute_children);
      if (forced_content_height.has_value()) {
        content_height = forced_content_height.value();
      } else if (box->style.height.has_value()) {
        const style::SizeSpec& height = box->style.height.value();
        if (!height.percent) {
          content_height = std::max(content_height, height.value);
        }
      }
      box->height = content_height + box->border_top + box->border_bottom + box->padding_top +
                    box->padding_bottom;
      const float child_cb_h = box->height - box->border_top - box->border_bottom;
      for (dom::Element* child : absolute_children) {
        box->positioned_children.push_back(
            BuildAbsolute(*child, cb_x, cb_y, cb_w, child_cb_h, kNoFloats));
      }
      return box;
    }

    // One in-flow flex item (CSS Flexbox 1 §9.2).
    struct FlexItemData
    {
      dom::Element* element = nullptr;
      const style::ComputedStyle* style = nullptr;
      float border_padding_main = 0;
      float margin_main = 0;
      float border_padding_cross = 0;
      float margin_cross = 0;
      float base_main = 0;     // flex base size (content-box)
      float min_main = 0;      // clamp floor when shrinking
      float content_main = 0;  // final content-box main size
      float content_cross = 0; // natural content-box cross size
      bool base_from_spec = false;
      bool cross_auto = true; // no explicit cross-size property
      std::unique_ptr<LayoutBox> box;
    };

    // A flex line (CSS Flexbox 1 §9.3).
    struct FlexLineData
    {
      std::vector<FlexItemData*> items;
      float outer_main_sum = 0; // outer main sizes including gaps
    };

    // Collects and measures the in-flow flex items of |element|.  |row|
    // selects the main-axis interpretation.  Column items are laid out at
    // their cross width up front so the measured content height can serve as
    // the flex base size.
    void CollectFlexItems(dom::Element& element,
                          float avail_width,
                          bool row,
                          style::AlignItems align_items,
                          float cb_x,
                          float cb_y,
                          float cb_w,
                          float cb_h,
                          std::vector<dom::Element*>& absolute_children,
                          std::vector<FlexItemData>& items)
    {
      for (dom::Node* child : element.ChildNodes()) {
        if (child->node_type() != dom::NodeType::kElement) {
          continue;
        }
        dom::Element& child_el = *static_cast<dom::Element*>(child);
        const style::ComputedStyle& s = styles.StyleFor(child_el);
        if (s.display == style::Display::kNone) {
          continue;
        }
        if (s.position == style::Position::kAbsolute || s.position == style::Position::kFixed) {
          absolute_children.push_back(&child_el);
          continue;
        }
        FlexItemData item;
        item.element = &child_el;
        item.style = &s;
        LayoutBox edges;
        edges.style = s;
        ResolveBoxEdges(edges, avail_width);
        if (row) {
          item.border_padding_main =
              edges.border_left + edges.border_right + edges.padding_left + edges.padding_right;
          item.margin_main = edges.margin_left + edges.margin_right;
          item.border_padding_cross =
              edges.border_top + edges.border_bottom + edges.padding_top + edges.padding_bottom;
          item.margin_cross = edges.margin_top + edges.margin_bottom;
        } else {
          item.border_padding_main =
              edges.border_top + edges.border_bottom + edges.padding_top + edges.padding_bottom;
          item.margin_main = edges.margin_top + edges.margin_bottom;
          item.border_padding_cross =
              edges.border_left + edges.border_right + edges.padding_left + edges.padding_right;
          item.margin_cross = edges.margin_left + edges.margin_right;
        }
        // Flex base size: flex-basis, else the main-size property, else the
        // item's intrinsic content size.  Percentages resolve against the
        // container's content width.
        const style::SizeSpec* main_spec = nullptr;
        if (s.flex_basis.has_value()) {
          main_spec = &s.flex_basis.value();
        } else if (row ? s.width.has_value() : s.height.has_value()) {
          main_spec = row ? &s.width.value() : &s.height.value();
        }
        if (main_spec != nullptr) {
          item.base_main = ResolveSize(*main_spec, avail_width);
          item.base_from_spec = true;
        } else if (row) {
          item.base_main = MeasureContent(child_el, styles, registry).max;
        }
        if (row) {
          item.min_main = MeasureContent(child_el, styles, registry).min;
        }
        item.cross_auto = !(row ? s.height.has_value() : s.width.has_value());
        items.push_back(std::move(item));
      }

      // For a column, the base main size (height) is unknown until the item
      // is laid out at its cross width: build the boxes now and measure.
      if (!row) {
        for (FlexItemData& item : items) {
          float cross_width;
          if (item.style->width.has_value()) {
            cross_width = ResolveSize(item.style->width.value(), avail_width);
          } else if (align_items == style::AlignItems::kStretch) {
            cross_width = avail_width - item.margin_cross - item.border_padding_cross;
            if (cross_width < 0) {
              cross_width = 0;
            }
          } else {
            cross_width = MeasureContent(*item.element, styles, registry).max;
          }
          item.box = BuildFlexItem(
              *item.element, cross_width, std::nullopt, avail_width, cb_x, cb_y, cb_w, cb_h);
          if (!item.base_from_spec) {
            item.base_main = item.box->content_height();
          }
          item.min_main = 0;
          item.content_cross = item.box->content_width();
        }
      }
    }

    // Collects items into flex lines, wrapping when the container main size
    // is definite (CSS Flexbox 1 §9.3).
    static void PackFlexLines(const style::ComputedStyle& cs,
                              bool main_definite,
                              float container_main,
                              float main_gap,
                              std::vector<FlexItemData>& items,
                              std::vector<FlexLineData>& lines)
    {
      FlexLineData current;
      for (FlexItemData& item : items) {
        const float outer = item.base_main + item.border_padding_main + item.margin_main;
        if (cs.flex_wrap != style::FlexWrap::kNoWrap && main_definite && !current.items.empty() &&
            current.outer_main_sum + main_gap + outer > container_main) {
          lines.push_back(std::move(current));
          current = FlexLineData{};
        }
        if (!current.items.empty()) {
          current.outer_main_sum += main_gap;
        }
        current.outer_main_sum += outer;
        current.items.push_back(&item);
      }
      if (!current.items.empty()) {
        lines.push_back(std::move(current));
      }
    }

    // Resolves each item's flexible main size on every line (CSS Flexbox 1
    // §9.7).  Grow distributes positive free space by flex-grow; shrink takes
    // it back proportionally to flex-shrink × content-box flex base size,
    // clamped at the item's min-content main size.
    static void
    ResolveFlexLengths(bool main_definite, float container_main, std::vector<FlexLineData>& lines)
    {
      for (FlexLineData& line : lines) {
        for (FlexItemData* it : line.items) {
          it->content_main = it->base_main;
        }
        if (!main_definite) {
          continue;
        }
        const float free = container_main - line.outer_main_sum;
        if (free > 0) {
          float total_grow = 0;
          for (const FlexItemData* it : line.items) {
            total_grow += it->style->flex_grow;
          }
          if (total_grow > 0) {
            for (FlexItemData* it : line.items) {
              it->content_main += free * it->style->flex_grow / total_grow;
            }
          }
        } else if (free < 0) {
          float total_shrink = 0;
          for (const FlexItemData* it : line.items) {
            total_shrink += it->style->flex_shrink * it->base_main;
          }
          if (total_shrink > 0) {
            for (FlexItemData* it : line.items) {
              const float weight = it->style->flex_shrink * it->base_main;
              float final = it->base_main - (-free) * weight / total_shrink;
              if (it->min_main > 0 && final < it->min_main) {
                final = it->min_main;
              }
              if (final < 0) {
                final = 0;
              }
              it->content_main = final;
            }
          }
        }
      }
    }

    // Lays out |element|'s children as flex items (CSS Flexbox 1 §9), into
    // |box| (whose border-box width is already set).  Supports
    // flex-direction row/column (and reverses), flex-wrap, flex-grow/shrink/
    // basis, justify-content, align-items, align-content (with a definite
    // container cross size) and row/column gap.  Returns the container's
    // content height.
    float LayoutFlexContent(LayoutBox& box,
                            dom::Element& element,
                            float avail_width,
                            float cb_x,
                            float cb_y,
                            float cb_w,
                            float cb_h,
                            std::vector<dom::Element*>& absolute_children)
    {
      const style::ComputedStyle& cs = box.style;
      const bool row = cs.flex_direction == style::FlexDirection::kRow ||
                       cs.flex_direction == style::FlexDirection::kRowReverse;
      const bool reverse_main = cs.flex_direction == style::FlexDirection::kRowReverse ||
                                cs.flex_direction == style::FlexDirection::kColumnReverse;
      const bool reverse_cross = cs.flex_wrap == style::FlexWrap::kWrapReverse;
      const float main_gap = row ? cs.column_gap : cs.row_gap;
      const float cross_gap = row ? cs.row_gap : cs.column_gap;

      // Container main size along the item-flow axis.  For a row this is the
      // (definite) content width; for a column it is the specified content
      // height, else auto (content-determined).  Percent heights are not yet
      // supported and fall back to auto.
      float container_main = avail_width;
      if (!row && cs.height.has_value()) {
        const style::SizeSpec& height = cs.height.value();
        container_main = height.percent ? 0.0f : height.value;
      }
      const bool main_definite = row || cs.height.has_value();
      // Container cross size: for a column it is the (definite) content
      // width; for a row it is the specified content height, else auto.
      float container_cross = avail_width;
      if (row && cs.height.has_value()) {
        const style::SizeSpec& height = cs.height.value();
        container_cross = height.percent ? 0.0f : height.value;
      }
      const bool cross_definite = !row || cs.height.has_value();

      // ---- Collect and measure flex items (§9.2) ----
      std::vector<FlexItemData> items;
      CollectFlexItems(element,
                       avail_width,
                       row,
                       cs.align_items,
                       cb_x,
                       cb_y,
                       cb_w,
                       cb_h,
                       absolute_children,
                       items);

      // ---- Collect items into lines (§9.3) ----
      std::vector<FlexLineData> lines;
      PackFlexLines(cs, main_definite, container_main, main_gap, items, lines);
      if (lines.empty()) {
        return 0.0f;
      }

      // ---- Resolve flexible lengths (§9.7) ----
      ResolveFlexLengths(main_definite, container_main, lines);

      // ---- Build row item boxes at their final main size; fix column
      // heights to their final main size ----
      for (FlexItemData& item : items) {
        if (row) {
          item.box = BuildFlexItem(
              *item.element, item.content_main, std::nullopt, avail_width, cb_x, cb_y, cb_w, cb_h);
          item.content_cross = item.box->content_height();
        } else {
          item.box->height =
              item.content_main + item.border_padding_main; // main == height for column
        }
      }

      // ---- Cross sizes of lines and items (§9.6) ----
      std::vector<float> line_cross(lines.size(), 0.0f);
      for (std::size_t li = 0; li < lines.size(); ++li) {
        for (const FlexItemData* it : lines[li].items) {
          const float outer_cross = it->content_cross + it->border_padding_cross + it->margin_cross;
          line_cross[li] = std::max(line_cross[li], outer_cross);
        }
      }
      // A single line with a definite container cross size and the default
      // align-content: stretch grows to fill the container, so align-items
      // (center / flex-end / stretch) positions items within the full cross
      // size rather than the natural content height.
      if (cross_definite && lines.size() == 1 &&
          cs.align_content == style::AlignContent::kStretch) {
        line_cross[0] = container_cross;
      }

      float total_lines_cross = 0;
      for (std::size_t li = 0; li < lines.size(); ++li) {
        total_lines_cross += line_cross[li];
        if (li + 1 < lines.size()) {
          total_lines_cross += cross_gap;
        }
      }

      // Line cross positions.  Lines always stack along the cross axis; a
      // definite container cross size with multiple lines lets align-content
      // distribute the extra space (or stretch the lines).
      std::vector<float> line_offsets(lines.size(), 0.0f);
      float leading = 0;
      float gap = cross_gap;
      // With a definite container cross size and multiple lines,
      // align-content distributes the positive extra space.  With no (or
      // negative) extra space every alignment packs toward cross-start
      // (CSS Flexbox 1 §8.4), so the switch is skipped entirely.
      const float align_extra = cross_definite ? container_cross - total_lines_cross : 0.0f;
      if (align_extra > 0) {
        switch (cs.align_content) {
        case style::AlignContent::kStretch:
          if (align_extra > 0) {
            const float share = align_extra / static_cast<float>(lines.size());
            for (float& lc : line_cross) {
              lc += share;
            }
          }
          break;
        case style::AlignContent::kFlexEnd:
          leading = align_extra;
          break;
        case style::AlignContent::kCenter:
          leading = align_extra / 2.0f;
          break;
        case style::AlignContent::kSpaceBetween:
          gap = cross_gap + align_extra / static_cast<float>(lines.size() - 1);
          break;
        case style::AlignContent::kSpaceAround:
          gap = cross_gap + align_extra / static_cast<float>(lines.size());
          leading = (align_extra / static_cast<float>(lines.size())) / 2.0f;
          break;
        case style::AlignContent::kFlexStart:
          break;
        }
      }
      if (reverse_cross) {
        // Wrap-reverse stacks the lines from the cross end.
        float cursor = cross_definite ? container_cross : total_lines_cross;
        for (std::size_t li = 0; li < lines.size(); ++li) {
          cursor -= line_cross[li];
          line_offsets[li] = cursor;
          cursor -= gap;
        }
      } else {
        float cursor = leading;
        for (std::size_t li = 0; li < lines.size(); ++li) {
          line_offsets[li] = cursor;
          cursor += line_cross[li] + gap;
        }
      }

      // Baseline of a laid-out box relative to its border-box top.
      auto baseline_of = [](const LayoutBox& b) -> float {
        if (b.lines.empty()) {
          return b.height;
        }
        return b.lines.back().baseline - b.y;
      };

      // ---- Place items (§9.4 / §9.5) ----
      for (std::size_t li = 0; li < lines.size(); ++li) {
        const FlexLineData& line = lines[li];
        const int n = static_cast<int>(line.items.size());
        // Final free space along the main axis for this line.
        float content_sum = 0;
        for (const FlexItemData* it : line.items) {
          content_sum += it->content_main + it->border_padding_main + it->margin_main;
        }
        const float gaps = main_gap * static_cast<float>(std::max(0, n - 1));
        const float free = container_main - (content_sum + gaps);
        float main_cursor = 0;
        float extra_gap = 0;
        // With negative free space the line overflows the container; every
        // justify-content value then packs items toward main-start with
        // overflow at main-end (CSS Flexbox 1 §8.2), so the distribution is
        // only applied when there is space to distribute.
        if (free > 0) {
          switch (cs.justify_content) {
          case style::JustifyContent::kFlexEnd:
            main_cursor = free;
            break;
          case style::JustifyContent::kCenter:
            main_cursor = free / 2.0f;
            break;
          case style::JustifyContent::kSpaceBetween:
            if (n > 1) {
              extra_gap = free / static_cast<float>(n - 1);
            }
            break;
          case style::JustifyContent::kSpaceAround:
            extra_gap = free / static_cast<float>(n);
            main_cursor = extra_gap / 2.0f;
            break;
          case style::JustifyContent::kSpaceEvenly:
            extra_gap = free / static_cast<float>(n + 1);
            main_cursor = extra_gap;
            break;
          case style::JustifyContent::kFlexStart:
            break;
          }
        }

        float line_baseline = 0;
        if (cs.align_items == style::AlignItems::kBaseline) {
          for (const FlexItemData* it : line.items) {
            line_baseline = std::max(line_baseline, baseline_of(*it->box));
          }
        }

        for (FlexItemData* it : line.items) {
          // Cross size: stretch items fill the line.
          if (cs.align_items == style::AlignItems::kStretch && it->cross_auto) {
            const float target = line_cross[li] - it->margin_cross;
            if (row) {
              it->box->height = std::max(it->box->height, target);
              it->content_cross = it->box->content_height();
            } else {
              it->box->width = std::max(it->box->width, target);
              it->content_cross = it->box->content_width();
            }
          }

          const float outer_cross = it->content_cross + it->border_padding_cross + it->margin_cross;
          const float outer_main = it->content_main + it->border_padding_main + it->margin_main;

          // Cross offset within the line.
          float cross_pos = 0;
          switch (cs.align_items) {
          case style::AlignItems::kFlexEnd:
            cross_pos = line_cross[li] - outer_cross;
            break;
          case style::AlignItems::kCenter:
            cross_pos = (line_cross[li] - outer_cross) / 2.0f;
            break;
          case style::AlignItems::kBaseline:
            cross_pos = line_baseline - baseline_of(*it->box);
            break;
          case style::AlignItems::kStretch:
          case style::AlignItems::kFlexStart:
            break;
          }

          // Margin-box origin in container content coordinates.
          float main_pos = main_cursor;
          if (reverse_main) {
            main_pos = container_main - main_cursor - outer_main;
          }
          float cross_pos_abs = line_offsets[li] + cross_pos;

          float dx = main_pos;
          float dy = cross_pos_abs;
          if (!row) {
            std::swap(dx, dy);
          }
          TranslateBox(*it->box, box.content_x() + dx, box.content_y() + dy);
          box.children.push_back(std::move(it->box));

          main_cursor += outer_main + main_gap + extra_gap;
        }
      }

      // Container content height: for a row it is the stacked cross sizes of
      // the lines; for a column it is the tallest line's main extent.
      float content_height = 0;
      if (row) {
        content_height = total_lines_cross;
        if (cross_definite) {
          content_height = std::max(content_height, container_cross);
        }
      } else {
        for (const FlexLineData& line : lines) {
          float extent = 0;
          for (const FlexItemData* it : line.items) {
            extent += it->content_main + it->border_padding_main + it->margin_main;
          }
          extent +=
              main_gap * static_cast<float>(std::max(0, static_cast<int>(line.items.size()) - 1));
          content_height = std::max(content_height, extent);
        }
      }
      return content_height;
    }

    // Lays out an absolutely positioned element (out of flow) against its
    // containing block |cb_*|.  Content is laid out at a local origin and the
    // finished box is translated into place once its height (needed for
    // bottom/right offsets) is known.  Width follows CSS2.2 §10.3.7: the
    // shrink-to-fit min(max(min-content, available), preferred) when auto, or
    // the left/right constraint equation when both insets are given.
    std::unique_ptr<LayoutBox> BuildAbsolute(dom::Element& element,
                                             float cb_x,
                                             float cb_y,
                                             float cb_w,
                                             float cb_h,
                                             const std::vector<const LayoutBox*>& parent_floats)
    {
      auto box = std::make_unique<LayoutBox>();
      box->element = &element;
      box->style = styles.StyleFor(element);
      ResolveBoxEdges(*box, cb_w);

      const float extras = box->margin_left + box->margin_right + box->border_left +
                           box->border_right + box->padding_left + box->padding_right;
      // Available width for shrink-to-fit is found by solving the constraint
      // equation with the unspecified inset set to 0, i.e. it excludes any
      // specified left/right inset (CSS2.2 §10.3.7).
      float inset_x = 0;
      if (!box->style.left_auto) {
        inset_x += box->style.left;
      }
      if (!box->style.right_auto) {
        inset_x += box->style.right;
      }
      const float available = std::max(0.0f, cb_w - inset_x - extras);

      float content_width;
      if (box->style.width.has_value()) {
        content_width = ResolveSize(box->style.width.value(), cb_w);
      } else if (!box->style.left_auto && !box->style.right_auto) {
        // Constraint equation: left + width + right = containing block width.
        content_width = cb_w - box->style.left - box->style.right - extras;
        if (content_width < 0) {
          content_width = 0;
        }
      } else {
        // shrink-to-fit.
        const IntrinsicWidths w = MeasureContent(element, styles, registry);
        content_width = std::min(std::max(w.min, available), w.max);
      }
      box->width = content_width + box->border_left + box->border_right + box->padding_left +
                   box->padding_right;
      const float avail_width = box->width - box->border_left - box->border_right -
                                box->padding_left - box->padding_right;

      std::vector<dom::Element*> absolute_children;
      float content_height = LayoutBlockContent(*box,
                                                element,
                                                avail_width,
                                                box->border_left,
                                                box->border_top,
                                                avail_width,
                                                /*cb_h*/ 0.0f,
                                                parent_floats,
                                                absolute_children);
      if (box->style.height.has_value() && !box->style.height.value().percent) {
        content_height = std::max(content_height, box->style.height.value().value);
      }
      box->height = content_height + box->border_top + box->border_bottom + box->padding_top +
                    box->padding_bottom;

      // Absolutely positioned descendants use this box's padding box (local).
      const float child_cb_h = box->height - box->border_top - box->border_bottom;
      for (dom::Element* child : absolute_children) {
        box->positioned_children.push_back(BuildAbsolute(
            *child, box->border_left, box->border_top, avail_width, child_cb_h, parent_floats));
      }

      float x = cb_x;
      if (!box->style.left_auto) {
        x = cb_x + box->style.left;
      } else if (!box->style.right_auto) {
        x = cb_x + cb_w - box->style.right - box->width;
      }
      float y = cb_y;
      if (!box->style.top_auto) {
        y = cb_y + box->style.top;
      } else if (!box->style.bottom_auto) {
        y = cb_y + cb_h - box->style.bottom - box->height;
      }
      TranslateBox(*box, x, y); // content was laid out at the local (0,0)
      return box;
    }

    // Lays out a table cell's content into a box of the given content width.
    // The box is positioned at (0,0); the caller translates it to its grid
    // slot.  Margins are ignored (table cells have no margin in CSS).
    std::unique_ptr<LayoutBox> LayoutCell(
        dom::Element& element, float content_width, float cb_x, float cb_y, float cb_w, float cb_h)
    {
      auto box = std::make_unique<LayoutBox>();
      box->element = &element;
      box->style = styles.StyleFor(element);
      ResolveBoxEdges(*box, content_width);
      box->width = content_width + box->border_left + box->border_right + box->padding_left +
                   box->padding_right;
      const float avail_width = box->width - box->border_left - box->border_right -
                                box->padding_left - box->padding_right;
      std::vector<dom::Element*> absolute_children;
      const float content_height = LayoutBlockContent(
          *box, element, avail_width, cb_x, cb_y, cb_w, cb_h, kNoFloats, absolute_children);
      box->height = content_height + box->border_top + box->border_bottom + box->padding_top +
                    box->padding_bottom;
      for (dom::Element* child : absolute_children) {
        box->positioned_children.push_back(
            BuildAbsolute(*child, cb_x, cb_y, cb_w, cb_h, kNoFloats));
      }
      return box;
    }

    std::unique_ptr<LayoutBox> BuildTable(dom::Element& element,
                                          float containing_width,
                                          float origin_x,
                                          float origin_y,
                                          float cb_x,
                                          float cb_y,
                                          float cb_w,
                                          float cb_h)
    {
      auto table = std::make_unique<LayoutBox>();
      table->element = &element;
      table->style = styles.StyleFor(element);
      ResolveBoxEdges(*table, containing_width);

      // Table width: explicit, or fill the containing block (auto is treated as
      // 100% rather than CSS shrink-to-fit; documented limitation).
      float content_width;
      if (table->style.width.has_value()) {
        content_width = ResolveSize(table->style.width.value(), containing_width);
      } else {
        content_width = containing_width - table->margin_left - table->margin_right -
                        table->border_left - table->border_right - table->padding_left -
                        table->padding_right;
        if (content_width < 0) {
          content_width = 0;
        }
      }
      table->width = content_width + table->border_left + table->border_right +
                     table->padding_left + table->padding_right;
      table->x = origin_x + table->margin_left - table->border_left - table->padding_left;
      table->y = origin_y + table->margin_top - table->border_top - table->padding_top;
      const float table_x = table->content_x();
      const float table_y = table->content_y();

      // Collect rows (flattening row groups) and cells, then place cells into a
      // grid honoring colspan/rowspan.  Row-group boundaries are kept so a
      // rowspan="0" cell spans to the end of its own group (thead/tbody/tfoot,
      // or the implicit group of consecutive anonymous <tr>).
      struct RowInfo
      {
        dom::Element* element = nullptr;
        style::ComputedStyle style;
      };
      struct RowGroup
      {
        int start = 0; // inclusive first row index
        int end = 0;   // exclusive last row index
      };
      std::vector<RowInfo> rows;
      std::vector<RowGroup> row_groups;
      std::vector<CellInfo> cells;
      std::vector<std::vector<int>> grid; // grid[r][c] = cell index, or -1
      int implicit_group_start_ = -1;     // first row of the current implicit group

      auto flush_implicit_group = [&](int next_row) {
        if (implicit_group_start_ >= 0 && implicit_group_start_ < next_row) {
          row_groups.push_back(RowGroup{implicit_group_start_, next_row});
        }
        implicit_group_start_ = -1;
      };

      auto collect_rows = [&](const dom::Element& container) {
        for (dom::Node* child : container.ChildNodes()) {
          if (child->node_type() != dom::NodeType::kElement) {
            continue;
          }
          dom::Element& el = static_cast<dom::Element&>(*child);
          const style::ComputedStyle& cs = styles.StyleFor(el);
          if (cs.display == style::Display::kTableRowGroup) {
            flush_implicit_group(static_cast<int>(rows.size()));
            const int group_start = static_cast<int>(rows.size());
            for (dom::Node* gchild : el.ChildNodes()) {
              if (gchild->node_type() == dom::NodeType::kElement &&
                  styles.StyleFor(static_cast<dom::Element&>(*gchild)).display ==
                      style::Display::kTableRow) {
                dom::Element& rel = static_cast<dom::Element&>(*gchild);
                rows.push_back(RowInfo{&rel, styles.StyleFor(rel)});
              }
            }
            row_groups.push_back(RowGroup{group_start, static_cast<int>(rows.size())});
          } else if (cs.display == style::Display::kTableRow) {
            // Consecutive anonymous <tr> children form one implicit row group.
            if (implicit_group_start_ < 0) {
              implicit_group_start_ = static_cast<int>(rows.size());
            }
            rows.push_back(RowInfo{&el, cs});
          }
          // caption / colgroup / col / whitespace are not part of the row grid.
        }
      };
      collect_rows(element);
      flush_implicit_group(static_cast<int>(rows.size()));
      const int nrows = static_cast<int>(rows.size());

      // Collect every cell with its (unresolved) span; rowspan="0" is resolved
      // below once the row-group boundaries are known.
      for (int r = 0; r < nrows; ++r) {
        dom::Element& tr = *rows[static_cast<std::size_t>(r)].element;
        for (dom::Node* child : tr.ChildNodes()) {
          if (child->node_type() != dom::NodeType::kElement) {
            continue;
          }
          dom::Element& cel = static_cast<dom::Element&>(*child);
          const style::ComputedStyle& cs = styles.StyleFor(cel);
          if (cs.display != style::Display::kTableCell) {
            continue;
          }
          CellInfo info;
          info.element = &cel;
          info.style = cs;
          info.row = r;
          info.colspan = ResolveColspan(cel);
          info.rowspan = ResolveRowspan(cel, info.grows_downward);
          cells.push_back(std::move(info));
        }
      }
      // rowspan="0": the cell spans the remaining rows of its own row group
      // (WHATWG tables.html: "span all the remaining rows in the row group").
      for (CellInfo& cell : cells) {
        if (!cell.grows_downward) {
          continue;
        }
        int group_end = nrows;
        for (const RowGroup& group : row_groups) {
          if (cell.row >= group.start && cell.row < group.end) {
            group_end = group.end;
            break;
          }
        }
        cell.rowspan = std::max(1, group_end - cell.row);
      }

      // Place cells into the grid: find each cell's first free column, honoring
      // the (now resolved) colspan/rowspan occupancy.
      for (std::size_t i = 0; i < cells.size(); ++i) {
        CellInfo& info = cells[i];
        const std::size_t ri = static_cast<std::size_t>(info.row);
        if (grid.size() <= ri) {
          grid.resize(ri + 1);
        }
        int col = 0;
        while (true) {
          bool free = true;
          for (int cc = col; cc < col + info.colspan; ++cc) {
            const std::size_t cci = static_cast<std::size_t>(cc);
            if (cci < grid[ri].size() && grid[ri][cci] != -1) {
              free = false;
              break;
            }
          }
          if (free) {
            break;
          }
          ++col;
        }
        info.col = col;
        for (int rr = info.row; rr < info.row + info.rowspan; ++rr) {
          const std::size_t rri = static_cast<std::size_t>(rr);
          if (grid.size() <= rri) {
            grid.resize(rri + 1);
          }
          const std::size_t cols = static_cast<std::size_t>(col + info.colspan);
          if (grid[rri].size() < cols) {
            grid[rri].resize(cols, -1);
          }
          for (int cc = col; cc < col + info.colspan; ++cc) {
            grid[rri][static_cast<std::size_t>(cc)] = static_cast<int>(i);
          }
        }
      }

      const int ncols =
          grid.empty()
              ? 0
              : static_cast<int>(
                    std::max_element(grid.begin(), grid.end(), [](const auto& a, const auto& b) {
                      return a.size() < b.size();
                    })->size());

      // Intrinsic widths for auto column sizing.
      for (CellInfo& cell : cells) {
        const IntrinsicWidths w = MeasureContent(*cell.element, styles, registry);
        cell.min_width = w.min;
        cell.max_width = w.max;
      }

      const std::vector<float> col_widths = ComputeColumnWidths(cells, ncols, content_width);

      // Lay out each cell (at a local origin) and derive row heights.
      for (CellInfo& cell : cells) {
        cell.box = LayoutCell(
            *cell.element, SumColumns(col_widths, cell.col, cell.colspan), cb_x, cb_y, cb_w, cb_h);
      }

      std::vector<float> row_heights(rows.size(), 0.0f);
      for (const CellInfo& cell : cells) {
        if (cell.rowspan == 1) {
          const std::size_t ri = static_cast<std::size_t>(cell.row);
          row_heights[ri] = std::max(row_heights[ri], cell.box->height);
        }
      }
      // Row-spanning cells: make sure the spanned rows are tall enough, giving
      // any overflow to the last spanned row (simplified CSS2.1 distribution).
      for (const CellInfo& cell : cells) {
        if (cell.rowspan <= 1) {
          continue;
        }
        float spanned = 0;
        for (int r = cell.row;
             r < cell.row + cell.rowspan && r < static_cast<int>(row_heights.size());
             ++r) {
          spanned += row_heights[static_cast<std::size_t>(r)];
        }
        if (cell.box->height > spanned) {
          const int last =
              std::min(cell.row + cell.rowspan - 1, static_cast<int>(row_heights.size()) - 1);
          row_heights[static_cast<std::size_t>(last)] += cell.box->height - spanned;
        }
      }

      // Build the box tree (table -> rows -> cells) with grid positions.
      float y = 0;
      for (std::size_t r = 0; r < rows.size(); ++r) {
        auto row_box = std::make_unique<LayoutBox>();
        row_box->element = rows[r].element;
        row_box->style = rows[r].style;
        row_box->x = table_x;
        row_box->y = table_y + y;
        row_box->width = content_width;
        row_box->height = row_heights[r];
        for (CellInfo& cell : cells) {
          if (cell.row != static_cast<int>(r)) {
            continue;
          }
          const float cx = table_x + SumColumns(col_widths, 0, cell.col);
          const float cy = table_y + y;
          TranslateBox(*cell.box, cx, cy);
          if (cell.rowspan <= 1) {
            cell.box->height = row_heights[r];
          } else {
            float spanned = 0;
            for (int rr = cell.row;
                 rr < cell.row + cell.rowspan && rr < static_cast<int>(row_heights.size());
                 ++rr) {
              spanned += row_heights[static_cast<std::size_t>(rr)];
            }
            cell.box->height = spanned;
          }
          row_box->children.push_back(std::move(cell.box));
        }
        table->children.push_back(std::move(row_box));
        y += row_heights[r];
      }

      table->height =
          y + table->border_top + table->border_bottom + table->padding_top + table->padding_bottom;
      return table;
    }
  };

  dom::Element* root = document.document_element();
  if (root == nullptr) {
    return nullptr;
  }
  Builder builder{styles_, registry_, images_};
  return builder.BuildBlock(*root,
                            viewport_width,
                            0,
                            0, /*cb=initial containing block*/
                            0,
                            0,
                            viewport_width,
                            0,
                            kNoFloats);
}

} // namespace neko::layout
