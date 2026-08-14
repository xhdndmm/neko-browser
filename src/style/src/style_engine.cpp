#include "neko/style/style_engine.h"

#include "neko/base/logging.h"
#include "neko/base/string_util.h"
#include "neko/css/parser.h"
#include "neko/css/selector.h"
#include "neko/css/stylesheet.h"
#include "neko/css/value.h"
#include "neko/dom/query.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neko::style {
namespace {

// HTML user-agent stylesheet (Phase 4 scope).
constexpr std::string_view kUaStylesheet = R"css(
html { display: block; }
head, title, style, script, meta, link, base, template { display: none; }
body { display: block; margin: 8px; }
div, p, section, article, aside, header, footer, nav, main, hgroup,
h1, h2, h3, h4, h5, h6,
address, blockquote, pre, figure, figcaption, form, fieldset, details,
summary, hr, dl, dt, dd, ul, ol, li { display: block; }
table { display: table; }
caption { display: table-caption; }
thead, tbody, tfoot { display: table-row-group; }
tr { display: table-row; }
td, th { display: table-cell; }
a, span, em, strong, b, i, u, s, small, sub, sup, code, label,
select, textarea, input, q, cite, mark, time { display: inline; }
button { display: inline-block; appearance: auto; text-align: center;
         padding: 1px 6px; border: 2px solid; }
p { margin-top: 1em; margin-bottom: 1em; }
h1 { font-size: 2em; font-weight: bold; margin-top: 0.67em; margin-bottom: 0.67em; }
h2 { font-size: 1.5em; font-weight: bold; margin-top: 0.83em; margin-bottom: 0.83em; }
h3 { font-size: 1.17em; font-weight: bold; margin-top: 1em; margin-bottom: 1em; }
h4 { font-size: 1em; font-weight: bold; margin-top: 1.33em; margin-bottom: 1.33em; }
h5 { font-size: 0.83em; font-weight: bold; margin-top: 1.67em; margin-bottom: 1.67em; }
h6 { font-size: 0.67em; font-weight: bold; margin-top: 2.33em; margin-bottom: 2.33em; }
ul, ol { margin-top: 1em; margin-bottom: 1em; padding-left: 40px; }
li { display: block; }
pre { font-family: monospace; }
code { font-family: monospace; }
b, strong { font-weight: bold; }
i, em { font-style: italic; }
a { color: blue; text-decoration: underline; }
hr { border-top: 1px solid; border-top-color: #000; }
)css";

constexpr unsigned kInlineSpecificityA = 1000000;

// Synthetic specificity for inline styles (higher than any selector can
// reach, per CSS cascade rules).
constexpr css::Specificity kInlineSpecificity = {kInlineSpecificityA, 0, 0};

// Splits a value on whitespace.
std::vector<std::string> SplitWhitespace(std::string_view value)
{
  std::vector<std::string> parts;
  std::size_t i = 0;
  while (i < value.size()) {
    while (i < value.size() && (value[i] == ' ' || value[i] == '\t')) {
      ++i;
    }
    const std::size_t start = i;
    while (i < value.size() && value[i] != ' ' && value[i] != '\t') {
      ++i;
    }
    if (i > start) {
      parts.emplace_back(value.substr(start, i - start));
    }
  }
  return parts;
}

// Parses a single value into a SizeSpec (resolving em/rem against font sizes).
std::optional<SizeSpec> ParseSize(const std::string& text, float font_size, float root_font_size)
{
  const css::CssValue value = css::ParseCssValue(text);
  if (value.type != css::CssValue::Type::kLength && value.type != css::CssValue::Type::kNumber) {
    return std::nullopt;
  }
  SizeSpec spec;
  if (value.is_percent) {
    spec.value = value.value;
    spec.percent = true;
    return spec;
  }
  if (value.unit == "em") {
    spec.value = value.value * font_size;
  } else if (value.unit == "rem") {
    spec.value = value.value * root_font_size;
  } else {
    spec.value = value.value;
  }
  return spec;
}

// Applies a 1-4 value shorthand to the four sides.
void ApplyBoxShorthand(const std::string& value,
                       float font_size,
                       float root_font_size,
                       std::array<SizeSpec, 4>& sides)
{
  const std::vector<std::string> parts = SplitWhitespace(value);
  auto parse = [&](const std::string& text) -> SizeSpec {
    if (const std::optional<SizeSpec> spec = ParseSize(text, font_size, root_font_size)) {
      return spec.value();
    }
    return SizeSpec{};
  };
  switch (parts.size()) {
  case 1:
    sides = {parse(parts[0]), parse(parts[0]), parse(parts[0]), parse(parts[0])};
    break;
  case 2:
    sides = {parse(parts[0]), parse(parts[1]), parse(parts[0]), parse(parts[1])};
    break;
  case 3:
    sides = {parse(parts[0]), parse(parts[1]), parse(parts[2]), parse(parts[1])};
    break;
  default:
    if (parts.size() >= 4) {
      sides = {parse(parts[0]), parse(parts[1]), parse(parts[2]), parse(parts[3])};
    }
    break;
  }
}

// Margin shorthand ("margin: <top> <right> <bottom> <left>" with the usual
// 1-4 value rules).  Each token may be a length/percentage or "auto"; the
// per-side auto flags mirror the resolved sizes.
void ApplyMarginShorthand(const std::string& value,
                          float font_size,
                          float root_font_size,
                          std::array<SizeSpec, 4>& sides,
                          std::array<bool, 4>& auto_flags)
{
  const std::vector<std::string> parts = SplitWhitespace(value);
  auto parse = [&](const std::string& text, SizeSpec& out, bool& is_auto) {
    const css::CssValue v = css::ParseCssValue(text);
    if (v.type == css::CssValue::Type::kKeyword && v.text == "auto") {
      out = SizeSpec{};
      is_auto = true;
      return;
    }
    is_auto = false;
    if (const std::optional<SizeSpec> spec = ParseSize(text, font_size, root_font_size)) {
      out = spec.value();
    } else {
      out = SizeSpec{};
    }
  };
  auto set1 = [&](SizeSpec& s, bool& a) { parse(parts[0], s, a); };
  auto set2 = [&](SizeSpec& s, bool& a) { parse(parts.size() >= 2 ? parts[1] : parts[0], s, a); };
  auto set3 = [&](SizeSpec& s, bool& a) { parse(parts.size() >= 3 ? parts[2] : parts[0], s, a); };
  auto set4 = [&](SizeSpec& s, bool& a) {
    parse(parts.size() >= 4 ? parts[3] : (parts.size() == 2 ? parts[1] : parts[0]), s, a);
  };
  switch (parts.size()) {
  case 1:
    set1(sides[0], auto_flags[0]);
    set1(sides[1], auto_flags[1]);
    set1(sides[2], auto_flags[2]);
    set1(sides[3], auto_flags[3]);
    break;
  case 2:
    set1(sides[0], auto_flags[0]);
    set2(sides[1], auto_flags[1]);
    set1(sides[2], auto_flags[2]);
    set2(sides[3], auto_flags[3]);
    break;
  case 3:
    set1(sides[0], auto_flags[0]);
    set2(sides[1], auto_flags[1]);
    set3(sides[2], auto_flags[2]);
    set2(sides[3], auto_flags[3]);
    break;
  default:
    if (parts.size() >= 4) {
      set1(sides[0], auto_flags[0]);
      set2(sides[1], auto_flags[1]);
      set3(sides[2], auto_flags[2]);
      set4(sides[3], auto_flags[3]);
    }
    break;
  }
}

// Extracts width/color/style from a border value ("1px solid #000").
void ParseBorderValue(const std::string& value,
                      float font_size,
                      float root_font_size,
                      float& width,
                      std::optional<css::Color>& color,
                      BorderStyle& style)
{
  for (const std::string& part : SplitWhitespace(value)) {
    if (const std::optional<css::Color> c = css::ParseColor(part)) {
      color = c;
      continue;
    }
    const css::CssValue parsed = css::ParseCssValue(part);
    if (parsed.type == css::CssValue::Type::kLength ||
        parsed.type == css::CssValue::Type::kNumber) {
      if (const std::optional<SizeSpec> spec = ParseSize(part, font_size, root_font_size)) {
        width = spec.value().value;
      }
      continue;
    }
    if (parsed.type == css::CssValue::Type::kKeyword) {
      if (parsed.text == "solid") {
        style = BorderStyle::kSolid;
      } else if (parsed.text == "dashed") {
        style = BorderStyle::kDashed;
      } else if (parsed.text == "dotted") {
        style = BorderStyle::kDotted;
      } else if (parsed.text == "none") {
        style = BorderStyle::kNone;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Grid track / placement parsing (CSS Grid Layout 1).
// ---------------------------------------------------------------------------

// Splits a track list into comma- or whitespace-separated tokens while
// keeping "repeat(...)" intact (commas inside repeat must not split).
std::vector<std::string> SplitGridList(std::string_view text)
{
  std::vector<std::string> parts;
  std::size_t i = 0;
  int depth = 0;
  std::string current;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '(') {
      ++depth;
      current.push_back(c);
    } else if (c == ')') {
      if (depth > 0) {
        --depth;
      }
      current.push_back(c);
    } else if (depth == 0 && (c == ',' || c == ' ' || c == '\t')) {
      if (!current.empty()) {
        parts.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(c);
    }
    ++i;
  }
  if (!current.empty()) {
    parts.push_back(current);
  }
  return parts;
}

// Parses a single grid track token ("100px", "25%", "1fr", "auto",
// "min-content", "max-content", "repeat(2, 40px 1fr)").
void ParseGridTrackToken(const std::string& token,
                         float font_size,
                         float root_font_size,
                         std::vector<GridTrack>& out)
{
  const std::string lower = neko::base::ToLower(token);
  if (lower == "auto") {
    out.push_back(GridTrack{GridTrack::Kind::kAuto, 0, 0, 0});
    return;
  }
  if (lower == "min-content") {
    out.push_back(GridTrack{GridTrack::Kind::kMinContent, 0, 0, 0});
    return;
  }
  if (lower == "max-content") {
    out.push_back(GridTrack{GridTrack::Kind::kMaxContent, 0, 0, 0});
    return;
  }
  // repeat(N, <track-list>)
  const std::size_t repeat_pos = lower.find("repeat(");
  if (repeat_pos == 0 && lower.back() == ')') {
    const std::string inner = lower.substr(7, lower.size() - 8);
    const std::size_t comma = inner.find(',');
    if (comma != std::string::npos) {
      const std::string count_text = std::string(neko::base::Trim(inner.substr(0, comma)));
      const css::CssValue count_value = css::ParseCssValue(count_text);
      int count = 0;
      if (count_value.type == css::CssValue::Type::kNumber) {
        count = static_cast<int>(count_value.number);
      }
      if (count > 0) {
        const std::vector<std::string> inner_tracks = SplitGridList(inner.substr(comma + 1));
        for (int r = 0; r < count; ++r) {
          for (const std::string& track : inner_tracks) {
            ParseGridTrackToken(track, font_size, root_font_size, out);
          }
        }
      }
    }
    return;
  }
  // Nfr
  if (lower.size() > 2 && lower.compare(lower.size() - 2, 2, "fr") == 0) {
    const css::CssValue v = css::ParseCssValue(lower.substr(0, lower.size() - 2));
    if (v.type == css::CssValue::Type::kNumber && v.number > 0) {
      out.push_back(GridTrack{GridTrack::Kind::kFr, 0, 0, v.number});
      return;
    }
    return;
  }
  // Length / percentage.
  if (const std::optional<SizeSpec> spec = ParseSize(token, font_size, root_font_size)) {
    GridTrack track;
    track.kind = GridTrack::Kind::kFixed;
    if (spec.value().percent) {
      track.percent = spec.value().value;
    } else {
      track.length = spec.value().value;
    }
    out.push_back(track);
    return;
  }
  // Unknown token: treat as auto (permissive parsing).
  out.push_back(GridTrack{GridTrack::Kind::kAuto, 0, 0, 0});
}

std::vector<GridTrack>
ParseGridTrackList(const std::string& value, float font_size, float root_font_size)
{
  std::vector<GridTrack> tracks;
  for (const std::string& token : SplitGridList(value)) {
    ParseGridTrackToken(token, font_size, root_font_size, tracks);
  }
  return tracks;
}

// Parses one grid line value: "auto", "<number>", "span <number>".
void ParseGridLine(const std::string& text, GridPlacement& out)
{
  const std::string lower = neko::base::ToLower(text);
  if (lower == "auto" || lower.empty()) {
    out = GridPlacement{};
    return;
  }
  if (lower.compare(0, 5, "span ") == 0) {
    const css::CssValue v = css::ParseCssValue(std::string(neko::base::Trim(lower.substr(5))));
    if (v.type == css::CssValue::Type::kNumber && v.number >= 1) {
      out.kind = GridPlacement::Kind::kSpan;
      out.span = static_cast<int>(v.number);
    }
    return;
  }
  const css::CssValue v = css::ParseCssValue(lower);
  if (v.type == css::CssValue::Type::kNumber && v.number >= 1) {
    out.kind = GridPlacement::Kind::kLine;
    out.line = static_cast<int>(v.number);
  }
}

// Parses the grid-column / grid-row shorthand:
//   auto | <line> | span <n> | <line> / <line> | <line> / span <n> | span <n> / <line>
void ParseGridPlacementShorthand(const std::string& value, GridPlacement& start, GridPlacement& end)
{
  const std::vector<std::string> parts = SplitWhitespace(value);
  if (parts.empty()) {
    return;
  }
  // "span N" is a two-token unit.
  std::vector<std::string> units;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (parts[i] == "/") {
      continue; // separators are implicit
    }
    if (parts[i] == "span" && i + 1 < parts.size()) {
      units.push_back("span " + parts[i + 1]);
      ++i;
    } else {
      units.push_back(parts[i]);
    }
  }
  if (units.empty()) {
    return;
  }
  if (units.size() == 1) {
    // A single "span N" means "auto / span N" (the span belongs on the end
    // line); anything else sets the start line.
    if (units[0].compare(0, 5, "span ") == 0) {
      ParseGridLine(units[0], end);
    } else {
      ParseGridLine(units[0], start);
    }
    return;
  }
  ParseGridLine(units[0], start);
  ParseGridLine(units[1], end);
}

bool MediaQueryMatches(std::string_view prelude)
{
  const std::string lower = neko::base::ToLower(prelude);
  // A media query is treated as matching when it targets screen/all or the
  // empty query (the common case).  print/speech queries do not match.
  if (lower.find("print") != std::string::npos) {
    return false;
  }
  if (lower.find("speech") != std::string::npos) {
    return false;
  }
  return true;
}

} // namespace

void StyleEngine::ApplyStyles(dom::Document& document)
{
  // Collect author sheets from <style> elements.
  author_sheets_.clear();
  for (dom::Element* style : dom::QuerySelectorAll(document, "style")) {
    css::StyleSheet sheet = css::ParseStyleSheet(style->TextContent());
    author_sheets_.push_back(std::move(sheet));
  }

  styles_.clear();

  dom::Element* root = document.document_element();
  if (root == nullptr) {
    return;
  }

  ComputedStyle root_style;
  root_style.display = Display::kBlock;
  root_style.font_size = 16;
  root_style.line_height = 19.2f;
  ComputeElement(*root, root_style, root_style.font_size);
}

const ComputedStyle& StyleEngine::StyleFor(const dom::Element& element) const
{
  const auto it = styles_.find(&element);
  if (it != styles_.end()) {
    return it->second;
  }
  static const ComputedStyle kDefault;
  return kDefault;
}

void StyleEngine::ComputeElement(dom::Element& element,
                                 const ComputedStyle& inherited,
                                 float root_font_size)
{
  // Collect all matching declarations for this element.
  struct Candidate
  {
    const css::Declaration* declaration;
    css::Specificity specificity;
    int order;
  };
  std::vector<Candidate> candidates;
  int order = 0;

  const css::StyleSheet& ua = [] {
    static const css::StyleSheet sheet = css::ParseStyleSheet(kUaStylesheet);
    return sheet;
  }();

  auto collect = [&](const css::StyleSheet& sheet) {
    for (const css::StyleRule& rule : sheet.rules) {
      for (const css::ComplexSelector& selector : rule.selectors) {
        if (!css::MatchesSelector(element, selector)) {
          continue;
        }
        const css::Specificity specificity = [&] {
          // Reuse MatchingSpecificity is overkill here; compute per selector.
          css::Specificity spec;
          for (const css::CompoundSelector& compound : selector.compounds) {
            if (compound.tag.has_value()) {
              ++spec.c;
            }
            if (compound.id.has_value()) {
              ++spec.a;
            }
            spec.b += static_cast<unsigned>(compound.classes.size());
            spec.b += static_cast<unsigned>(compound.attributes.size());
            spec.b += static_cast<unsigned>(compound.pseudo_classes.size());
          }
          return spec;
        }();
        for (const css::Declaration& declaration : rule.declarations) {
          candidates.push_back(Candidate{&declaration, specificity, order++});
        }
      }
    }
    for (const css::AtRule& at_rule : sheet.at_rules) {
      if (at_rule.name == "media" && !MediaQueryMatches(at_rule.prelude)) {
        continue;
      }
      for (const css::StyleRule& rule : at_rule.rules) {
        for (const css::ComplexSelector& selector : rule.selectors) {
          if (!css::MatchesSelector(element, selector)) {
            continue;
          }
          css::Specificity spec;
          for (const css::CompoundSelector& compound : selector.compounds) {
            if (compound.tag.has_value()) {
              ++spec.c;
            }
            if (compound.id.has_value()) {
              ++spec.a;
            }
            spec.b += static_cast<unsigned>(compound.classes.size());
            spec.b += static_cast<unsigned>(compound.attributes.size());
            spec.b += static_cast<unsigned>(compound.pseudo_classes.size());
          }
          for (const css::Declaration& declaration : rule.declarations) {
            candidates.push_back(Candidate{&declaration, spec, order++});
          }
        }
      }
    }
  };
  collect(ua);
  for (const css::StyleSheet& sheet : author_sheets_) {
    collect(sheet);
  }

  // Inline style attribute (highest authority among normal declarations).
  std::vector<css::Declaration> inline_decls;
  const std::optional<std::string_view> inline_style = element.GetAttribute("style");
  if (inline_style.has_value()) {
    inline_decls = css::ParseDeclarationBlock(inline_style.value());
  }

  // Parse inline declarations first so their addresses stay stable while the
  // candidate list is built (no reallocation of the container holding them).
  for (const css::Declaration& declaration : inline_decls) {
    candidates.push_back(Candidate{&declaration, kInlineSpecificity, order++});
  }

  // Cascade: importance > specificity > order.
  std::map<std::string, Candidate> winners;
  for (const Candidate& candidate : candidates) {
    const std::string& property = candidate.declaration->property;
    const bool important = candidate.declaration->important;
    auto existing = winners.find(property);
    if (existing == winners.end()) {
      winners.emplace(property, candidate);
      continue;
    }
    const Candidate& current = existing->second;
    const bool current_important = current.declaration->important;
    bool replace = false;
    if (important != current_important) {
      replace = important;
    } else if (candidate.specificity.a != current.specificity.a ||
               candidate.specificity.b != current.specificity.b ||
               candidate.specificity.c != current.specificity.c) {
      replace = current.specificity < candidate.specificity;
    } else {
      replace = candidate.order > current.order;
    }
    if (replace) {
      existing->second = candidate;
    }
  }

  // Resolve the computed style.
  ComputedStyle out;
  out.color = inherited.color;
  out.font_size = inherited.font_size;
  out.font_weight = inherited.font_weight;
  out.font_italic = inherited.font_italic;
  out.font_family = inherited.font_family;
  out.line_height = inherited.line_height;
  out.text_align = inherited.text_align;
  out.text_decoration_underline = inherited.text_decoration_underline;

  auto find = [&](std::string_view property) -> const css::Declaration* {
    const auto it = winners.find(std::string(property));
    return it == winners.end() ? nullptr : it->second.declaration;
  };

  const bool font_size_set = find("font-size") != nullptr;
  const bool line_height_set = find("line-height") != nullptr;

  // font-size first (em/rem depend on it).
  if (const css::Declaration* d = find("font-size")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kLength) {
      if (v.is_percent) {
        out.font_size = inherited.font_size * v.value / 100.0f;
      } else if (v.unit == "em") {
        out.font_size = inherited.font_size * v.value;
      } else if (v.unit == "rem") {
        out.font_size = root_font_size * v.value;
      } else {
        out.font_size = v.value;
      }
    } else if (v.type == css::CssValue::Type::kKeyword) {
      static const std::map<std::string, float> kSizes = {{"xx-small", 9.0f},
                                                          {"x-small", 10.0f},
                                                          {"small", 13.0f},
                                                          {"medium", 16.0f},
                                                          {"large", 18.0f},
                                                          {"x-large", 24.0f},
                                                          {"xx-large", 32.0f}};
      const auto it = kSizes.find(v.text);
      if (it != kSizes.end()) {
        out.font_size = it->second;
      } else if (v.text == "smaller") {
        out.font_size = inherited.font_size * 0.8f;
      } else if (v.text == "larger") {
        out.font_size = inherited.font_size * 1.2f;
      }
    }
  }

  // font-weight.
  if (const css::Declaration* d = find("font-weight")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kNumber) {
      out.font_weight = static_cast<int>(v.number);
    } else if (v.type == css::CssValue::Type::kKeyword) {
      if (v.text == "bold") {
        out.font_weight = 700;
      } else if (v.text == "normal") {
        out.font_weight = 400;
      }
    }
  }

