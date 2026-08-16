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

// Width of the marker gutter reserved on the left of every <li> content box.
// The marker (bullet / number) is drawn in this gutter, inside the list's
// padding-left (the UA stylesheet gives <ul>/<ol> a 40px padding).
constexpr float kListMarkerGap = 24.0f;

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
  if (spec.is_clamp) {
    // clamp(MIN, VAL, MAX) = max(MIN, min(VAL, MAX)).
    const auto resolve = [&](const style::CalcTerm& t) {
      return containing * t.percent / 100.0f + t.offset;
    };
    if (spec.extremum_args.size() != 3) {
      return 0;
    }
    const float min_val = resolve(spec.extremum_args[0]);
    const float val = resolve(spec.extremum_args[1]);
    const float max_val = resolve(spec.extremum_args[2]);
    return std::max(min_val, std::min(val, max_val));
  }
  if (spec.is_extremum) {
    float best = 0;
    for (std::size_t i = 0; i < spec.extremum_args.size(); ++i) {
      const float v =
          containing * spec.extremum_args[i].percent / 100.0f + spec.extremum_args[i].offset;
      if (i == 0) {
        best = v;
      } else if (spec.extremum_is_max) {
        best = std::max(best, v);
      } else {
        best = std::min(best, v);
      }
    }
    return best;
  }
  if (spec.is_calc) {
    return containing * spec.calc.percent / 100.0f + spec.calc.offset;
  }
  if (spec.percent) {
    return containing * spec.value / 100.0f;
  }
  return spec.value;
}

// Resolves a specified width/height to a CONTENT-box size honoring
// box-sizing (CSS-UI-3 §6): with border-box the specified size covers the
// border+padding, so the content size is the specified size minus those.
// |borders_paddings| is border+padding on the relevant axis.
float SpecToContent(const style::SizeSpec& spec,
                    float containing,
                    float borders_paddings,
                    style::BoxSizing sizing)
{
  const float resolved = ResolveSize(spec, containing);
  if (sizing == style::BoxSizing::kBorderBox) {
    return std::max(0.0f, resolved - borders_paddings);
  }
  return resolved;
}

