
// Replaced elements whose box comes from CSS size / presentational attributes /
// intrinsic size rather than from in-flow content.  <svg> belongs here: an
// inline SVG (e.g. a 18x18 icon with width/height attributes) is a replaced
// element per the HTML standard; sizing it as an empty inline box measured it
// as zero and collapsed icon slots in flex/intrinsic contexts.
static bool IsReplacedElement(std::string_view tag)
{
  return tag == "img" || tag == "video" || tag == "canvas" || tag == "svg";
}

namespace {

// Empty float list for block formatting contexts that do not inherit floats
// (inline-block, table cells, floats, absolute boxes start a fresh BFC).
const std::vector<const LayoutBox*> kNoFloats;

// Width of the marker gutter reserved on the left of every <li> content box.
// The marker (bullet / number) is drawn in this gutter, inside the list's
// padding-left (the UA stylesheet gives <ul>/<ol> a 40px padding).
constexpr float kListMarkerGap = 24.0f;

// Native widget boxes (WHATWG HTML §15.5 Widgets).  A widget's native
// appearance is implementation-defined -- the standard says only "Need to detail
// the native appearance" throughout the section -- so these are measured
// defaults instead of values the standard derives.  The ones in `*Em` scale with
// the control's font-size, which is what the measured browsers do; the others
// are fixed sizes.
constexpr float kCheckboxBoxSize = 13.0f;    // Chrome's default (Firefox 156
                                             // measures 14x14), frozen at 13x13 by
                                             // the form-control contract so paint
                                             // and hit-testing agree on one box
constexpr float kRangeWidthEm = 12.0f;       // Firefox: 12em at any font-size
constexpr float kRangeHeightEm = 1.5f;       // Firefox: ~1.5em (20px at 13.33px)
constexpr float kColorWellWidth = 64.0f;     // Firefox: a fixed 64x32, whatever the
constexpr float kColorWellHeight = 32.0f;    // font-size
constexpr float kFileWidthEm = 17.7f;        // Firefox: 17.8em at its field font
constexpr float kSelectArrowWidthEm = 1.35f; // Firefox: 18px of drop-down arrow
                                             // area at its 13.33px field font

// §15.5.6 / §4.10.5.3.5 defaults: a text control shows 20 characters when the
// `size` attribute is absent or invalid; §4.10.10.3 defaults a textarea to 20
// columns and 2 rows; §4.10.9's display size defaults to 1, and a list box whose
// `size` attribute is absent or invalid is four rows high (§15.5.16).
constexpr int kDefaultFieldSizeChars = 20;
constexpr int kDefaultTextAreaCols = 20;
constexpr int kDefaultTextAreaRows = 2;
constexpr int kDefaultSelectDisplaySize = 1;
constexpr int kDefaultListBoxRows = 4;

// Upper bound on the character sizes a control is laid out with.  `size`,
// `cols` and `rows` are remote input: without a cap, size=2000000000 asks for a
// box two billion pixels wide.  The style engine caps repeat()/grid counts the
// same way.
constexpr int kMaxFieldSizeChars = 1000;

// Width of the disclosure-marker gutter reserved on the left of a <summary>
// that is the disclosure control of a <details> (WHATWG HTML §4.11.1).  The
// triangle itself is painted by the paint layer, not by a font glyph, so the
// gutter only has to fit 7x7 css px plus breathing room.
constexpr float kDisclosureMarkerGap = 16.0f;

// The <details> element whose disclosure control is |summary|, or nullptr.
// Per §4.11.1 the disclosure control is the first <summary> element child of a
// <details>; a second <summary> is never one.
const dom::Element* DetailsOfSummaryControl(const dom::Element& summary)
{
  if (summary.tag_name() != "summary") {
    return nullptr;
  }
  const dom::Node* parent = summary.parent();
  if (parent == nullptr || parent->node_type() != dom::NodeType::kElement) {
    return nullptr;
  }
  const auto& details = static_cast<const dom::Element&>(*parent);
  if (details.tag_name() != "details") {
    return nullptr;
  }
  for (const dom::Node* child : details.ChildNodes()) {
    if (child->node_type() != dom::NodeType::kElement) {
      continue;
    }
    const auto& element = static_cast<const dom::Element&>(*child);
    if (element.tag_name() != "summary") {
      continue;
    }
    return &element == &summary ? &details : nullptr;
  }
  return nullptr;
}

// True when |child| of |parent| generates a box.  WHATWG HTML §4.11.1: a
// <details> without the open attribute renders only its disclosure control
// (its first <summary> element child) -- every other child, text nodes
// included, is not rendered.  Without a <summary> a closed <details> renders
// nothing at all.  This is the engine's rendering rule for details (the UA
// sheet cannot express it: the real sheet uses a ::details-content slot and
// content-visibility, neither of which this style engine has), so it is
// applied by every place that walks a box's children: block flow, flex, grid
// items and intrinsic-width measurement.
bool RendersChild(const dom::Element& parent, const dom::Node& child)
{
  if (parent.tag_name() != "details" || parent.HasAttribute("open")) {
    return true;
  }
  for (const dom::Node* candidate : parent.ChildNodes()) {
    if (candidate->node_type() != dom::NodeType::kElement) {
      continue;
    }
    const auto& element = static_cast<const dom::Element&>(*candidate);
    if (element.tag_name() == "summary") {
      return candidate == &child;
    }
  }
  return false; // no summary: nothing is rendered while closed
}

// The disclosure-marker gutter a summary box reserves.  Always reserved for a
// disclosure control: marker customization (`list-style-type`, `::marker`) is
// NOT IMPLEMENTED (see the UA sheet comment).
float DisclosureMarkerGutter(const dom::Element& element)
{
  return DetailsOfSummaryControl(element) != nullptr ? kDisclosureMarkerGap : 0.0f;
}

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

// An absolutely positioned child collected while laying out a parent's
// content, together with the vertical position it would occupy in normal
// flow.  |static_x|/|static_y| are the offsets from the absolute containing
// block origin to the margin edge of the box's hypothetical in-flow position
// (the "static position", CSS2.2 §10.3.7 / §10.6.4); they are used when the
// corresponding inset (left/right or top/bottom) is auto.  |static_x| is known
// at collection time; |static_y| depends on the in-flow content that precedes
// the element, so it is filled in during the block layout pass.
struct AbsoluteChild
{
  dom::Element* element = nullptr;
  float static_x = 0;
  float static_y = 0;
  // Number of in-flow block-level siblings preceding this element in document
  // order.  The flow cursor after them (hence |static_y|) is only known once
  // they have been laid out.
  std::size_t preceding_blocks = 0;
};

// CSS 2.1 section 8.3.1: two vertical margins are "adjoining" when no
// border, padding, line box, clearance or BFC root separates them, and they
// collapse to a single margin: the largest of the positive margins plus the
// most negative of the negative ones.
float CollapseAdjoiningMargins(float previous_bottom, float next_top)
{
  if (previous_bottom >= 0.0f && next_top >= 0.0f) {
    return std::max(previous_bottom, next_top);
  }
  if (previous_bottom <= 0.0f && next_top <= 0.0f) {
    return std::min(previous_bottom, next_top);
  }
  return previous_bottom + next_top;
}

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
// Number of UTF-8 code points in |text| (invalid bytes count as one each).
// The monospace fallback model advances one glyph per code point; counting
// bytes made every CJK character three advances wide.
std::size_t CountCodePoints(std::string_view text)
{
  std::size_t count = 0;
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t before = pos;
    char32_t cp = 0;
    if (!base::DecodeUtf8Next(text, pos, cp) && pos == before) {
      ++pos; // invalid byte: skip it so the loop always advances
    }
    ++count;
  }
  return count;
}