  // font-family.
  if (const css::Declaration* d = find("font-family")) {
    out.font_family = d->value;
  }

  // font-style.
  if (const css::Declaration* d = find("font-style")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword) {
      out.font_italic = (v.text == "italic" || v.text == "oblique");
    }
  }

  // object-fit.
  if (const css::Declaration* d = find("object-fit")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword) {
      if (v.text == "contain") {
        out.object_fit = ObjectFit::kContain;
      } else if (v.text == "cover") {
        out.object_fit = ObjectFit::kCover;
      } else if (v.text == "none") {
        out.object_fit = ObjectFit::kNone;
      } else if (v.text == "scale-down") {
        out.object_fit = ObjectFit::kScaleDown;
      }
    }
  }

  // vertical-align.
  if (const css::Declaration* d = find("vertical-align")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword) {
      if (v.text == "middle") {
        out.vertical_align = VerticalAlign::kMiddle;
      } else if (v.text == "top") {
        out.vertical_align = VerticalAlign::kTop;
      } else if (v.text == "bottom") {
        out.vertical_align = VerticalAlign::kBottom;
      } else if (v.text == "text-top") {
        out.vertical_align = VerticalAlign::kTextTop;
      } else if (v.text == "text-bottom") {
        out.vertical_align = VerticalAlign::kTextBottom;
      }
    }
  }

  // line-height.
  if (const css::Declaration* d = find("line-height")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kNumber) {
      out.line_height = out.font_size * v.number;
    } else if (v.type == css::CssValue::Type::kLength) {
      out.line_height = v.is_percent ? out.font_size * v.value / 100.0f
                                     : (v.unit == "em" ? out.font_size * v.value : v.value);
    }
  }

  // `line-height: normal` scales with the font size. ComputedStyle stores
  // line-height as absolute px, so an element that changes font-size without
  // an explicit line-height must re-derive it from the inherited ratio
  // (default 19.2/16 = 1.2) instead of keeping the parent's stale value.
  if (font_size_set && !line_height_set) {
    out.line_height = out.font_size * (inherited.line_height / inherited.font_size);
  }

  // color / text-align.
  if (const css::Declaration* d = find("color")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kColor) {
      out.color = v.color;
    }
  }
  if (const css::Declaration* d = find("text-align")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword) {
      if (v.text == "center") {
        out.text_align = TextAlign::kCenter;
      } else if (v.text == "right") {
        out.text_align = TextAlign::kRight;
      } else if (v.text == "justify") {
        out.text_align = TextAlign::kJustify;
      } else {
        out.text_align = TextAlign::kLeft;
      }
    }
  }
  if (const css::Declaration* d = find("text-decoration")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword) {
      out.text_decoration_underline = (v.text == "underline");
    }
  }

  // display.
  if (const css::Declaration* d = find("display")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword) {
      if (v.text == "block") {
        out.display = Display::kBlock;
      } else if (v.text == "grid") {
        out.display = Display::kGrid;
      } else if (v.text == "flex") {
        out.display = Display::kFlex;
      } else if (v.text == "inline-flex") {
        out.display = Display::kInlineFlex;
      } else if (v.text == "inline") {
        out.display = Display::kInline;
      } else if (v.text == "inline-block") {
        out.display = Display::kInlineBlock;
      } else if (v.text == "none") {
        out.display = Display::kNone;
      } else if (v.text == "table") {
        out.display = Display::kTable;
      } else if (v.text == "table-row-group" || v.text == "table-header-group" ||
                 v.text == "table-footer-group") {
        out.display = Display::kTableRowGroup;
      } else if (v.text == "table-row") {
        out.display = Display::kTableRow;
      } else if (v.text == "table-cell") {
        out.display = Display::kTableCell;
      } else if (v.text == "table-caption") {
        out.display = Display::kTableCaption;
      }
    }
  }

  // flex-direction (flex container).
  if (const css::Declaration* d = find("flex-direction")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword) {
      if (v.text == "row-reverse") {
        out.flex_direction = FlexDirection::kRowReverse;
      } else if (v.text == "column") {
        out.flex_direction = FlexDirection::kColumn;
      } else if (v.text == "column-reverse") {
        out.flex_direction = FlexDirection::kColumnReverse;
      } else {
        out.flex_direction = FlexDirection::kRow;
      }
    }
  }

  // flex-wrap (flex container).
  if (const css::Declaration* d = find("flex-wrap")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword) {
      if (v.text == "wrap") {
        out.flex_wrap = FlexWrap::kWrap;
      } else if (v.text == "wrap-reverse") {
        out.flex_wrap = FlexWrap::kWrapReverse;
      } else {
        out.flex_wrap = FlexWrap::kNoWrap;
      }
    }
  }

  // justify-content (flex container).
  if (const css::Declaration* d = find("justify-content")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword) {
      if (v.text == "flex-end") {
        out.justify_content = JustifyContent::kFlexEnd;
      } else if (v.text == "center") {
        out.justify_content = JustifyContent::kCenter;
      } else if (v.text == "space-between") {
        out.justify_content = JustifyContent::kSpaceBetween;
      } else if (v.text == "space-around") {
        out.justify_content = JustifyContent::kSpaceAround;
      } else if (v.text == "space-evenly") {
        out.justify_content = JustifyContent::kSpaceEvenly;
      } else {
        out.justify_content = JustifyContent::kFlexStart;
      }
    }
  }

  // align-items (flex container; CSS initial value is stretch).
  if (const css::Declaration* d = find("align-items")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword) {
      if (v.text == "flex-start") {
        out.align_items = AlignItems::kFlexStart;
      } else if (v.text == "flex-end") {
        out.align_items = AlignItems::kFlexEnd;
      } else if (v.text == "center") {
        out.align_items = AlignItems::kCenter;
      } else if (v.text == "baseline") {
        out.align_items = AlignItems::kBaseline;
      } else {
        out.align_items = AlignItems::kStretch;
      }
    }
  }

  // align-content (flex container; used with multiple lines).
  if (const css::Declaration* d = find("align-content")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword) {
      if (v.text == "flex-start") {
        out.align_content = AlignContent::kFlexStart;
      } else if (v.text == "flex-end") {
        out.align_content = AlignContent::kFlexEnd;
      } else if (v.text == "center") {
        out.align_content = AlignContent::kCenter;
      } else if (v.text == "space-between") {
        out.align_content = AlignContent::kSpaceBetween;
      } else if (v.text == "space-around") {
        out.align_content = AlignContent::kSpaceAround;
      } else {
        out.align_content = AlignContent::kStretch;
      }
    }
  }

  // flex-grow (flex item).
  if (const css::Declaration* d = find("flex-grow")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kNumber) {
      out.flex_grow = std::max(0.0f, v.number);
    }
  }

  // flex-shrink (flex item).
  if (const css::Declaration* d = find("flex-shrink")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kNumber) {
      out.flex_shrink = std::max(0.0f, v.number);
    }
  }

  // flex-basis (flex item; auto => content-based).
  if (const css::Declaration* d = find("flex-basis")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword && v.text == "auto") {
      out.flex_basis = std::nullopt;
    } else if (const std::optional<SizeSpec> spec =
                   ParseSize(d->value, out.font_size, root_font_size)) {
      out.flex_basis = spec.value();
    }
  }

  // flex shorthand: <grow> <shrink> <basis> | <grow> (=> 1 1 0) | none.
  if (const css::Declaration* d = find("flex")) {
    const std::vector<std::string> parts = SplitWhitespace(d->value);
    auto parse_number = [](const std::string& s) -> std::optional<float> {
      const css::CssValue v = css::ParseCssValue(s);
      if (v.type == css::CssValue::Type::kNumber) {
        return std::max(0.0f, v.number);
      }
      return std::nullopt;
    };
    if (parts.size() == 1 && parts[0] == "none") {
      out.flex_grow = 0;
      out.flex_shrink = 0;
      out.flex_basis = SizeSpec{0, false};
    } else if (parts.size() == 1) {
      // "flex: <grow>" == "flex: <grow> 1 0".
      if (const std::optional<float> g = parse_number(parts[0])) {
        out.flex_grow = g.value();
        out.flex_shrink = 1;
        out.flex_basis = SizeSpec{0, false};
      }
    } else if (parts.size() >= 2) {
      if (const std::optional<float> g = parse_number(parts[0])) {
        out.flex_grow = g.value();
      }
      if (const std::optional<float> s = parse_number(parts[1])) {
        out.flex_shrink = s.value();
      }
      if (parts.size() >= 3) {
        const std::string& basis = parts[2];
        if (basis != "auto") {
          if (const std::optional<SizeSpec> spec =
                  ParseSize(basis, out.font_size, root_font_size)) {
            out.flex_basis = spec.value();
          }
        } else {
          out.flex_basis = std::nullopt;
        }
      }
    }
  }

  // order (flex item; CSS Flexbox 1 §5.4).  Items are laid out in ascending
  // order; equal values keep document order.
  if (const css::Declaration* d = find("order")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kNumber) {
      out.order = static_cast<int>(v.number);
    }
  }

  // align-self (flex item; CSS Flexbox 1 §8.3).  Overrides the container's
  // align-items for this item; "auto" falls back to the container.
  if (const css::Declaration* d = find("align-self")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword) {
      if (v.text == "auto") {
        out.align_self = std::nullopt;
      } else if (v.text == "flex-start") {
        out.align_self = AlignItems::kFlexStart;
      } else if (v.text == "flex-end") {
        out.align_self = AlignItems::kFlexEnd;
      } else if (v.text == "center") {
        out.align_self = AlignItems::kCenter;
      } else if (v.text == "baseline") {
        out.align_self = AlignItems::kBaseline;
      } else {
        out.align_self = AlignItems::kStretch;
      }
    }
  }

  // gap (row-gap / column-gap / shorthand).  The shorthand is applied first
  // so an explicit longhand declared later overrides its component.
  auto parse_gap = [&](const std::string& text) -> float {
    if (const std::optional<SizeSpec> spec = ParseSize(text, out.font_size, root_font_size)) {
      return spec.value().value;
    }
    return 0.0f;
  };
  if (const css::Declaration* d = find("gap")) {
    const std::vector<std::string> parts = SplitWhitespace(d->value);
    if (parts.size() >= 2) {
      out.row_gap = parse_gap(parts[0]);
      out.column_gap = parse_gap(parts[1]);
    } else if (parts.size() == 1) {
      out.row_gap = out.column_gap = parse_gap(parts[0]);
    }
  }
  if (const css::Declaration* d = find("row-gap")) {
    out.row_gap = parse_gap(d->value);
  }
  if (const css::Declaration* d = find("column-gap")) {
    out.column_gap = parse_gap(d->value);
  }

  // Grid (CSS Grid Layout 1).  Track templates and item placement.
  if (const css::Declaration* d = find("grid-template-columns")) {
    out.grid_template_columns = ParseGridTrackList(d->value, out.font_size, root_font_size);
  }
  if (const css::Declaration* d = find("grid-template-rows")) {
    out.grid_template_rows = ParseGridTrackList(d->value, out.font_size, root_font_size);
  }
  if (const css::Declaration* d = find("grid-column")) {
    ParseGridPlacementShorthand(d->value, out.grid_column_start, out.grid_column_end);
  }
  if (const css::Declaration* d = find("grid-row")) {
    ParseGridPlacementShorthand(d->value, out.grid_row_start, out.grid_row_end);
  }
  if (const css::Declaration* d = find("grid-column-start")) {
    ParseGridLine(d->value, out.grid_column_start);
  }
  if (const css::Declaration* d = find("grid-column-end")) {
    ParseGridLine(d->value, out.grid_column_end);
  }
  if (const css::Declaration* d = find("grid-row-start")) {
    ParseGridLine(d->value, out.grid_row_start);
  }
  if (const css::Declaration* d = find("grid-row-end")) {
    ParseGridLine(d->value, out.grid_row_end);
  }

  // position.
  if (const css::Declaration* d = find("position")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword) {
      if (v.text == "relative") {
        out.position = Position::kRelative;
      } else if (v.text == "absolute") {
        out.position = Position::kAbsolute;
      } else if (v.text == "fixed") {
        out.position = Position::kFixed;
      }
    }
  }

  // float (CSS 2.2 §9.5): left / right / none.
  if (const css::Declaration* d = find("float")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword) {
      if (v.text == "left") {
        out.floating = Float::kLeft;
      } else if (v.text == "right") {
        out.floating = Float::kRight;
      }
    }
  }

  // appearance (CSS-UI-4 §7.2): none / auto / button.  Other compat values
  // (checkbox, radio, textfield, ...) are not implemented; the declaration
  // is ignored and the computed value stays at the initial value (none).
  if (const css::Declaration* d = find("appearance")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword) {
      if (v.text == "none") {
        out.appearance = Appearance::kNone;
      } else if (v.text == "auto") {
        out.appearance = Appearance::kAuto;
      } else if (v.text == "button") {
        out.appearance = Appearance::kButton;
      }
    }
  }

  // width / height.
  if (const css::Declaration* d = find("width")) {
    out.width = ParseSize(d->value, out.font_size, root_font_size);
  }
  if (const css::Declaration* d = find("height")) {
    out.height = ParseSize(d->value, out.font_size, root_font_size);
  }

  // min-width / min-height / max-width / max-height (CSS 2.2 §10.4).
  // A max value of "none" means "no constraint" (nullopt); "auto" is invalid
  // for these properties and is treated as no constraint.
  auto parse_bounds = [&](const char* name) -> std::optional<SizeSpec> {
    if (const css::Declaration* d = find(name)) {
      const css::CssValue v = css::ParseCssValue(d->value);
      if (v.type == css::CssValue::Type::kKeyword && v.text == "none") {
        return std::nullopt;
      }
      return ParseSize(d->value, out.font_size, root_font_size);
    }
    return std::nullopt;
  };
  out.min_width = parse_bounds("min-width");
  out.max_width = parse_bounds("max-width");
  out.min_height = parse_bounds("min-height");
  out.max_height = parse_bounds("max-height");

  // margin / padding.
  // Each margin side also tracks whether the value was "auto" (only legal for
  // margins).  Auto margins resolve to 0 in normal flow; flex layout uses the
  // flag to distribute free space (CSS Flexbox 1 §8.1).
  auto set_margin = [&](const char* name, SizeSpec& target, bool& auto_flag) {
    if (const css::Declaration* d = find(name)) {
      const css::CssValue v = css::ParseCssValue(d->value);
      if (v.type == css::CssValue::Type::kKeyword && v.text == "auto") {
        target = SizeSpec{};
        auto_flag = true;
      } else if (const std::optional<SizeSpec> spec =
                     ParseSize(d->value, out.font_size, root_font_size)) {
        target = spec.value();
        auto_flag = false;
      }
    }
  };
  set_margin("margin-top", out.margin_top, out.margin_top_auto);
  set_margin("margin-right", out.margin_right, out.margin_right_auto);
  set_margin("margin-bottom", out.margin_bottom, out.margin_bottom_auto);
  set_margin("margin-left", out.margin_left, out.margin_left_auto);
  auto set_padding = [&](const char* name, SizeSpec& target) {
    if (const css::Declaration* d = find(name)) {
      if (const std::optional<SizeSpec> spec = ParseSize(d->value, out.font_size, root_font_size)) {
        target = spec.value();
      }
    }
  };
  set_padding("padding-top", out.padding_top);
  set_padding("padding-right", out.padding_right);
  set_padding("padding-bottom", out.padding_bottom);
  set_padding("padding-left", out.padding_left);

  if (const css::Declaration* d = find("margin")) {
    std::array<SizeSpec, 4> sides = {
        out.margin_top, out.margin_right, out.margin_bottom, out.margin_left};
    std::array<bool, 4> auto_flags = {
        out.margin_top_auto, out.margin_right_auto, out.margin_bottom_auto, out.margin_left_auto};
    ApplyMarginShorthand(d->value, out.font_size, root_font_size, sides, auto_flags);
    out.margin_top = sides[0];
    out.margin_right = sides[1];
    out.margin_bottom = sides[2];
    out.margin_left = sides[3];
    out.margin_top_auto = auto_flags[0];
    out.margin_right_auto = auto_flags[1];
    out.margin_bottom_auto = auto_flags[2];
    out.margin_left_auto = auto_flags[3];
  }
  if (const css::Declaration* d = find("padding")) {
    std::array<SizeSpec, 4> sides = {
        out.padding_top, out.padding_right, out.padding_bottom, out.padding_left};
    ApplyBoxShorthand(d->value, out.font_size, root_font_size, sides);
    out.padding_top = sides[0];
    out.padding_right = sides[1];
    out.padding_bottom = sides[2];
    out.padding_left = sides[3];
  }

  // borders.
  auto set_border_side = [&](std::string_view side, SizeSpec& width) {
    const std::string suffix(side);
    if (const css::Declaration* d = find(std::string("border-") + suffix + "-width")) {
      if (const std::optional<SizeSpec> spec = ParseSize(d->value, out.font_size, root_font_size)) {
        width = spec.value();
      }
    }
    if (const css::Declaration* d = find(std::string("border-") + suffix)) {
      float w = 0;
      std::optional<css::Color> color;
      BorderStyle style = BorderStyle::kNone;
      ParseBorderValue(d->value, out.font_size, root_font_size, w, color, style);
      if (w > 0) {
        width.value = w;
        width.percent = false;
      }
      if (color.has_value()) {
        out.border_color = color;
      }
      if (style != BorderStyle::kNone) {
        out.border_style = style;
      }
    }
  };
  set_border_side("top", out.border_top);
  set_border_side("right", out.border_right);
  set_border_side("bottom", out.border_bottom);
  set_border_side("left", out.border_left);

  if (const css::Declaration* d = find("border-width")) {
    float w = 0;
    std::optional<css::Color> color;
    BorderStyle style = BorderStyle::kNone;
    ParseBorderValue(d->value, out.font_size, root_font_size, w, color, style);
    if (w > 0) {
      out.border_top.value = out.border_right.value = out.border_bottom.value =
          out.border_left.value = w;
      out.border_top.percent = out.border_right.percent = out.border_bottom.percent =
          out.border_left.percent = false;
    }
  }
  if (const css::Declaration* d = find("border-color")) {
    if (const std::optional<css::Color> color = css::ParseColor(d->value)) {
      out.border_color = color;
    }
  }
  if (const css::Declaration* d = find("border")) {
    float w = 0;
    std::optional<css::Color> color;
    BorderStyle style = BorderStyle::kNone;
    ParseBorderValue(d->value, out.font_size, root_font_size, w, color, style);
    if (w > 0) {
      out.border_top.value = out.border_right.value = out.border_bottom.value =
          out.border_left.value = w;
      out.border_top.percent = out.border_right.percent = out.border_bottom.percent =
          out.border_left.percent = false;
    }
    if (color.has_value()) {
      out.border_color = color;
    }
    if (style != BorderStyle::kNone) {
      out.border_style = style;
    }
  }

  // background.
  if (const css::Declaration* d = find("background-color")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kColor) {
      out.background_color = v.color;
    }
  }
  if (const css::Declaration* d = find("background")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kColor) {
      out.background_color = v.color;
    }
  }

  // offsets.
  auto set_offset = [&](const char* name, float& target, bool& auto_flag) {
    if (const css::Declaration* d = find(name)) {
      if (const std::optional<SizeSpec> spec = ParseSize(d->value, out.font_size, root_font_size)) {
        if (!spec.value().percent) {
          target = spec.value().value;
          auto_flag = false;
        }
      }
    }
  };
  set_offset("left", out.left, out.left_auto);
  set_offset("top", out.top, out.top_auto);
  set_offset("right", out.right, out.right_auto);
  set_offset("bottom", out.bottom, out.bottom_auto);

  // Store and recurse.
  styles_[&element] = out;
  for (dom::Node* child : element.ChildNodes()) {
    if (child->node_type() != dom::NodeType::kElement) {
      continue;
    }
    ComputeElement(*static_cast<dom::Element*>(child), out, root_font_size);
  }
}

} // namespace neko::style