// Border-box size that corresponds to a content-box size.
[[maybe_unused]] float ContentToBox(float content, float borders_paddings)
{
  return content + borders_paddings;
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

    // Text alignment (CSS Text 3 §5): shift the line's content within the
    // available width.  The line's content spans [line_left, content_right]
    // in the line's local coordinates (relative to origin_x); the alignment
    // fills the leftover space.  Left alignment leaves the content as-is;
    // justify is not implemented (words would need to be spread), so it also
    // leaves the content as-is.
    float align_offset = 0;
    if (container_style.text_align == style::TextAlign::kCenter ||
        container_style.text_align == style::TextAlign::kRight) {
      float content_right = 0;
      for (const TextRun& run : line.runs) {
        content_right = std::max(content_right, run.x + run.width);
      }
      for (const InlineBox& b : line.boxes) {
        content_right = std::max(content_right, b.x + b.width);
      }
      const float used = std::max(0.0f, content_right - line_left);
      const float leftover = std::max(0.0f, line_right - line_left - used);
      if (container_style.text_align == style::TextAlign::kRight) {
        align_offset = leftover;
      } else {
        align_offset = leftover / 2.0f;
      }
    }

    // Baseline absolute within the line.
    const float baseline = origin_y + line_top + baseline_off;
    line.baseline = baseline;
    // Position text runs: their bottom sits on the baseline.
    for (TextRun& run : line.runs) {
      float asc = 0, desc = 0;
      run_metrics(run, asc, desc);
      run.y = baseline - asc;
      run.x = origin_x + run.x + align_offset;
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
      b.x = origin_x + b.x + align_offset;
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
    const std::string& text = item.text;
    // white-space: nowrap — collapse whitespace but never wrap: the entire
    // run is one unbreakable word.  It may overflow the line box (the
    // correct nowrap behavior for navbars, buttons and tab strips).
    if (style.white_space == style::WhiteSpace::kNowrap) {
      std::string collapsed;
      bool first = true;
      bool pending_space = false;
      for (const char c : text) {
        if (IsWordBreak(c)) {
          pending_space = true;
          continue;
        }
        if (pending_space && !first) {
          collapsed.push_back(' ');
        }
        pending_space = false;
        first = false;
        collapsed.push_back(c);
      }
      if (!collapsed.empty()) {
        add_word(collapsed, style, item.element, selector);
      }
      continue;
    }
    std::size_t start = 0;
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
    // Only a definite plain length (not a percentage / calc / extremum) is a
    // fixed intrinsic size.  With box-sizing: border-box the specified width
    // covers border+padding, so the content intrinsic width is smaller.
    if (style.width.has_value() && !style.width.value().percent && !style.width.value().is_calc &&
        !style.width.value().is_extremum) {
      const float specified = style.width.value().value;
      if (style.box_sizing == style::BoxSizing::kBorderBox) {
        const float bp = ResolveSize(style.border_left, 0) + ResolveSize(style.border_right, 0) +
                         ResolveSize(style.padding_left, 0) + ResolveSize(style.padding_right, 0);
        w.min = w.max = std::max(0.0f, specified - bp);
      } else {
        w.min = w.max = specified;
      }
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
                             child_style.display == style::Display::kListItem ||
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
      const float border_padding = ResolveSize(cell.style.border_left, table_width) +
                                   ResolveSize(cell.style.border_right, table_width) +
                                   ResolveSize(cell.style.padding_left, table_width) +
                                   ResolveSize(cell.style.padding_right, table_width);
      const float w = SpecToContent(
          cell.style.width.value(), table_width, border_padding, cell.style.box_sizing);
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

// The ordinal position (1-based) of an <li> within its nearest <ol>/<ul>
// ancestor: the number of preceding <li> siblings at the same level.  Returns
// 0 when the element is not inside a list.
int ListItemOrdinal(const dom::Element& element)
{
  const dom::Node* parent = element.parent();
  if (parent == nullptr || parent->node_type() != dom::NodeType::kElement) {
    return 0;
  }
  const dom::Element& parent_el = *static_cast<const dom::Element*>(parent);
  const std::string_view parent_tag = parent_el.tag_name();
  if (parent_tag != "ol" && parent_tag != "ul") {
    return 0;
  }
  int ordinal = 0;
  for (dom::Node* child : parent_el.ChildNodes()) {
    if (child == &element) {
      break;
    }
    if (child->node_type() == dom::NodeType::kElement &&
        static_cast<const dom::Element*>(child)->tag_name() == "li") {
      ++ordinal;
    }
  }
  return ordinal + 1;
}

// The marker text for an <li> given its list-style-type and ordinal.  Bullet
// types return a glyph; the numbering types return the number (followed by a
// period).  Returns empty for list-style-type: none.
std::string ListMarkerText(style::ListStyleType type, int ordinal)
{
  switch (type) {
  case style::ListStyleType::kDisc:
    return "\xE2\x80\xA2 "; // U+2022 BULLET + space
  case style::ListStyleType::kCircle:
    return "\xE2\x97\x8B "; // U+25CB WHITE CIRCLE + space
  case style::ListStyleType::kSquare:
    return "\xE2\x96\xAA "; // U+25AA BLACK SMALL SQUARE + space
  case style::ListStyleType::kDecimal:
    return std::to_string(std::max(ordinal, 1)) + ". ";
  case style::ListStyleType::kLowerAlpha: {
    const int n = std::max(ordinal, 1);
    std::string out;
    out.push_back(static_cast<char>('a' + ((n - 1) % 26)));
    out += ". ";
    return out;
  }
  case style::ListStyleType::kUpperAlpha: {
    const int n = std::max(ordinal, 1);
    std::string out;
    out.push_back(static_cast<char>('A' + ((n - 1) % 26)));
    out += ". ";
    return out;
  }
  case style::ListStyleType::kLowerRoman:
  case style::ListStyleType::kUpperRoman: {
    // Roman numerals up to 3999 (beyond that, the value repeats modulo).
    static constexpr const char* kUnits[] = {
        "", "i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix"};
    static constexpr const char* kTens[] = {
        "", "x", "xx", "xxx", "xl", "l", "lx", "lxx", "lxxx", "xc"};
    static constexpr const char* kHundreds[] = {
        "", "c", "cc", "ccc", "cd", "d", "dc", "dcc", "dccc", "cm"};
    static constexpr const char* kThousands[] = {"", "m", "mm", "mmm"};
    int n = std::max(ordinal, 1);
    std::string roman = std::string(kThousands[(n / 1000) % 4]) + kHundreds[(n / 100) % 10] +
                        kTens[(n / 10) % 10] + kUnits[n % 10];
    if (type == style::ListStyleType::kUpperRoman) {
      for (char& c : roman) {
        c = static_cast<char>(c - 32); // to uppercase ASCII
      }
    }
    return roman + ". ";
  }
  case style::ListStyleType::kNone:
    break;
  }
  return std::string();
}

} // namespace

std::unique_ptr<LayoutBox>
LayoutEngine::BuildLayoutTree(dom::Document& document, float viewport_width, float viewport_height)
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

    // Applies CSS 2.1 §10.3.3 auto horizontal margins: a block-level box with
    // a definite width and auto left/right margins centers itself within its
    // containing block by splitting the leftover space between the margins
    // (the common `margin: 0 auto` pattern).  Must run after the box's width
    // is final and before its x is derived from margin_left.
    void CenterWithAutoMargins(LayoutBox& box, float containing_width)
    {
      const bool left_auto = box.style.margin_left_auto;
      const bool right_auto = box.style.margin_right_auto;
      if (!left_auto && !right_auto) {
        return;
      }
      const float used = box.width + box.margin_left + box.margin_right;
      const float leftover = containing_width - used;
      if (leftover <= 0) {
        return;
      }
      if (left_auto && right_auto) {
        box.margin_left = box.margin_right = leftover / 2.0f;
      } else if (left_auto) {
        box.margin_left = leftover;
      } else {
        box.margin_right = leftover;
      }
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

      const float border_padding_w =
          box->border_left + box->border_right + box->padding_left + box->padding_right;
      float content_width;
      if (box->style.width.has_value()) {
        content_width = SpecToContent(
            box->style.width.value(), containing_width, border_padding_w, box->style.box_sizing);
      } else {
        const float extras = box->margin_left + box->margin_right + border_padding_w;
        const float available = std::max(0.0f, containing_width - extras);
        const IntrinsicWidths w = MeasureContent(element, styles, registry);
        content_width = std::min(std::max(w.min, available), w.max);
      }
      box->width = content_width + border_padding_w;

      // Border-box origin at the margin edge; content is placed at
      // content_x()/content_y() = +border+padding.
      box->x = box->margin_left;
      box->y = box->margin_top;

      const float avail_width = box->width - box->border_left - box->border_right -
                                box->padding_left - box->padding_right;
      std::vector<dom::Element*> absolute_children;
      float content_height = LayoutBlockContent(
          *box, element, avail_width, 0, 0, avail_width, 0, kNoFloats, absolute_children);
      const float border_padding_h =
          box->border_top + box->border_bottom + box->padding_top + box->padding_bottom;
      if (box->style.height.has_value() && !box->style.height.value().percent) {
        content_height = SpecToContent(box->style.height.value(),
                                       containing_width,
                                       border_padding_h,
                                       box->style.box_sizing); // specified height wins (10.6.2)
      } else if (box->style.aspect_ratio.has_value() && box->style.width.has_value()) {
        // aspect-ratio (CSS Box Sizing 4): definite width, auto height.
        content_height = content_width / box->style.aspect_ratio.value();
      }
      box->height = content_height + border_padding_h;
      const float child_cb_h = box->height - box->border_top - box->border_bottom;
      for (dom::Element* child : absolute_children) {
        box->positioned_children.push_back(
            BuildAbsolute(*child, 0, 0, avail_width, child_cb_h, kNoFloats));
      }
      return box;
    }

    // Lays out a form control (<input>/<textarea>/<select>) as an atomic
    // inline box with default widget chrome (border + light background) and the
    // control's value — or its placeholder when empty — as a single text run.
    // Selection / editing is wired by the browser layer (focused element +
    // key input).
    std::unique_ptr<LayoutBox> BuildFormControl(dom::Element& element, float containing_width)
    {
      auto box = std::make_unique<LayoutBox>();
      box->element = &element;
      box->style = styles.StyleFor(element);
      ResolveBoxEdges(*box, containing_width);

      // Default widget chrome; author CSS overrides via background/border.
      if (!box->style.background_color.has_value()) {
        box->style.background_color = css::Color{255, 255, 255, 255};
      }
      if (box->border_top == 0) {
        box->border_top = 1;
      }
      if (box->border_right == 0) {
        box->border_right = 1;
      }
      if (box->border_bottom == 0) {
        box->border_bottom = 1;
      }
      if (box->border_left == 0) {
        box->border_left = 1;
      }
      if (!box->style.border_color.has_value()) {
        box->style.border_color = css::Color{0, 0, 0, 255};
      }
      if (box->padding_left == 0 && box->padding_right == 0) {
        box->padding_left = box->padding_right = 4;
      }
      if (box->padding_top == 0 && box->padding_bottom == 0) {
        box->padding_top = box->padding_bottom = 2;
      }

      const float border_padding_w = box->border_left + box->border_right + box->padding_left +
                                     box->padding_right;
      float content_width = 0;
      if (box->style.width.has_value() && !box->style.width.value().percent &&
          !box->style.width.value().is_calc && !box->style.width.value().is_extremum) {
        content_width = box->style.width.value().value;
      } else {
        content_width = 170.0f; // default text-field width
      }
      box->width = content_width + border_padding_w;

      const float line_h = std::max(1.0f, box->style.font_size * 1.2f);
      box->height = line_h + box->padding_top + box->padding_bottom + box->border_top +
                    box->border_bottom;
      box->x = box->margin_left;
      box->y = box->margin_top;

      // The displayed text: the value for an <input>, the content for a
      // <textarea>, the first option for a <select>; placeholder when empty.
      std::string text;
      if (element.tag_name() == "textarea") {
        text = element.TextContent();
      } else if (element.tag_name() == "select") {
        for (dom::Node* c : element.ChildNodes()) {
          if (c->node_type() != dom::NodeType::kElement) {
            continue;
          }
          dom::Element& opt = static_cast<dom::Element&>(*c);
          if (opt.tag_name() == "option") {
            const std::optional<std::string_view> val = opt.GetAttribute("value");
            text = val.has_value() ? std::string(*val) : opt.TextContent();
            break;
          }
        }
      } else {
        const std::optional<std::string_view> val = element.GetAttribute("value");
        text = val.has_value() ? std::string(*val) : "";
      }
      const bool is_placeholder = text.empty();
      if (is_placeholder) {
        text = std::string(element.GetAttribute("placeholder").value_or(""));
      }
      if (!text.empty()) {
        TextRun run;
        run.text = text;
        run.font_family = box->style.font_family;
        run.font_weight = box->style.font_weight;
        run.font_italic = box->style.font_italic;
        run.font_size = box->style.font_size;
        // The box origin carries its margins; content starts inside the
        // border+padding.  Run coordinates must be global (the box origin
        // offset is included), matching how the painter draws runs.
        run.x = box->x + box->border_left + box->padding_left;
        run.y = box->y + box->border_top + box->padding_top;
        run.color = is_placeholder ? css::Color{160, 160, 160, 255}
                                   : css::Color{0, 0, 0, 255};
        run.width = MeasureTextWidth(registry, run.font_family, run.font_weight, run.font_italic,
                                     run.text, run.font_size);
        run.element = &element;
        Line line;
        line.height = line_h;
        line.runs.push_back(std::move(run));
        box->lines.push_back(std::move(line));
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

      const float border_padding_w =
          box->border_left + box->border_right + box->padding_left + box->padding_right;
      float content_width;
      if (box->style.width.has_value()) {
        content_width = SpecToContent(
            box->style.width.value(), containing_width, border_padding_w, box->style.box_sizing);
      } else {
        const float extras = box->margin_left + box->margin_right + border_padding_w;
        const float available = std::max(0.0f, containing_width - extras);
        const IntrinsicWidths w = MeasureContent(element, styles, registry);
        content_width = std::min(std::max(w.min, available), w.max);
      }
      box->width = content_width + border_padding_w;

      // Horizontal placement: left float at the containing block's left edge,
      // right float at the right edge (aligned so its right margin box meets
      // the right edge).  The border box origin sits at the margin edge (the
      // same convention as in-flow blocks); border/padding extend right/down
      // from there, so they are not subtracted.
      if (box->style.floating == style::Float::kLeft) {
        box->x = left_edge + box->margin_left;
      } else {
        box->x = left_edge + containing_width - box->margin_right - box->width;
      }
      box->y = float_y + box->margin_top;

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
      const float border_padding_h =
          box->border_top + box->border_bottom + box->padding_top + box->padding_bottom;
      if (box->style.height.has_value() && !box->style.height.value().percent) {
        content_height = SpecToContent(
            box->style.height.value(), containing_width, border_padding_h, box->style.box_sizing);
      } else if (box->style.aspect_ratio.has_value() && box->style.width.has_value()) {
        content_height = content_width / box->style.aspect_ratio.value();
      }
      box->height = content_height + border_padding_h;
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
      // Form controls render as atomic inline boxes with value/placeholder.
      if (child_element.tag_name() == "input" || child_element.tag_name() == "textarea" ||
          child_element.tag_name() == "select") {
        auto block_box = BuildFormControl(child_element, containing_width);
        InlineItem item;
        item.style = &child_style;
        item.element = &child_element;
        item.atomic = true;
        item.width = block_box->margin_left + block_box->width + block_box->margin_right;
        item.height = block_box->margin_top + block_box->height + block_box->margin_bottom;
        item.baseline_offset = block_box->height;
        item.block_box = std::move(block_box);
        items.push_back(std::move(item));
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
                             std::vector<dom::Element*>& absolute_children,
                             float percent_base_h = 0)
    {
      // A flex container's children are flex items, not normal-flow content.
      if (box.style.display == style::Display::kFlex ||
          box.style.display == style::Display::kInlineFlex) {
        return LayoutFlexContent(
            box, element, avail_width, cb_x, cb_y, cb_w, cb_h, absolute_children);
      }
      // A grid container's children are grid items, not normal-flow content.
      if (box.style.display == style::Display::kGrid) {
        return LayoutGridContent(
            box, element, avail_width, cb_x, cb_y, cb_w, cb_h, absolute_children);
      }
      float cursor_y = 0;
      std::vector<InlineItem> inline_items;
      // Block-level (and table) children are laid out after the inline content
      // so that their vertical position accounts for the preceding text lines.
      struct BlockChild
      {
        dom::Element* element;
        style::ComputedStyle style;
        bool table = false;
      };
      std::vector<BlockChild> block_children;
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
            child_style.display == style::Display::kListItem ||
            child_style.display == style::Display::kFlex ||
            child_style.display == style::Display::kGrid) {
          block_children.push_back(BlockChild{&child_element, child_style, /*table=*/false});
        } else if (child_style.display == style::Display::kTable) {
          block_children.push_back(BlockChild{&child_element, child_style, /*table=*/true});
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
      // Block-level children come after the inline content.
      cursor_y += lines_height;
      for (const BlockChild& bc : block_children) {
        std::vector<const LayoutBox*> cur = parent_floats;
        for (const auto& f : box.floats) {
          cur.push_back(f.get());
        }
        std::unique_ptr<LayoutBox> child_box;
        if (bc.table) {
          child_box = BuildTable(*bc.element,
                                 avail_width,
                                 box.content_x(),
                                 box.content_y() + cursor_y,
                                 cb_x,
                                 cb_y,
                                 cb_w,
                                 cb_h);
        } else {
          child_box = BuildBlock(*bc.element,
                                 avail_width,
                                 box.content_x(),
                                 box.content_y() + cursor_y,
                                 cb_x,
                                 cb_y,
                                 cb_w,
                                 cb_h,
                                 cur,
                                 /*percent_base_h=*/percent_base_h);
        }
        cursor_y += child_box->margin_top + child_box->height + child_box->margin_bottom;
        box.children.push_back(std::move(child_box));
      }
      // Floats expand the containing block: its height reaches at least the
      // bottom of every float it holds (the float is out of flow, so its
      // height is not otherwise counted here).
      float bottom = cursor_y;
      for (const auto& f : box.floats) {
        bottom = std::max(bottom, (f->y - box.content_y()) + f->height);
      }

      // A list item paints its marker (bullet / ordinal) in the reserved
      // gutter at the start of its first line.  The marker is a text run in
      // the gutter between the list's padding and the content.
      if (box.style.display == style::Display::kListItem &&
          box.style.list_style_type != style::ListStyleType::kNone) {
        const std::string marker =
            ListMarkerText(box.style.list_style_type, ListItemOrdinal(element));
        if (!marker.empty()) {
          TextRun run;
          run.text = marker;
          run.font_family = box.style.font_family;
          run.font_weight = box.style.font_weight;
          run.font_italic = box.style.font_italic;
          run.font_size = box.style.font_size;
          run.color = box.style.color.value_or(css::Color{0, 0, 0, 255});
          run.element = &element;
          run.width = MeasureTextWidth(
              registry, run.font_family, run.font_weight, run.font_italic, run.text, run.font_size);
          // Align the marker's top with the first line of content.
          float marker_y = box.content_y() + box.style.line_height * 0.15f;
          for (const Line& line : box.lines) {
            if (!line.runs.empty()) {
              marker_y = line.runs.front().y;
              break;
            }
          }
          run.x = box.content_x() - kListMarkerGap;
          run.y = marker_y;
          if (box.lines.empty()) {
            Line empty;
            empty.height = box.style.line_height;
            box.lines.push_back(std::move(empty));
          }
          box.lines.front().runs.insert(box.lines.front().runs.begin(), std::move(run));
        }
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
                                          const std::vector<const LayoutBox*>& parent_floats,
                                          float percent_base_h = 0)
    {
      auto box = std::make_unique<LayoutBox>();
      box->element = &element;
      box->style = styles.StyleFor(element);
      // CSS background-image: carry the URL and resolve the decoded image
      // through the ImageProvider (the browser fetches it keyed by element,
      // the same mechanism as <img>).
      if (box->style.background_image.has_value()) {
        box->background_image_url = box->style.background_image.value();
        if (images != nullptr) {
          box->background_image = images->Find(element);
        }
      }
      ResolveBoxEdges(*box, containing_width);

      // A list item reserves space for its marker on the left: the marker box
      // is placed in the gap between the list's padding and the item content.
      if (box->style.display == style::Display::kListItem &&
          box->style.list_style_type != style::ListStyleType::kNone) {
        box->padding_left += kListMarkerGap;
      }

      // Width: explicit (px or %) or fill the containing block.
      const float border_padding_w =
          box->border_left + box->border_right + box->padding_left + box->padding_right;
      float content_width;
      if (box->style.width.has_value()) {
        // box-sizing: with border-box the specified width covers border+padding.
        content_width = SpecToContent(
            box->style.width.value(), containing_width, border_padding_w, box->style.box_sizing);
      } else {
        content_width = containing_width - box->margin_left - box->margin_right - border_padding_w;
        if (content_width < 0) {
          content_width = 0;
        }
      }
      // min/max-width clamp (CSS 2.2 §10.4); min wins over max.  The clamps
      // constrain the same box as the width (content-box or border-box).
      if (box->style.min_width.has_value()) {
        content_width = std::max(
            content_width,
            SpecToContent(box->style.min_width.value(),
                          containing_width,
                          border_padding_w,
                          box->style.box_sizing));
      }
      if (box->style.max_width.has_value()) {
        content_width = std::min(
            content_width,
            SpecToContent(box->style.max_width.value(),
                          containing_width,
                          border_padding_w,
                          box->style.box_sizing));
      }
      box->width = content_width + border_padding_w;

      // Auto horizontal margins center the box within its containing block.
      CenterWithAutoMargins(*box, containing_width);

      // Border-box position, including the relative offset.  |origin_x| is the
      // content-box origin of the parent; the border box sits at this box's
      // margin edge, so border/padding extend right/down from there (they are
      // not subtracted — doing so pushed padded/bordered boxes off-canvas).
      float rel_x = 0;
      float rel_y = 0;
      if (box->style.position == style::Position::kRelative) {
        rel_x = box->style.left;
        rel_y = box->style.top;
      }
      box->x = origin_x + box->margin_left + rel_x;
      box->y = origin_y + box->margin_top + rel_y;

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

      const float border_padding_h =
          box->border_top + box->border_bottom + box->padding_top + box->padding_bottom;

      // Definite content height of this box, when it has an explicit height
      // that can resolve now.  It becomes the percentage-height basis for the
      // box's block children (CSS 2.2 §10.5: a percentage height resolves
      // against the containing block only when that height is definite).
      float definite_content_height = 0;
      if (box->style.height.has_value()) {
        if (box->style.height.value().percent) {
          if (percent_base_h > 0) {
            definite_content_height = std::max(0.0f,
                                               SpecToContent(box->style.height.value(),
                                                             percent_base_h,
                                                             border_padding_h,
                                                             box->style.box_sizing));
          }
        } else {
          definite_content_height = std::max(0.0f,
                                             SpecToContent(box->style.height.value(),
                                                           containing_width,
                                                           border_padding_h,
                                                           box->style.box_sizing));
        }
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
                                                absolute_children,
                                                /*percent_base_h=*/definite_content_height);

      if (box->style.height.has_value()) {
        if (box->style.height.value().percent) {
          if (percent_base_h > 0) {
            content_height = std::max(content_height, definite_content_height);
          }
          // else: a percentage height against an auto containing block is auto.
        } else {
          content_height = std::max(content_height,
                                    SpecToContent(box->style.height.value(),
                                                  containing_width,
                                                  border_padding_h,
                                                  box->style.box_sizing));
        }
      } else if (box->style.aspect_ratio.has_value() && box->style.width.has_value()) {
        content_height = content_width / box->style.aspect_ratio.value();
      }
      box->height = content_height + border_padding_h;

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

      const float border_padding_w =
          box->border_left + box->border_right + box->padding_left + box->padding_right;
      float content_width;
      if (forced_content_width.has_value()) {
        // The flex algorithm's main size is a content-box size.
        content_width = forced_content_width.value();
      } else if (box->style.width.has_value()) {
        const style::SizeSpec& width = box->style.width.value();
        content_width =
            SpecToContent(width, containing_width, border_padding_w, box->style.box_sizing);
      } else {
        content_width = containing_width - box->margin_left - box->margin_right - border_padding_w;
        if (content_width < 0) {
          content_width = 0;
        }
      }
      // min/max-width clamp (CSS 2.2 §10.4); min wins over max.  The clamps
      // constrain the same box as the width (content-box or border-box).
      if (box->style.min_width.has_value()) {
        content_width = std::max(content_width,
                                 SpecToContent(box->style.min_width.value(),
                                               containing_width,
                                               border_padding_w,
                                               box->style.box_sizing));
      }
      if (box->style.max_width.has_value()) {
        content_width = std::min(content_width,
                                 SpecToContent(box->style.max_width.value(),
                                               containing_width,
                                               border_padding_w,
                                               box->style.box_sizing));
      }
      if (box->style.min_width.has_value()) {
        content_width = std::max(content_width,
                                 SpecToContent(box->style.min_width.value(),
                                               containing_width,
                                               border_padding_w,
                                               box->style.box_sizing));
      }
      box->width = content_width + border_padding_w;
      // Margin-box origin at (0,0); content is placed at +border+padding.
      box->x = box->margin_left - box->border_left - box->padding_left;
      box->y = box->margin_top - box->border_top - box->padding_top;

      const float avail = box->width - box->border_left - box->border_right - box->padding_left -
                          box->padding_right;
      std::vector<dom::Element*> absolute_children;
      float content_height = LayoutBlockContent(
          *box, element, avail, cb_x, cb_y, cb_w, cb_h, kNoFloats, absolute_children);
      const float border_padding_h =
          box->border_top + box->border_bottom + box->padding_top + box->padding_bottom;
      if (forced_content_height.has_value()) {
        content_height = forced_content_height.value();
      } else if (box->style.height.has_value()) {
        const style::SizeSpec& height = box->style.height.value();
        if (!height.percent) {
          content_height = std::max(
              content_height,
              SpecToContent(height, containing_width, border_padding_h, box->style.box_sizing));
        }
      }
      // min/max-height clamp (CSS 2.2 §10.4); min wins over max.
      if (box->style.min_height.has_value()) {
        content_height = std::max(content_height,
                                  SpecToContent(box->style.min_height.value(),
                                                containing_width,
                                                border_padding_h,
                                                box->style.box_sizing));
      }
      if (box->style.max_height.has_value()) {
        content_height = std::min(content_height,
                                  SpecToContent(box->style.max_height.value(),
                                                containing_width,
                                                border_padding_h,
                                                box->style.box_sizing));
      }
      if (box->style.min_height.has_value()) {
        content_height = std::max(content_height,
                                  SpecToContent(box->style.min_height.value(),
                                                containing_width,
                                                border_padding_h,
                                                box->style.box_sizing));
      }
      box->height = content_height + border_padding_h;
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
      float base_main = 0;      // flex base size (content-box)
      float min_main = 0;       // clamp floor when shrinking
      float min_main_clamp = 0; // resolved min-width/height on the main axis
      float max_main_clamp = 0; // resolved max-width/height on the main axis (0 = none)
      float content_main = 0;   // final content-box main size
      float content_cross = 0;  // natural content-box cross size
      bool base_from_spec = false;
      bool cross_auto = true;        // no explicit cross-size property
      int auto_main_margins = 0;     // number of auto margins on the main axis
      int auto_cross_margins = 0;    // number of auto margins on the cross axis
      bool auto_main_start = false;  // an auto margin on the main-start side
      bool auto_cross_start = false; // an auto margin on the cross-start side
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
          item.auto_main_margins = (s.margin_left_auto ? 1 : 0) + (s.margin_right_auto ? 1 : 0);
          item.auto_cross_margins = (s.margin_top_auto ? 1 : 0) + (s.margin_bottom_auto ? 1 : 0);
          item.auto_main_start = s.margin_left_auto;
          item.auto_cross_start = s.margin_top_auto;
        } else {
          item.border_padding_main =
              edges.border_top + edges.border_bottom + edges.padding_top + edges.padding_bottom;
          item.margin_main = edges.margin_top + edges.margin_bottom;
          item.border_padding_cross =
              edges.border_left + edges.border_right + edges.padding_left + edges.padding_right;
          item.margin_cross = edges.margin_left + edges.margin_right;
          item.auto_main_margins = (s.margin_top_auto ? 1 : 0) + (s.margin_bottom_auto ? 1 : 0);
          item.auto_cross_margins = (s.margin_left_auto ? 1 : 0) + (s.margin_right_auto ? 1 : 0);
          item.auto_main_start = s.margin_top_auto;
          item.auto_cross_start = s.margin_left_auto;
        }
        // Resolved min/max sizes on the main axis (clamp the flexible main
        // size; percentages resolve against the container content width).
        const style::SizeSpec* min_main_spec = nullptr;
        const style::SizeSpec* max_main_spec = nullptr;
        if (row) {
          if (s.min_width.has_value()) {
            min_main_spec = &s.min_width.value();
          }
          if (s.max_width.has_value()) {
            max_main_spec = &s.max_width.value();
          }
        } else {
          if (s.min_height.has_value()) {
            min_main_spec = &s.min_height.value();
          }
          if (s.max_height.has_value()) {
            max_main_spec = &s.max_height.value();
          }
        }
        if (min_main_spec != nullptr) {
          item.min_main_clamp =
              SpecToContent(*min_main_spec, avail_width, item.border_padding_main, s.box_sizing);
        }
        if (max_main_spec != nullptr) {
          item.max_main_clamp =
              SpecToContent(*max_main_spec, avail_width, item.border_padding_main, s.box_sizing);
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
          item.base_main =
              SpecToContent(*main_spec, avail_width, item.border_padding_main, s.box_sizing);
          item.base_from_spec = true;
        } else if (row) {
          item.base_main = MeasureContent(child_el, styles, registry).max;
        }
        if (row) {
          // Shrink floor: the item's min-content main size, but never below
          // the resolved min-width (CSS Flexbox 1 §9.7).
          item.min_main = MeasureContent(child_el, styles, registry).min;
          if (item.min_main_clamp > item.min_main) {
            item.min_main = item.min_main_clamp;
          }
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
            cross_width = SpecToContent(item.style->width.value(),
                                        avail_width,
                                        item.border_padding_cross,
                                        item.style->box_sizing);
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
          item.min_main = item.min_main_clamp;
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
    // clamped at the item's min-content main size.  Resolved min/max main
    // sizes are applied last (min wins over max, matching CSS 2.2 §10.4).
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
        // Apply the resolved min/max constraints on the final main size.
        for (FlexItemData* it : line.items) {
          if (it->max_main_clamp > 0 && it->content_main > it->max_main_clamp) {
            it->content_main = it->max_main_clamp;
          }
          if (it->min_main > 0 && it->content_main < it->min_main) {
            it->content_main = it->min_main;
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
      // height, else auto (content-determined).  Percent heights are
      // indefinite (they depend on the containing block's resolved height,
      // which the engine does not track) and fall back to auto; treating them
      // as a definite 0 collapses the container.
      const float container_bp_h =
          box.border_top + box.border_bottom + box.padding_top + box.padding_bottom;
      float container_main = avail_width;
      if (!row && cs.height.has_value() && !cs.height.value().percent) {
        container_main =
            SpecToContent(cs.height.value(), avail_width, container_bp_h, cs.box_sizing);
      }
      const bool main_definite = row || (cs.height.has_value() && !cs.height.value().percent);
      // Container cross size: for a column it is the (definite) content
      // width; for a row it is the specified content height, else auto.
      float container_cross = avail_width;
      if (row && cs.height.has_value() && !cs.height.value().percent) {
        container_cross =
            SpecToContent(cs.height.value(), avail_width, container_bp_h, cs.box_sizing);
      }
      const bool cross_definite = !row || (cs.height.has_value() && !cs.height.value().percent);

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

      // ---- Ordering (CSS Flexbox 1 §5.4) ----
      // Items are laid out in ascending |order|; a stable sort keeps document
      // order for equal values (this affects packing, wrapping and the
      // painting order of the finished boxes).
      std::stable_sort(
          items.begin(), items.end(), [](const FlexItemData& a, const FlexItemData& b) {
            return a.style->order < b.style->order;
          });

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
          // Single-line containers fall back to flex-start (CSS Flexbox 1
          // §8.4); dividing by (lines-1) with fewer than two lines would
          // yield infinity / underflow.
          if (lines.size() >= 2) {
            gap = cross_gap + align_extra / static_cast<float>(lines.size() - 1);
          }
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
        // Auto main margins absorb the positive free space and take
        // precedence over justify-content (CSS Flexbox 1 §8.1); each auto
        // margin on the line receives an equal share.
        int auto_main_total = 0;
        for (const FlexItemData* it : line.items) {
          auto_main_total += it->auto_main_margins;
        }
        const float auto_main_share =
            (free > 0 && auto_main_total > 0) ? free / static_cast<float>(auto_main_total) : 0.0f;
        // With negative free space the line overflows the container; every
        // justify-content value then packs items toward main-start with
        // overflow at main-end (CSS Flexbox 1 §8.2), so the distribution is
        // only applied when there is space to distribute (and no auto
        // margins are consuming it).
        if (free > 0 && auto_main_total == 0) {
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
        for (const FlexItemData* it : line.items) {
          if (it->style->align_self.value_or(cs.align_items) == style::AlignItems::kBaseline) {
            line_baseline = std::max(line_baseline, baseline_of(*it->box));
          }
        }

        for (FlexItemData* it : line.items) {
          // Effective cross alignment: align-self overrides align-items.
          const style::AlignItems eff_align = it->style->align_self.value_or(cs.align_items);
          // The item's effective main margin includes its share of the free
          // space distributed to auto margins.
          const float eff_margin_main =
              it->margin_main + auto_main_share * static_cast<float>(it->auto_main_margins);
          // Cross size: stretch items fill the line (auto cross margins
          // override stretch, CSS Flexbox 1 §8.1).
          if (it->auto_cross_margins == 0 && eff_align == style::AlignItems::kStretch &&
              it->cross_auto) {
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
          const float outer_main = it->content_main + it->border_padding_main + eff_margin_main;

          // Cross offset within the line.  Auto cross margins absorb the
          // line's free cross space and override align-self.
          float cross_pos = 0;
          if (it->auto_cross_margins > 0) {
            const float free_cross = line_cross[li] - outer_cross;
            if (free_cross > 0) {
              const float share = free_cross / static_cast<float>(it->auto_cross_margins);
              if (it->auto_cross_start) {
                cross_pos += share;
              }
            }
          } else {
            switch (eff_align) {
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
          }

          // Margin-box origin in container content coordinates.  An auto
          // margin on the item's main-start side absorbs one share of the
          // free space, pushing the item along the main axis.
          if (it->auto_main_start) {
            main_cursor += auto_main_share;
          }
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

    // Lays out |element|'s children as grid items (CSS Grid Layout 1 §7–10)
    // into |box| (whose border-box width is already set).  Supports explicit
    // grid-template-columns/rows (px/%/fr/auto/min-content/max-content, plus
    // repeat()), row-major auto placement, grid-column/row line + span
    // placement, and row/column gaps.  Tracks beyond the explicit template
    // (implicit) are auto-sized.  Returns the container's content height.
    float LayoutGridContent(LayoutBox& box,
                            dom::Element& element,
                            float avail_width,
                            float cb_x,
                            float cb_y,
                            float cb_w,
                            float cb_h,
                            std::vector<dom::Element*>& absolute_children)
    {
      const style::ComputedStyle& cs = box.style;
      const float column_gap = cs.column_gap;
      const float row_gap = cs.row_gap;
      const float container_width = avail_width;
      // The container content height is definite only for an explicit
      // non-percentage height (auto/percent rows then resolve like auto).
      const float container_bp_h =
          box.border_top + box.border_bottom + box.padding_top + box.padding_bottom;
      const bool height_definite = cs.height.has_value() && !cs.height.value().percent;
      const float container_height =
          height_definite
              ? SpecToContent(cs.height.value(), avail_width, container_bp_h, cs.box_sizing)
              : 0.0f;

      // One in-flow grid item with its resolved placement.
      struct GridItem
      {
        dom::Element* element = nullptr;
        const style::ComputedStyle* style = nullptr;
        int col_start = 0; // 0-based
        int col_span = 1;
        int row_start = 0;
        int row_span = 1;
        std::unique_ptr<LayoutBox> layout;
      };
      std::vector<GridItem> items;

      for (dom::Node* child : element.ChildNodes()) {
        if (child->node_type() != dom::NodeType::kElement) {
          continue;
        }
        auto* el = static_cast<dom::Element*>(child);
        const style::ComputedStyle& s = styles.StyleFor(*el);
        if (s.display == style::Display::kNone) {
          continue;
        }
        if (s.position == style::Position::kAbsolute || s.position == style::Position::kFixed) {
          absolute_children.push_back(el);
          continue;
        }
        GridItem item;
        item.element = el;
        item.style = &s;
        // Column placement: explicit start line, span or end-line end.
        if (s.grid_column_start.kind == style::GridPlacement::Kind::kLine) {
          item.col_start = std::max(0, s.grid_column_start.line - 1);
        }
        if (s.grid_column_end.kind == style::GridPlacement::Kind::kSpan) {
          item.col_span = std::max(1, s.grid_column_end.span);
        } else if (s.grid_column_end.kind == style::GridPlacement::Kind::kLine) {
          item.col_span = std::max(1, s.grid_column_end.line - s.grid_column_start.line);
        }
        // Row placement.
        if (s.grid_row_start.kind == style::GridPlacement::Kind::kLine) {
          item.row_start = std::max(0, s.grid_row_start.line - 1);
        }
        if (s.grid_row_end.kind == style::GridPlacement::Kind::kSpan) {
          item.row_span = std::max(1, s.grid_row_end.span);
        } else if (s.grid_row_end.kind == style::GridPlacement::Kind::kLine) {
          item.row_span = std::max(1, s.grid_row_end.line - s.grid_row_start.line);
        }
        items.push_back(std::move(item));
      }
      if (items.empty()) {
        return 0.0f;
      }

      const std::size_t explicit_cols = cs.grid_template_columns.size();
      const std::size_t explicit_rows = cs.grid_template_rows.size();

      // ---- Auto placement (grid-auto-flow: row) ----
      // occupied[r][c] marks a claimed cell; columns beyond the explicit
      // template are implicit (auto-sized).
      std::vector<std::vector<bool>> occupied;
      auto ensure_cells = [&](int rows, int cols) {
        if (static_cast<int>(occupied.size()) < rows) {
          occupied.resize(static_cast<std::size_t>(rows));
        }
        for (auto& row : occupied) {
          if (static_cast<int>(row.size()) < cols) {
            row.resize(static_cast<std::size_t>(cols), false);
          }
        }
      };
      const auto cells_free = [&](int row, int col, int col_span, int row_span) {
        for (int r2 = row; r2 < row + row_span; ++r2) {
          for (int c = col; c < col + col_span; ++c) {
            if (occupied[static_cast<std::size_t>(r2)][static_cast<std::size_t>(c)]) {
              return false;
            }
          }
        }
        return true;
      };
      const auto claim_cells = [&](int row, int col, int col_span, int row_span) {
        ensure_cells(row + row_span, col + col_span);
        for (int r2 = row; r2 < row + row_span; ++r2) {
          for (int c = col; c < col + col_span; ++c) {
            occupied[static_cast<std::size_t>(r2)][static_cast<std::size_t>(c)] = true;
          }
        }
      };
      int grid_rows = 0;
      int grid_cols = static_cast<int>(explicit_cols);
      for (GridItem& item : items) {
        const int col_span = std::max(1, item.col_span);
        const int row_span = std::max(1, item.row_span);
        const bool has_explicit_col =
            item.style->grid_column_start.kind == style::GridPlacement::Kind::kLine;
        bool placed = false;
        if (has_explicit_col) {
          const int col = item.col_start;
          for (int row = 0; row <= grid_rows; ++row) {
            ensure_cells(row + row_span, col + col_span);
            if (cells_free(row, col, col_span, row_span)) {
              claim_cells(row, col, col_span, row_span);
              item.row_start = row;
              placed = true;
              break;
            }
          }
          if (!placed) {
            // No room in an existing row: start a new row at the column.
            ensure_cells(grid_rows + row_span, col + col_span);
            claim_cells(grid_rows, col, col_span, row_span);
            item.row_start = grid_rows;
            placed = true;
          }
          item.col_start = col;
        } else {
          // Auto column: fill the current grid's columns row by row, wrapping
          // to new rows; auto items never extend the column count.
          const int cols = std::max(grid_cols, 1);
          for (int row = 0; row <= grid_rows; ++row) {
            ensure_cells(row + row_span, cols + col_span);
            for (int col = 0; col + col_span <= cols; ++col) {
              if (cells_free(row, col, col_span, row_span)) {
                claim_cells(row, col, col_span, row_span);
                item.col_start = col;
                item.row_start = row;
                placed = true;
                break;
              }
            }
            if (placed) {
              break;
            }
          }
          if (!placed) {
            ensure_cells(grid_rows + row_span, cols + col_span);
            claim_cells(grid_rows, 0, col_span, row_span);
            item.col_start = 0;
            item.row_start = grid_rows;
            placed = true;
          }
        }
        grid_rows = std::max(grid_rows, item.row_start + row_span);
        grid_cols = std::max(grid_cols, item.col_start + col_span);
      }

      // ---- Column track sizing ----
      std::vector<float> col_sizes(static_cast<std::size_t>(grid_cols), 0.0f);
      // Phase 1: fixed tracks.
      for (int c = 0; c < grid_cols; ++c) {
        const style::GridTrack* t = c < static_cast<int>(explicit_cols)
                                        ? &cs.grid_template_columns[static_cast<std::size_t>(c)]
                                        : nullptr;
        if (t != nullptr && t->kind == style::GridTrack::Kind::kFixed) {
          col_sizes[static_cast<std::size_t>(c)] =
              t->percent > 0 ? container_width * t->percent / 100.0f : t->length;
        }
      }
      // Phase 2: content tracks (auto/min-content/max-content and implicit
      // tracks) sized to the items starting in each column (a spanning item
      // contributes to its start column only — documented simplification).
      for (int c = 0; c < grid_cols; ++c) {
        const style::GridTrack* t = c < static_cast<int>(explicit_cols)
                                        ? &cs.grid_template_columns[static_cast<std::size_t>(c)]
                                        : nullptr;
        style::GridTrack::Kind kind = t != nullptr ? t->kind : style::GridTrack::Kind::kAuto;
        if (kind == style::GridTrack::Kind::kFixed || kind == style::GridTrack::Kind::kFr) {
          continue;
        }
        for (const GridItem& item : items) {
          if (item.col_start != c) {
            continue;
          }
          const IntrinsicWidths w = MeasureContent(*item.element, styles, registry);
          col_sizes[static_cast<std::size_t>(c)] =
              std::max(col_sizes[static_cast<std::size_t>(c)],
                       kind == style::GridTrack::Kind::kMinContent ? w.min : w.max);
        }
      }
      // Phase 3: fr tracks share the leftover space.
      {
        float used = 0;
        for (int c = 0; c < grid_cols; ++c) {
          used += col_sizes[static_cast<std::size_t>(c)];
        }
        used += column_gap * static_cast<float>(std::max(0, grid_cols - 1));
        float total_fr = 0;
        for (int c = 0; c < grid_cols; ++c) {
          const style::GridTrack* t = c < static_cast<int>(explicit_cols)
                                          ? &cs.grid_template_columns[static_cast<std::size_t>(c)]
                                          : nullptr;
          if (t != nullptr && t->kind == style::GridTrack::Kind::kFr) {
            total_fr += t->fr;
          }
        }
        if (total_fr > 0) {
          const float leftover = std::max(0.0f, container_width - used);
          for (int c = 0; c < grid_cols; ++c) {
            const style::GridTrack* t = c < static_cast<int>(explicit_cols)
                                            ? &cs.grid_template_columns[static_cast<std::size_t>(c)]
                                            : nullptr;
            if (t != nullptr && t->kind == style::GridTrack::Kind::kFr) {
              col_sizes[static_cast<std::size_t>(c)] = leftover * t->fr / total_fr;
            }
          }
        }
      }

      // The width of an item's grid area (its spanned columns plus gaps).
      const auto cell_width = [&](const GridItem& item) -> float {
        float w = 0;
        for (int c = item.col_start; c < item.col_start + item.col_span; ++c) {
          if (c > item.col_start) {
            w += column_gap;
          }
          w += col_sizes[static_cast<std::size_t>(c)];
        }
        return w;
      };

      // Build each item's box at the grid-area width (local origin; the box
      // is translated into place after the rows are sized).
      for (GridItem& item : items) {
        item.layout =
            BuildBlock(*item.element, cell_width(item), 0, 0, cb_x, cb_y, cb_w, cb_h, kNoFloats);
      }

      // ---- Row track sizing ----
      std::vector<float> row_sizes(static_cast<std::size_t>(grid_rows), 0.0f);
      for (int r = 0; r < grid_rows; ++r) {
        const style::GridTrack* t = r < static_cast<int>(explicit_rows)
                                        ? &cs.grid_template_rows[static_cast<std::size_t>(r)]
                                        : nullptr;
        if (t != nullptr && t->kind == style::GridTrack::Kind::kFixed) {
          row_sizes[static_cast<std::size_t>(r)] = t->percent > 0 && height_definite
                                                       ? container_height * t->percent / 100.0f
                                                       : t->length;
        }
      }
      for (int r = 0; r < grid_rows; ++r) {
        const style::GridTrack* t = r < static_cast<int>(explicit_rows)
                                        ? &cs.grid_template_rows[static_cast<std::size_t>(r)]
                                        : nullptr;
        style::GridTrack::Kind kind = t != nullptr ? t->kind : style::GridTrack::Kind::kAuto;
        if (kind == style::GridTrack::Kind::kFixed || kind == style::GridTrack::Kind::kFr) {
          continue;
        }
        for (const GridItem& item : items) {
          if (item.row_start != r) {
            continue;
          }
          const float outer =
              item.layout->margin_top + item.layout->height + item.layout->margin_bottom;
          row_sizes[static_cast<std::size_t>(r)] =
              std::max(row_sizes[static_cast<std::size_t>(r)], outer);
        }
      }
      if (height_definite) {
        float used = 0;
        for (int r = 0; r < grid_rows; ++r) {
          used += row_sizes[static_cast<std::size_t>(r)];
        }
        used += row_gap * static_cast<float>(std::max(0, grid_rows - 1));
        float total_fr = 0;
        for (int r = 0; r < grid_rows; ++r) {
          const style::GridTrack* t = r < static_cast<int>(explicit_rows)
                                          ? &cs.grid_template_rows[static_cast<std::size_t>(r)]
                                          : nullptr;
          if (t != nullptr && t->kind == style::GridTrack::Kind::kFr) {
            total_fr += t->fr;
          }
        }
        if (total_fr > 0) {
          const float leftover = std::max(0.0f, container_height - used);
          for (int r = 0; r < grid_rows; ++r) {
            const style::GridTrack* t = r < static_cast<int>(explicit_rows)
                                            ? &cs.grid_template_rows[static_cast<std::size_t>(r)]
                                            : nullptr;
            if (t != nullptr && t->kind == style::GridTrack::Kind::kFr) {
              row_sizes[static_cast<std::size_t>(r)] = leftover * t->fr / total_fr;
            }
          }
        }
      }

      // ---- Track offsets and item placement ----
      std::vector<float> col_offsets(static_cast<std::size_t>(grid_cols), 0.0f);
      {
        float x = 0;
        for (int c = 0; c < grid_cols; ++c) {
          col_offsets[static_cast<std::size_t>(c)] = x;
          x += col_sizes[static_cast<std::size_t>(c)] + column_gap;
        }
      }
      std::vector<float> row_offsets(static_cast<std::size_t>(grid_rows), 0.0f);
      {
        float y = 0;
        for (int r = 0; r < grid_rows; ++r) {
          row_offsets[static_cast<std::size_t>(r)] = y;
          y += row_sizes[static_cast<std::size_t>(r)] + row_gap;
        }
      }
      for (GridItem& item : items) {
        const float cell_x = col_offsets[static_cast<std::size_t>(item.col_start)];
        const float cell_y = row_offsets[static_cast<std::size_t>(item.row_start)];
        TranslateBox(*item.layout, box.content_x() + cell_x, box.content_y() + cell_y);
        box.children.push_back(std::move(item.layout));
      }

      // Container content height: the stacked row tracks plus gaps.
      float content_height = 0;
      for (int r = 0; r < grid_rows; ++r) {
        content_height += row_sizes[static_cast<std::size_t>(r)];
        if (r + 1 < grid_rows) {
          content_height += row_gap;
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

      const float border_padding_w =
          box->border_left + box->border_right + box->padding_left + box->padding_right;
      const float extras = box->margin_left + box->margin_right + border_padding_w;
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
        content_width =
            SpecToContent(box->style.width.value(), cb_w, border_padding_w, box->style.box_sizing);
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
      box->width = content_width + border_padding_w;
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
      const float border_padding_h =
          box->border_top + box->border_bottom + box->padding_top + box->padding_bottom;
      if (box->style.height.has_value() && !box->style.height.value().percent) {
        content_height =
            std::max(content_height,
                     SpecToContent(
                         box->style.height.value(), cb_w, border_padding_h, box->style.box_sizing));
      }
      box->height = content_height + border_padding_h;

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

      // Table width.  An explicit width is honored directly; with width:auto
      // the table shrink-wraps its content (CSS2.1 17.5.2): the width is
      // max(min-content, min(max-content, available)).  The final value for
      // auto tables is computed below after the columns are measured.
      const float border_padding_w =
          table->border_left + table->border_right + table->padding_left + table->padding_right;
      const float available_content =
          containing_width - table->margin_left - table->margin_right - border_padding_w;
      float content_width;
      if (table->style.width.has_value()) {
        content_width = SpecToContent(table->style.width.value(),
                                      containing_width,
                                      border_padding_w,
                                      table->style.box_sizing);
      } else {
        content_width = std::max(0.0f, available_content);
      }
      table->width = content_width + border_padding_w;
      table->x = origin_x + table->margin_left - table->border_left - table->padding_left;
      table->y = origin_y + table->margin_top - table->border_top - table->padding_top;
      float table_x = table->content_x();
      float table_y = table->content_y();

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

      // width:auto tables shrink to fit their content (CSS2.1 17.5.2):
      // table width = max(min-content, min(max-content, available)).  This
      // keeps a small table from being stretched across the full containing
      // block, which would spread its columns with large gaps.
      if (!table->style.width.has_value()) {
        float min_content = 0; // widest single column content per column
        float max_content = 0; // natural (unwrapped) content width per column
        // Sum the per-column extrema of the non-spanning cells.
        std::vector<float> col_min(static_cast<std::size_t>(ncols), 0.0f);
        std::vector<float> col_max(static_cast<std::size_t>(ncols), 0.0f);
        for (const CellInfo& cell : cells) {
          if (cell.colspan != 1) {
            continue;
          }
          const std::size_t c = static_cast<std::size_t>(cell.col);
          if (c < col_min.size()) {
            col_min[c] = std::max(col_min[c], cell.min_width);
            col_max[c] = std::max(col_max[c], cell.max_width);
          }
        }
        for (const float w : col_min) {
          min_content += w;
        }
        for (const float w : col_max) {
          max_content += w;
        }
        // A caption's natural width also contributes to the table's
        // max-content width (the table must be wide enough to hold it).
        for (dom::Node* child : element.ChildNodes()) {
          if (child->node_type() != dom::NodeType::kElement) {
            continue;
          }
          const dom::Element& child_el = static_cast<const dom::Element&>(*child);
          if (styles.StyleFor(child_el).display == style::Display::kTableCaption) {
            max_content = std::max(max_content, MeasureContent(child_el, styles, registry).max);
            break;
          }
        }
        const float fit = std::min(std::max(min_content, available_content), max_content);
        content_width = std::max(0.0f, fit);
        table->width = content_width + border_padding_w;
      }

      // max-width clamp (CSS 2.2 §10.4) on the final width.
      if (table->style.max_width.has_value()) {
        const float max_w = SpecToContent(table->style.max_width.value(),
                                          containing_width,
                                          border_padding_w,
                                          table->style.box_sizing);
        content_width = std::min(content_width, std::max(0.0f, max_w));
        table->width = content_width + border_padding_w;
      }

      // Auto horizontal margins center the table within its containing block
      // (the width is final now; recompute x and the content origin used for
      // cells/caption from the resolved margin).
      CenterWithAutoMargins(*table, containing_width);
      table->x = origin_x + table->margin_left - table->border_left - table->padding_left;
      table_x = table->content_x();
      table_y = table->content_y();

      // The table's caption (display: table-caption), if any, is laid out as a
      // block above the rows at the table's final content width and becomes
      // the table box's first child.
      std::unique_ptr<LayoutBox> caption_box;
      float caption_height = 0;
      for (dom::Node* child : element.ChildNodes()) {
        if (child->node_type() != dom::NodeType::kElement) {
          continue;
        }
        dom::Element& child_el = static_cast<dom::Element&>(*child);
        if (styles.StyleFor(child_el).display != style::Display::kTableCaption) {
          continue;
        }
        caption_box = std::make_unique<LayoutBox>();
        caption_box->element = &child_el;
        caption_box->style = styles.StyleFor(child_el);
        ResolveBoxEdges(*caption_box, content_width);
        caption_box->width = content_width + caption_box->border_left + caption_box->border_right +
                             caption_box->padding_left + caption_box->padding_right;
        const float caption_avail = caption_box->width - caption_box->border_left -
                                    caption_box->border_right - caption_box->padding_left -
                                    caption_box->padding_right;
        std::vector<dom::Element*> caption_absolute;
        caption_height = LayoutBlockContent(*caption_box,
                                            child_el,
                                            caption_avail,
                                            cb_x,
                                            cb_y,
                                            cb_w,
                                            cb_h,
                                            kNoFloats,
                                            caption_absolute);
        caption_box->height = caption_height + caption_box->border_top +
                              caption_box->border_bottom + caption_box->padding_top +
                              caption_box->padding_bottom;
        // Lay out at a local origin (0,0), then translate the whole box --
        // including the text runs laid out by LayoutBlockContent -- so that
        // its content box starts at the table's content origin (the same
        // pattern LayoutCell uses).
        TranslateBox(*caption_box,
                     table_x - caption_box->border_left - caption_box->padding_left,
                     table_y - caption_box->border_top - caption_box->padding_top);
        for (dom::Element* abs_child : caption_absolute) {
          caption_box->positioned_children.push_back(
              BuildAbsolute(*abs_child, table_x, table_y, cb_w, cb_h, kNoFloats));
        }
        break; // only the first caption participates in the table model
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

      // Build the box tree (table -> [caption] -> rows -> cells) with grid
      // positions.  The caption sits above the rows.
      float y = 0;
      if (caption_box != nullptr) {
        const float caption_outer = caption_height + caption_box->border_top +
                                    caption_box->border_bottom + caption_box->padding_top +
                                    caption_box->padding_bottom;
        table->children.push_back(std::move(caption_box));
        y += caption_outer;
      }
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
                            viewport_height,
                            kNoFloats,
                            /*percent_base_h=*/viewport_height);
}

} // namespace neko::layout