// for |family| when a registry is available; falls back to the monospace model
// (font_size per code point).
float MeasureTextWidth(const graphics::FontRegistry* registry,
                       std::string_view family,
                       int weight,
                       bool italic,
                       std::string_view text,
                       float font_size)
{
  if (registry == nullptr) {
    return static_cast<float>(CountCodePoints(text)) * font_size;
  }
  return registry->SelectorFor(std::string(family), weight, italic)->TextWidth(text, font_size);
}

// Width of the widest unbreakable unit in |text| (the min-content width).
// Line-breaking opportunities follow the same rules as inline layout: runs of
// non-space characters break at ASCII whitespace, and every East Asian
// ideograph (or fullwidth/CJK punctuation character) is its own unit (CSS Text
// 3 §5.1 treats CJK text as breakable between any two ideographs).  Splitting
// at whitespace only made every CJK run measure as one unbreakable word, which
// stopped flex items (min-width:auto) and other min-content consumers from
// shrinking a CJK title below its full text width -- bilibili's title h3 as a
// flex column then forced its card past the grid column width.
float WidestWordWidth(const graphics::FontRegistry* registry,
                      std::string_view family,
                      int weight,
                      bool italic,
                      std::string_view text,
                      float font_size)
{
  float widest = 0;
  std::size_t run_start = 0; // start of the unbreakable run being accumulated
  std::size_t scan = 0;
  while (scan < text.size()) {
    const std::size_t unit_start = scan;
    char32_t cp = 0;
    if (!base::DecodeUtf8Next(text, scan, cp)) {
      // Invalid byte: keep it inside the current run and step over it.
      if (scan == unit_start) {
        ++scan;
      }
      continue;
    }
    const bool space = cp < 0x80 && IsWordBreak(static_cast<char>(cp));
    const bool ideograph = IsCjkCodePoint(cp);
    if (!space && !ideograph) {
      continue; // part of the current run
    }
    // Flush the run that ends here (if any).
    if (unit_start > run_start) {
      widest = std::max(widest,
                        MeasureTextWidth(registry,
                                         family,
                                         weight,
                                         italic,
                                         text.substr(run_start, unit_start - run_start),
                                         font_size));
    }
    if (ideograph) {
      // A CJK breakable character is an unbreakable unit on its own.
      widest = std::max(widest,
                        MeasureTextWidth(registry,
                                         family,
                                         weight,
                                         italic,
                                         text.substr(unit_start, scan - unit_start),
                                         font_size));
    }
    run_start = scan;
  }
  if (run_start < text.size()) {
    widest = std::max(
        widest,
        MeasureTextWidth(registry, family, weight, italic, text.substr(run_start), font_size));
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

// Text as the layout must measure it: the preserving white-space values keep every
// space (CSS Text 3 §3), so the collapsing that normal / pre-line need must not run
// -- otherwise a <pre>'s intrinsic width is the width of its collapsed text and a
// table cell holding it is sized wrong.
std::string MeasuredText(std::string_view text, style::WhiteSpace white_space)
{
  if (white_space == style::WhiteSpace::kPre || white_space == style::WhiteSpace::kPreWrap) {
    return std::string(text);
  }
  return CollapseWhitespace(text);
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
// Percentages resolve against |containing_width| / |containing_height|; a
// percentage height with an indefinite base (|containing_height| <= 0) is
// treated as auto (CSS 2.2 §10.5), and the intrinsic ratio then carries it.
// Defined after ParseNonNegativeInt; declared here for CollectInline.
void ComputeReplacedSize(const style::ComputedStyle& style,
                         const dom::Element& element,
                         const image::Image* img,
                         float containing_width,
                         float containing_height,
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
                      const graphics::FontSelector* selector,
                      bool may_wrap = true) {
    const float word_width = selector != nullptr
                                 ? selector->TextWidth(word, style.font_size)
                                 : static_cast<float>(CountCodePoints(word)) * style.font_size;
    // Ensure the word fits on the current (or a lower) line; a word that
    // cannot fit even at a line start is pushed below any overlapping floats.
    // An item wider than the block itself is placed as-is (never dropped).
    while (may_wrap && x + word_width > line_right) {
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
    run.visible = style.visibility != style::Visibility::kHidden;
    run.underline = style.text_decoration_underline;
    run.element = element;
    // Inline element background + padding (CSS 2.2 §8.4.1/§10.1).  Percentages
    // resolve against the inline container's available width.
    if (style.background_color.has_value()) {
      run.background = *style.background_color;
      run.padding_left = ResolveSize(style.padding_left, available_width);
      run.padding_right = ResolveSize(style.padding_right, available_width);
      run.padding_top = ResolveSize(style.padding_top, available_width);
      run.padding_bottom = ResolveSize(style.padding_bottom, available_width);
    }
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
    // white-space (CSS Text 3 §3) decides what happens to whitespace here, and
    // this loop is where it happens: the inline items keep their source text and
    // the collapsing is done while laying out lines.
    //   pre / pre-wrap  keep every space, and a newline forces a line break;
    //   pre-line        collapses spaces but keeps the newlines;
    //   normal / nowrap collapse whitespace (nowrap also never wraps);
    //   pre             additionally never wraps: a long line overflows.
    const bool preserves_spaces = style.white_space == style::WhiteSpace::kPre ||
                                  style.white_space == style::WhiteSpace::kPreWrap;
    const bool preserves_newlines =
        preserves_spaces || style.white_space == style::WhiteSpace::kPreLine;
    const bool may_wrap = style.white_space != style::WhiteSpace::kNowrap &&
                          style.white_space != style::WhiteSpace::kPre;
    const float space_advance =
        selector != nullptr ? selector->Advance(' ', style.font_size) : style.font_size * 0.5f;

    std::size_t start = 0;
    while (start < text.size()) {
      if (IsWordBreak(text[start])) {
        // A preserved newline is a forced break (CRLF counts once).
        if (preserves_newlines && (text[start] == '\n' || text[start] == '\r')) {
          ++start;
          if (start < text.size() && text[start] == '\n') {
            ++start;
          }
          flush_line();
          continue;
        }
        const std::size_t whitespace_start = start;
        while (start < text.size() && IsWordBreak(text[start])) {
          if (preserves_newlines && (text[start] == '\n' || text[start] == '\r')) {
            break; // handled as a forced break on the next iteration
          }
          ++start;
        }
        if (preserves_spaces) {
          // Every space in the run is significant (a <pre> indents with them).
          for (std::size_t i = whitespace_start; i < start; ++i) {
            if (may_wrap && x + space_advance > line_right && x > line_left) {
              flush_line();
            }
            x += space_advance;
          }
        } else {
          // Collapse the run into a single space; leading whitespace at a line
          // start collapses to nothing (the HTML source indentation around an
          // <img> must not shift it).
          const bool at_line_start = x == line_left && line.runs.empty() && line.boxes.empty();
          if (!at_line_start) {
            if (may_wrap && x + space_advance > line_right && x > line_left) {
              flush_line();
            }
            x += space_advance;
          }
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
      add_word(word, style, item.element, selector, may_wrap);
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
// Defined after MeasureContent; used here for the width attribute fallback.
std::optional<std::int64_t> ParseNonNegativeInt(const dom::Element& element, std::string_view name);

// Measures the min/max intrinsic content width of |element|, recursing through
// the inline + block content model.  Replaced elements (img/input/...) use
// their explicit CSS width, else the width presentational attribute, and zero
// when neither applies (height-only replaced boxes keep their zero-width
// contribution; they do not arise in the supported content).  Percentages,
// floats and positioning are out of scope for this measurement.  Text is
// measured through |registry| (real advances with per-character fallback)
// when provided, else with the monospace fallback.
// Horizontal space a box needs beyond its own content: its padding and border,
// plus the marker gutter when it is a list item (the marker is drawn in a gutter
// inside the item's content box).  Intrinsic-width bookkeeping must count this for
// every box it measures (CSS Sizing 1 §5), otherwise a shrink-to-fit container
// comes out narrower than its own content and text wraps where a browser would not:
// miaow.com's list broke after "(" because the auto-width table sized to the text
// width while the item's content box was that minus the list padding and the
// marker gap.  Percentage padding resolves against an indefinite width here, so it
// contributes nothing (the same choice browsers make for intrinsic sizing).
float HorizontalBoxExtras(const style::ComputedStyle& style)
{
  float extras = ResolveSize(style.padding_left, 0) + ResolveSize(style.padding_right, 0) +
                 ResolveSize(style.border_left, 0) + ResolveSize(style.border_right, 0);
  if (style.display == style::Display::kListItem) {
    extras += kListMarkerGap;
  }
  return extras;
}

IntrinsicWidths MeasureContent(const dom::Element& element,
                               const style::StyleEngine& styles,
                               const graphics::FontRegistry* registry)
{
  const style::ComputedStyle& style = styles.StyleFor(element);
  const bool replaced = element.tag_name() == "img" || element.tag_name() == "video" ||
                        element.tag_name() == "canvas" || element.tag_name() == "svg" ||
                        element.tag_name() == "input" || element.tag_name() == "textarea" ||
                        element.tag_name() == "select";
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
    } else if (!style.width.has_value()) {
      if (const std::optional<std::int64_t> attr_w = ParseNonNegativeInt(element, "width");
          attr_w.has_value()) {
        // Presentational hint: a width attribute sizes the replaced box when
        // no CSS width applies at all (HTML rendering / SVG sizing).  An
        // author width -- even a percentage, which this measurement does not
        // resolve -- overrides the hint, so the attribute is ignored then.
        // Inline SVG icons carrying only width/height attributes measured 0
        // here, so a flex container sizing to its content dropped the icon's
        // whole slot and the text item collapsed/wrapped (bilibili's top
        // nav).
        w.min = w.max = static_cast<float>(attr_w.value());
      }
    }
    return w;
  }

  // A grid container's intrinsic width (CSS Grid Layout 1 §7.2).  max-content
  // is the sum of its in-flow items' max-content contributions (one row, with
  // `fr` tracks contributing their items' max-content), plus the column gaps;
  // min-content is the widest item.  Without this, an auto-sized grid track
  // containing a grid container is measured as a single block, so a nested
  // grid (e.g. a two-column hero) computes its columns against a too-narrow
  // containing width and its items overlap.
  if (style.display == style::Display::kGrid || style.display == style::Display::kInlineGrid) {
    IntrinsicWidths gw;
    double sum_max = 0;
    // Intrinsic widths are floats; the accumulation stays in double to avoid
    // rounding drift over many items, so each add promotes explicitly.
    auto add_max = [&sum_max](float value) { sum_max += static_cast<double>(value); };
    bool first = true;
    for (const dom::Node* child : element.ChildNodes()) {
      if (!RendersChild(element, *child)) {
        continue;
      }
      if (child->node_type() == dom::NodeType::kText) {
        const std::string& text = static_cast<const dom::Text&>(*child).data();
        const std::string collapsed = MeasuredText(text, style.white_space);
        if (collapsed.empty()) {
          continue;
        }
        if (!first) {
          add_max(style.column_gap);
        }
        add_max(MeasureTextWidth(registry,
                                 style.font_family,
                                 style.font_weight,
                                 style.font_italic,
                                 collapsed,
                                 style.font_size));
        gw.min = std::max(gw.min,
                          WidestWordWidth(registry,
                                          style.font_family,
                                          style.font_weight,
                                          style.font_italic,
                                          collapsed,
                                          style.font_size));
        first = false;
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
      if (child_style.position == style::Position::kAbsolute ||
          child_style.position == style::Position::kFixed) {
        continue;
      }
      const IntrinsicWidths cw = MeasureContent(child_el, styles, registry);
      gw.min = std::max(gw.min, cw.min);
      if (!first) {
        add_max(style.column_gap);
      }
      add_max(cw.max);
      first = false;
    }
    gw.max = static_cast<float>(sum_max);
    return gw;
  }

  // A flex container's intrinsic width (CSS Flexbox 1 §9.9.1): max-content is
  // the sum of the items' outer max-content sizes on one line (flex-wrap does
  // not force a break for intrinsic sizing), min-content is the widest item.
  // A column stacks its items, so both are the widest item.  Without this the
  // generic block path below took the widest child instead of the row sum: a
  // nested flex row (`flex: 0 1 auto; min-width: 0`) then reported its own
  // min-content width, shrank, and wrapped its items (baidu.com's top nav
  // collapsed into three rows).
  if (style.display == style::Display::kFlex || style.display == style::Display::kInlineFlex) {
    const bool row = style.flex_direction == style::FlexDirection::kRow ||
                     style.flex_direction == style::FlexDirection::kRowReverse;
    IntrinsicWidths fw;
    double sum_max = 0;
    bool first = true;
    for (const dom::Node* child : element.ChildNodes()) {
      if (!RendersChild(element, *child)) {
        continue;
      }
      if (child->node_type() == dom::NodeType::kText) {
        const std::string& text = static_cast<const dom::Text&>(*child).data();
        const std::string collapsed = MeasuredText(text, style.white_space);
        if (collapsed.empty()) {
          continue;
        }
        const float text_max = MeasureTextWidth(registry,
                                                style.font_family,
                                                style.font_weight,
                                                style.font_italic,
                                                collapsed,
                                                style.font_size);
        const float text_min = WidestWordWidth(registry,
                                               style.font_family,
                                               style.font_weight,
                                               style.font_italic,
                                               collapsed,
                                               style.font_size);
        if (!first) {
          sum_max += static_cast<double>(row ? style.column_gap : 0.0f);
        }
        sum_max += static_cast<double>(text_max);
        fw.min = std::max(fw.min, text_min);
        fw.max = std::max(fw.max, text_max);
        first = false;
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
      if (child_style.position == style::Position::kAbsolute ||
          child_style.position == style::Position::kFixed) {
        continue;
      }
      const IntrinsicWidths cw = MeasureContent(child_el, styles, registry);
      float child_max = cw.max;
      // A definite main size (the item's width, or its flex-basis in a row) is
      // the flex base size, so it also bounds the container's max-content
      // contribution.  Without this an empty `width:60px` flex item measured 0
      // and its row collapsed to zero width.  The min-content contribution
      // stays content-based: a flex item may shrink below its specified size
      // (the shrink floor below re-derives that bound).
      const style::SizeSpec* main_spec = nullptr;
      if (row) {
        if (child_style.flex_basis.has_value()) {
          main_spec = &child_style.flex_basis.value();
        } else if (child_style.width.has_value()) {
          main_spec = &child_style.width.value();
        }
      } else if (child_style.width.has_value()) {
        main_spec = &child_style.width.value();
      }
      if (main_spec != nullptr && !main_spec->percent && !main_spec->is_calc &&
          !main_spec->is_extremum) {
        child_max = SpecToContent(
            *main_spec,
            0.0f,
            ResolveSize(child_style.padding_left, 0) + ResolveSize(child_style.padding_right, 0) +
                ResolveSize(child_style.border_left, 0) + ResolveSize(child_style.border_right, 0),
            child_style.box_sizing);
      }
      const float outer_extra =
          HorizontalBoxExtras(child_style) + DisclosureMarkerGutter(child_el) +
          ResolveSize(child_style.margin_left, 0) + ResolveSize(child_style.margin_right, 0);
      if (!first) {
        sum_max += static_cast<double>(row ? style.column_gap : 0.0f);
      }
      sum_max += static_cast<double>(child_max + outer_extra);
      fw.min = std::max(fw.min, cw.min + outer_extra);
      fw.max = std::max(fw.max, child_max + outer_extra);
      first = false;
    }
    if (row) {
      fw.max = static_cast<float>(sum_max);
    }
    return fw;
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
    if (!RendersChild(element, *child)) {
      continue;
    }
    if (child->node_type() == dom::NodeType::kText) {
      const std::string& text = static_cast<const dom::Text&>(*child).data();
      line_min = std::max(line_min,
                          WidestWordWidth(registry,
                                          style.font_family,
                                          style.font_weight,
                                          style.font_italic,
                                          text,
                                          style.font_size));
      // CollapseWhitespace trims both ends, but a piece that follows content keeps
      // its leading space on the line: the line breaker only drops whitespace at a
      // line start, and it advances by one space before the next word.  Measuring
      // " (" as "(" therefore left an auto-width table one space narrower than the
      // line it has to hold, and the last word wrapped -- miaow.com's list, whose
      // text is split across <a> elements with " (" between them.  A whitespace-only
      // piece is the same case with an empty body, so it needs no separate branch.
      // The preserving values keep their whitespace verbatim and are unaffected.
      const bool keeps_leading_space = line_max > 0.0f && !text.empty() &&
                                       IsWordBreak(text.front()) &&
                                       style.white_space != style::WhiteSpace::kPre &&
                                       style.white_space != style::WhiteSpace::kPreWrap;
      const std::string measured = MeasuredText(text, style.white_space);
      line_max += MeasureTextWidth(registry,
                                   style.font_family,
                                   style.font_weight,
                                   style.font_italic,
                                   keeps_leading_space ? " " + measured : measured,
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
    if (child_style.position == style::Position::kAbsolute ||
        child_style.position == style::Position::kFixed) {
      // Out-of-flow content does not contribute to intrinsic sizes (CSS
      // Sizing 1 §5).  Without this an absolutely positioned subtree (e.g.
      // the carousel body over the shim skeleton) pushed its replaced
      // descendants' attribute sizes into the min/max-content of a grid
      // item, blowing up the `1fr` track minimums.
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
    const float child_extras = HorizontalBoxExtras(child_style) + DisclosureMarkerGutter(child_el);
    if (block_level) {
      // Block-level content starts a new line.
      flush_line();
      const IntrinsicWidths cw = MeasureContent(child_el, styles, registry);
      out.min = std::max(out.min, cw.min + child_extras);
      out.max = std::max(out.max, cw.max + child_extras);
    } else {
      // Inline content flows onto the current line.
      const IntrinsicWidths cw = MeasureContent(child_el, styles, registry);
      line_min = std::max(line_min, cw.min + child_extras);
      line_max += cw.max + child_extras;
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

// The number of characters a `size` / `cols` / `rows` attribute asks for, or
// nullopt when the attribute is absent, unparsable, or zero -- §4.10.5.3.5,
// §4.10.10.3 and §4.10.9 all require a value greater than zero, so zero is
// invalid as well.  The result is capped: the attribute is remote input, and
// without a cap size=2000000000 would ask for a box two billion pixels wide
// (the style engine caps repeat()/grid counts for the same reason).
std::optional<int> FieldSizeAttribute(const dom::Element& element, std::string_view attribute)
{
  const std::optional<std::int64_t> value = ParseNonNegativeInt(element, attribute);
  if (!value.has_value() || *value <= 0) {
    return std::nullopt;
  }
  return static_cast<int>(std::min<std::int64_t>(*value, kMaxFieldSizeChars));
}

// An option's label (§4.10.10.4 "getting an option element's label"): its
// non-empty `label` attribute, else its text content with whitespace collapsed
// (an option's label is shown on one line).
std::string OptionLabel(const dom::Element& option)
{
  const std::optional<std::string_view> label = option.GetAttribute("label");
  if (label.has_value() && !label->empty()) {
    return std::string(*label);
  }
  return CollapseWhitespace(option.TextContent());
}

// Appends the option children of |select|, descending into optgroups (an
// optgroup cannot nest, so this is one level in practice).
void CollectOptions(const dom::Element& select, std::vector<const dom::Element*>& options)
{
  for (const dom::Node* child : select.ChildNodes()) {
    if (child->node_type() != dom::NodeType::kElement) {
      continue;
    }
    const dom::Element& element = static_cast<const dom::Element&>(*child);
    if (element.tag_name() == "option") {
      options.push_back(&element);
    } else if (element.tag_name() == "optgroup") {
      CollectOptions(element, options);
    }
  }
}

// The option whose label a drop-down select shows: the first option carrying
// the `selected` attribute, else the first option (§4.10.10.4 -- selectedness
// defaults to the first option when no option is selected).
const dom::Element* SelectedOption(const std::vector<const dom::Element*>& options)
{
  for (const dom::Element* option : options) {
    if (option->HasAttribute("selected")) {
      return option;
    }
  }
  return options.empty() ? nullptr : options.front();
}

// The label of a text button.  §15.5.12: the contents are the `value`
// attribute's text, or text derived from the `type` attribute in an
// implementation-defined (and probably locale-specific) fashion -- English here.
// A <button> shows its own content instead.  An input[type=image] shows its
// `src` image (NOT IMPLEMENTED: it falls back to an empty label).
std::string ButtonLabel(const dom::Element& element)
{
  const std::optional<std::string_view> value = element.GetAttribute("value");
  if (value.has_value() && !value->empty()) {
    return std::string(*value);
  }
  if (element.tag_name() == "button") {
    return CollapseWhitespace(element.TextContent());
  }
  const std::string_view type = element.GetAttribute("type").value_or("text");
  if (base::AsciiEqualsIgnoreCase(type, "submit")) {
    return "Submit";
  }
  if (base::AsciiEqualsIgnoreCase(type, "reset")) {
    return "Reset";
  }
  return {};
}

// <input type="password"> renders its value as U+2022 bullets (WHATWG
// §15.5.6), one per UTF-8 code point -- a CJK character masks to one bullet,
// not one per continuation byte.  The placeholder is never masked (browsers
// show it in the clear).
std::string MaskPasswordValue(std::string_view value)
{
  std::string out;
  out.reserve(value.size());
  for (const char raw : value) {
    const auto byte = static_cast<unsigned char>(raw);
    if ((byte & 0xC0U) != 0x80U) { // a lead byte starts one code point
      out += "\xE2\x80\xA2";
    }
  }
  return out;
}

// Hint text shown while a text control is empty: the standard `placeholder`
// attribute, then two non-standard attributes that Baidu's home page carries its
// search-box hint in.  Those two names are site-specific (no other page defines
// them) and the site's own scripts feed them; the engine keeps reading them so
// that the hint survives while its script support is partial.
std::string PlaceholderFor(const dom::Element& element)
{
  for (const std::string_view name :
       {"placeholder", "data-ai-placeholder", "data-normal-placeholder"}) {
    if (const std::optional<std::string_view> value = element.GetAttribute(name);
        value.has_value() && !value->empty()) {
      return std::string(*value);
    }
  }
  return {};
}

void ComputeReplacedSize(const style::ComputedStyle& style,
                         const dom::Element& element,
                         const image::Image* img,
                         float containing_width,
                         float containing_height,
                         float& out_w,
                         float& out_h)
{
  // Intrinsic size: the decoded frame when available; <video>/<canvas>/<svg>
  // without one fall back to the CSS default replaced size 300x150 (an svg's
  // width/height attributes are handled below as presentational hints).
  float intrinsic_w = img != nullptr ? static_cast<float>(img->width) : 0.0f;
  float intrinsic_h = img != nullptr ? static_cast<float>(img->height) : 0.0f;
  if (img == nullptr && (element.tag_name() == "video" || element.tag_name() == "canvas" ||
                         element.tag_name() == "svg")) {
    intrinsic_w = 300;
    intrinsic_h = 150;
  }

  // Specified size: CSS width/height win over the width/height attributes
  // (presentational hints, HTML spec).  Percentages resolve against the
  // containing block here, so the ratio transfer below sees the USED size:
  // `width: 100%` on a 2:1 image in a 400px container is 400x200, not its
  // intrinsic 100x50 (the height used to stay at the intrinsic value and the
  // rendered picture stretched).  A percentage HEIGHT against an INDEFINITE
  // containing block height (|containing_height| <= 0) is auto per CSS 2.2
  // §10.5 -- it falls through and the ratio carries it.
  std::optional<float> spec_w;
  std::optional<float> spec_h;
  if (style.width.has_value()) {
    spec_w = ResolveSize(style.width.value(), containing_width);
  } else if (const std::optional<std::int64_t> attr = ParseNonNegativeInt(element, "width")) {
    spec_w = static_cast<float>(attr.value());
  }
  if (style.height.has_value()) {
    const style::SizeSpec& height = style.height.value();
    if (!height.percent || containing_height > 0) {
      spec_h = ResolveSize(height, height.percent ? containing_height : containing_width);
    }
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
