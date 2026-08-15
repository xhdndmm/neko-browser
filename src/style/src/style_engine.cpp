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
#include <cctype>
#include <cstdlib>
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

// Maps a declaration to the physical (property, value) pairs it contributes
// to.  CSS Logical Properties (CSS Logical Properties 1) are translated to
// their physical equivalents; inline/block two-axis shorthands expand into
// both sides.  place-items collapses to align-items (the inline axis is not
// implemented).  Everything else passes through unchanged.
std::vector<std::pair<std::string, std::string>> NormalizeDeclaration(const css::Declaration& decl)
{
  const std::string& p = decl.property;
  if (p == "inline-size")
    return {{"width", decl.value}};
  if (p == "block-size")
    return {{"height", decl.value}};
  if (p == "min-inline-size")
    return {{"min-width", decl.value}};
  if (p == "max-inline-size")
    return {{"max-width", decl.value}};
  if (p == "min-block-size")
    return {{"min-height", decl.value}};
  if (p == "max-block-size")
    return {{"max-height", decl.value}};
  if (p == "margin-inline-start")
    return {{"margin-left", decl.value}};
  if (p == "margin-inline-end")
    return {{"margin-right", decl.value}};
  if (p == "margin-block-start")
    return {{"margin-top", decl.value}};
  if (p == "margin-block-end")
    return {{"margin-bottom", decl.value}};
  if (p == "padding-inline-start")
    return {{"padding-left", decl.value}};
  if (p == "padding-inline-end")
    return {{"padding-right", decl.value}};
  if (p == "padding-block-start")
    return {{"padding-top", decl.value}};
  if (p == "padding-block-end")
    return {{"padding-bottom", decl.value}};
  if (p == "border-block-start")
    return {{"border-top", decl.value}};
  if (p == "border-block-end")
    return {{"border-bottom", decl.value}};
  if (p == "border-inline-start")
    return {{"border-left", decl.value}};
  if (p == "border-inline-end")
    return {{"border-right", decl.value}};
  if (p == "place-items") {
    // The inline (justify) value is not implemented; keep align-items only.
    const std::vector<std::string> parts = SplitWhitespace(decl.value);
    return {{"align-items", parts.empty() ? "" : parts[0]}};
  }
  // Two-axis shorthands: one value applies to both axes; two values are
  // start/end (inline) or block-start/block-end (block) — i.e. left/right or
  // top/bottom in a horizontal LTR writing mode.
  auto two_axis = [&](const char* first, const char* second) {
    const std::vector<std::string> parts = SplitWhitespace(decl.value);
    if (parts.size() >= 2) {
      return std::vector<std::pair<std::string, std::string>>{{first, parts[0]},
                                                              {second, parts[1]}};
    }
    const std::string value = parts.empty() ? std::string() : parts[0];
    return std::vector<std::pair<std::string, std::string>>{{first, value}, {second, value}};
  };
  if (p == "margin-inline")
    return two_axis("margin-left", "margin-right");
  if (p == "margin-block")
    return two_axis("margin-top", "margin-bottom");
  if (p == "padding-inline")
    return two_axis("padding-left", "padding-right");
  if (p == "padding-block")
    return two_axis("padding-top", "padding-bottom");
  return {{p, decl.value}};
}

// Substitutes var(--name[, fallback]) references in a value using
// |custom_properties| (CSS Custom Properties for Cascading Variables Level 1
// §3.1).  Returns nullopt when a var() reference cannot be resolved and has
// no fallback — per spec the declaration is then invalid at computed-value
// time (treated as unset).  Nested var() inside a fallback is not supported
// (documented limitation).
std::optional<std::string> ResolveVars(const std::string& value,
                                       const std::map<std::string, std::string>& custom_properties)
{
  std::string out;
  out.reserve(value.size());
  std::size_t pos = 0;
  while (pos < value.size()) {
    const std::size_t start = value.find("var(", pos);
    if (start == std::string::npos) {
      out.append(value, pos, std::string::npos);
      break;
    }
    out.append(value, pos, start - pos);
    const std::size_t close = value.find(')', start + 4);
    if (close == std::string::npos) {
      return std::nullopt;
    }
    const std::string body = value.substr(start + 4, close - start - 4);
    const std::size_t comma = body.find(',');
    const std::string name = std::string(neko::base::Trim(body.substr(0, comma)));
    const auto it = custom_properties.find(name);
    if (it != custom_properties.end()) {
      out += it->second;
    } else if (comma != std::string::npos) {
      out += std::string(neko::base::Trim(body.substr(comma + 1)));
    } else {
      return std::nullopt;
    }
    pos = close + 1;
  }
  return out;
}

// Context for resolving a CSS length: font-relative (em/rem) and viewport-
// relative (vw/vh/vmin/vmax) units resolve here.  The viewport defaults match
// the engine's documented viewport (800x600 — the same values
// window.innerWidth/Height report); wiring the real window size is future
// work.
struct SizeContext
{
  float font_size = 16;
  float root_font_size = 16;
  float viewport_width = 800;
  float viewport_height = 600;
};

// A tiny recursive-descent parser for calc() expressions, which are linear
// combinations of lengths and percentages:
//   <sum>  := <term> (('+' | '-') <term>)*
//   <term> := number[unit] | '(' <sum> ')'
//   unit   := px | em | rem | % | vw | vh | vmin | vmax | (none)
// Multiplication/division inside calc() are not implemented (documented
// limitation).
struct CalcParser
{
  std::string_view s;
  std::size_t i = 0;
  const SizeContext& ctx;

  explicit CalcParser(std::string_view text, const SizeContext& c) : s(text), ctx(c) {}

  void SkipWs()
  {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
      ++i;
    }
  }

  bool IsDigit(char c) const
  {
    return c >= '0' && c <= '9';
  }

  std::optional<CalcTerm> ParseSum()
  {
    SkipWs();
    const std::optional<CalcTerm> first = ParseTerm();
    if (!first.has_value()) {
      return std::nullopt;
    }
    CalcTerm acc = first.value();
    for (;;) {
      SkipWs();
      if (i >= s.size()) {
        break;
      }
      const char op = s[i];
      if (op != '+' && op != '-') {
        break;
      }
      ++i;
      const std::optional<CalcTerm> rhs = ParseTerm();
      if (!rhs.has_value()) {
        return std::nullopt;
      }
      if (op == '+') {
        acc.offset += rhs->offset;
        acc.percent += rhs->percent;
      } else {
        acc.offset -= rhs->offset;
        acc.percent -= rhs->percent;
      }
    }
    return acc;
  }

  std::optional<CalcTerm> ParseTerm()
  {
    SkipWs();
    // An optional "calc" keyword: "calc(expr)" is equivalent to "(expr)" and
    // appears when min()/max()/clamp() arguments are themselves calc() calls.
    if (i + 4 <= s.size()) {
      const char a = s[i];
      const char b = s[i + 1];
      const char c = s[i + 2];
      const char d = s[i + 3];
      if ((a == 'c' || a == 'C') && (b == 'a' || b == 'A') && (c == 'l' || c == 'L') &&
          (d == 'c' || d == 'C') && i + 4 < s.size() && s[i + 4] == '(') {
        i += 4;
      }
    }
    if (i < s.size() && s[i] == '(') {
      ++i;
      const std::optional<CalcTerm> inner = ParseSum();
      SkipWs();
      if (!inner.has_value() || i >= s.size() || s[i] != ')') {
        return std::nullopt;
      }
      ++i;
      return inner;
    }
    return ParseNumber();
  }

  std::optional<CalcTerm> ParseNumber()
  {
    SkipWs();
    const std::size_t start = i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
      ++i;
    }
    bool digits = false;
    while (i < s.size() && IsDigit(s[i])) {
      digits = true;
      ++i;
    }
    if (i < s.size() && s[i] == '.') {
      ++i;
      while (i < s.size() && IsDigit(s[i])) {
        digits = true;
        ++i;
      }
    }
    if (!digits) {
      return std::nullopt;
    }
    const float number = std::strtof(std::string(s.substr(start, i - start)).c_str(), nullptr);
    const std::size_t unit_start = i;
    while (i < s.size() &&
           (s[i] == '%' || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z'))) {
      ++i;
    }
    std::string unit(s.substr(unit_start, i - unit_start));
    for (char& c : unit) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    CalcTerm term;
    if (unit == "%") {
      term.percent = number;
    } else if (unit == "px") {
      term.offset = number;
    } else if (unit == "em") {
      term.offset = number * ctx.font_size;
    } else if (unit == "rem") {
      term.offset = number * ctx.root_font_size;
    } else if (unit == "vw") {
      term.offset = number * ctx.viewport_width / 100.0f;
    } else if (unit == "vh") {
      term.offset = number * ctx.viewport_height / 100.0f;
    } else if (unit == "vmin") {
      term.offset = number * std::min(ctx.viewport_width, ctx.viewport_height) / 100.0f;
    } else if (unit == "vmax") {
      term.offset = number * std::max(ctx.viewport_width, ctx.viewport_height) / 100.0f;
    } else if (unit.empty()) {
      term.offset = number; // unitless (valid inside calc for 0 etc.)
    } else {
      return std::nullopt; // unknown unit
    }
    return term;
  }
};

// Parses the comma-separated arguments of min()/max()/clamp(), each of which
// is a calc expression.  Commas inside parentheses do not split.
std::optional<std::vector<CalcTerm>> ParseCalcArguments(std::string_view text,
                                                        const SizeContext& ctx)
{
  std::vector<std::string_view> parts;
  std::size_t begin = 0;
  int depth = 0;
  for (std::size_t pos = 0; pos < text.size(); ++pos) {
    const char c = text[pos];
    if (c == '(') {
      ++depth;
    } else if (c == ')') {
      if (depth > 0) {
        --depth;
      }
    } else if (c == ',' && depth == 0) {
      parts.push_back(text.substr(begin, pos - begin));
      begin = pos + 1;
    }
  }
  parts.push_back(text.substr(begin));
  std::vector<CalcTerm> args;
  args.reserve(parts.size());
  for (const std::string_view part : parts) {
    CalcParser parser(part, ctx);
    const std::optional<CalcTerm> term = parser.ParseSum();
    parser.SkipWs();
    if (!term.has_value() || parser.i != part.size()) {
      return std::nullopt;
    }
    args.push_back(term.value());
  }
  return args;
}

// Parses calc(...)/min(...)/max(...)/clamp(...) into a SizeSpec whose
// resolution against the containing block happens at layout time.  Returns
// nullopt when the expression cannot be parsed.
std::optional<SizeSpec> ParseMathFunction(const std::string& text, const SizeContext& ctx)
{
  const std::string lower = neko::base::ToLower(text);
  const bool is_calc = lower.rfind("calc(", 0) == 0;
  const bool is_min = lower.rfind("min(", 0) == 0;
  const bool is_max = lower.rfind("max(", 0) == 0;
  const bool is_clamp = lower.rfind("clamp(", 0) == 0;
  if (!is_calc && !is_min && !is_max && !is_clamp) {
    return std::nullopt;
  }
  const std::size_t open = text.find('(');
  const std::size_t close = text.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close < open) {
    return std::nullopt;
  }
  const std::string_view inner(text.data() + open + 1, close - open - 1);

  if (is_calc) {
    CalcParser parser(inner, ctx);
    const std::optional<CalcTerm> term = parser.ParseSum();
    parser.SkipWs();
    if (!term.has_value() || parser.i != inner.size()) {
      return std::nullopt;
    }
    SizeSpec spec;
    spec.is_calc = true;
    spec.calc = term.value();
    return spec;
  }
  const std::optional<std::vector<CalcTerm>> args = ParseCalcArguments(inner, ctx);
  if (!args.has_value()) {
    return std::nullopt;
  }
  if (is_clamp) {
    if (args->size() != 3) {
      return std::nullopt;
    }
    SizeSpec spec;
    spec.is_clamp = true;
    spec.extremum_args = args.value();
    return spec;
  }
  if (args->size() < 2) {
    return std::nullopt;
  }
  SizeSpec spec;
  spec.is_extremum = true;
  spec.extremum_is_max = is_max;
  spec.extremum_args = args.value();
  return spec;
}

// Resolves a SizeSpec to an absolute length against a containing value.
// Percentages resolve against |containing|; calc()/min()/max()/clamp()
// combinations resolve against it too.  Used where a resolved px value is
// needed at style time (e.g. font-size, where percentages are relative to the
// inherited size).
float ResolveSpec(const SizeSpec& spec, float containing)
{
  if (spec.is_clamp) {
    const auto resolve = [&](const CalcTerm& t) {
      return containing * t.percent / 100.0f + t.offset;
    };
    if (spec.extremum_args.size() != 3) {
      return 0;
    }
    return std::max(resolve(spec.extremum_args[0]),
                    std::min(resolve(spec.extremum_args[1]), resolve(spec.extremum_args[2])));
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

// Parses a single value into a SizeSpec (resolving em/rem against font sizes
// and vw/vh against the viewport; calc()/min()/max()/clamp() are kept for
// layout-time resolution).
std::optional<SizeSpec> ParseSize(const std::string& text, const SizeContext& ctx)
{
  if (const std::optional<SizeSpec> math = ParseMathFunction(text, ctx)) {
    return math;
  }
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
    spec.value = value.value * ctx.font_size;
  } else if (value.unit == "rem") {
    spec.value = value.value * ctx.root_font_size;
  } else if (value.unit == "vw") {
    spec.value = value.value * ctx.viewport_width / 100.0f;
  } else if (value.unit == "vh") {
    spec.value = value.value * ctx.viewport_height / 100.0f;
  } else if (value.unit == "vmin") {
    spec.value = value.value * std::min(ctx.viewport_width, ctx.viewport_height) / 100.0f;
  } else if (value.unit == "vmax") {
    spec.value = value.value * std::max(ctx.viewport_width, ctx.viewport_height) / 100.0f;
  } else {
    spec.value = value.value;
  }
  return spec;
}

// Applies a 1-4 value shorthand to the four sides.
void ApplyBoxShorthand(const std::string& value,
                       const SizeContext& ctx,
                       std::array<SizeSpec, 4>& sides)
{
  const std::vector<std::string> parts = SplitWhitespace(value);
  auto parse = [&](const std::string& text) -> SizeSpec {
    if (const std::optional<SizeSpec> spec = ParseSize(text, ctx)) {
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
                          const SizeContext& ctx,
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
    if (const std::optional<SizeSpec> spec = ParseSize(text, ctx)) {
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
                      const SizeContext& ctx,
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
      if (const std::optional<SizeSpec> spec = ParseSize(part, ctx)) {
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
                         const SizeContext& ctx,
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
            ParseGridTrackToken(track, ctx, out);
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
  if (const std::optional<SizeSpec> spec = ParseSize(token, ctx)) {
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

std::vector<GridTrack> ParseGridTrackList(const std::string& value, const SizeContext& ctx)
{
  std::vector<GridTrack> tracks;
  for (const std::string& token : SplitGridList(value)) {
    ParseGridTrackToken(token, ctx, tracks);
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
  // Collect author sheets from <style> elements.  Externally loaded
  // <link rel=stylesheet> sheets (registered via SetExternalStylesheets) are
  // iterated directly by the cascade in ComputeElement — no per-Apply copy.
  author_sheets_.clear();
  std::vector<dom::Element*> style_elements = dom::QuerySelectorAll(document, "style");
  for (dom::Element* style : style_elements) {
    const std::string text = style->TextContent();
    // Reuse the parsed sheet when the element's text is unchanged (scripts
    // mutate the DOM every timer tick; re-parsing identical <style> content
    // on each pass is wasted work).
    auto it = author_parse_cache_.find(style);
    if (it != author_parse_cache_.end() && it->second.first == text) {
      author_sheets_.push_back(it->second.second); // copy; sheets are small
      continue;
    }
    css::StyleSheet sheet = css::ParseStyleSheet(text);
    author_sheets_.push_back(sheet);
    author_parse_cache_[style] = std::make_pair(text, std::move(sheet));
  }
  // Prune cache entries for <style> elements that no longer exist.
  if (author_parse_cache_.size() > style_elements.size()) {
    for (auto it = author_parse_cache_.begin(); it != author_parse_cache_.end();) {
      const bool alive = std::find(style_elements.begin(), style_elements.end(), it->first) !=
                         style_elements.end();
      it = alive ? std::next(it) : author_parse_cache_.erase(it);
    }
  }

  // Rebuild the rule index (cheap relative to the cascade) so per-element
  // matching only considers candidate rules.
  BuildCascadeIndex(document);

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

void StyleEngine::SetExternalStylesheets(std::vector<css::StyleSheet> sheets)
{
  external_sheets_ = std::move(sheets);
}

// The parsed HTML user-agent stylesheet, parsed once.  Returns a reference to
// a function-local static (NOT a copy — a `[] { static ...; return sheet; }()`
// lambda deduces a by-value return and leaves the caller with a dangling
// reference to a destroyed temporary, which is a use-after-free).
const css::StyleSheet& UaSheet()
{
  static const css::StyleSheet sheet = css::ParseStyleSheet(kUaStylesheet);
  return sheet;
}

void StyleEngine::BuildCascadeIndex(dom::Document& /*document*/)
{
  auto buckets = std::make_unique<CascadeBuckets>();

  const css::StyleSheet& ua = UaSheet();

  auto add_rule = [&buckets](const css::StyleRule& rule) {
    const int rule_index = static_cast<int>(buckets->rules.size());
    IndexedCascadeRule indexed;
    indexed.rule = &rule;
    indexed.order = rule_index;
    for (const css::ComplexSelector& selector : rule.selectors) {
      indexed.specificities.push_back(SelectorSpecificity(selector));
      if (selector.compounds.empty()) {
        continue;
      }
      // Bucket by the rightmost compound's key.  A compound that carries both
      // an id and classes is keyed by the id only (most selective; an element
      // without the id cannot match).  Class compounds are keyed by every
      // class so an element carrying any of them finds the rule; full
      // matching still runs on the candidates.  Tag/pseudo/attribute-only
      // compounds use the tag bucket or the universal bucket.
      const css::CompoundSelector& rightmost = selector.compounds.back();
      if (rightmost.id.has_value()) {
        buckets->by_id[*rightmost.id].push_back(rule_index);
      } else if (!rightmost.classes.empty()) {
        for (const std::string& cls : rightmost.classes) {
          buckets->by_class[cls].push_back(rule_index);
        }
      } else if (rightmost.tag.has_value()) {
        buckets->by_tag[*rightmost.tag].push_back(rule_index);
      } else {
        buckets->universal.push_back(rule_index);
      }
    }
    buckets->rules.push_back(std::move(indexed));
  };

  auto add_sheet = [&add_rule](const css::StyleSheet& sheet) {
    for (const css::StyleRule& rule : sheet.rules) {
      add_rule(rule);
    }
    for (const css::AtRule& at_rule : sheet.at_rules) {
      if (at_rule.name == "media" && !MediaQueryMatches(at_rule.prelude)) {
        continue;
      }
      for (const css::StyleRule& rule : at_rule.rules) {
        add_rule(rule);
      }
    }
  };

  add_sheet(ua);
  for (const css::StyleSheet& sheet : author_sheets_) {
    add_sheet(sheet);
  }
  for (const css::StyleSheet& sheet : external_sheets_) {
    add_sheet(sheet);
  }

  buckets_ = std::move(buckets);
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

  if (buckets_ != nullptr) {
    // Gather candidate rules from the buckets keyed by this element's id,
    // classes and tag, plus the universal bucket.  A rule can be reachable
    // through several buckets (id + class, or multiple classes); dedupe by
    // sorting the indices, which also restores document order for the
    // cascade tie-break.
    const CascadeBuckets& buckets = *buckets_;
    std::vector<int> candidate_indices;
    candidate_indices.reserve(16);
    candidate_indices.insert(
        candidate_indices.end(), buckets.universal.begin(), buckets.universal.end());
    if (const std::optional<std::string_view> id = element.Id()) {
      const std::string id_key(*id);
      const auto it = buckets.by_id.find(id_key);
      if (it != buckets.by_id.end()) {
        candidate_indices.insert(candidate_indices.end(), it->second.begin(), it->second.end());
      }
    }
    for (const std::string_view cls : element.ClassList()) {
      const std::string cls_key(cls);
      const auto it = buckets.by_class.find(cls_key);
      if (it != buckets.by_class.end()) {
        candidate_indices.insert(candidate_indices.end(), it->second.begin(), it->second.end());
      }
    }
    {
      const std::string tag_key(element.tag_name());
      const auto it = buckets.by_tag.find(tag_key);
      if (it != buckets.by_tag.end()) {
        candidate_indices.insert(candidate_indices.end(), it->second.begin(), it->second.end());
      }
    }
    std::sort(candidate_indices.begin(), candidate_indices.end());
    candidate_indices.erase(std::unique(candidate_indices.begin(), candidate_indices.end()),
                            candidate_indices.end());

    for (const int rule_index : candidate_indices) {
      const IndexedCascadeRule& indexed = buckets.rules[static_cast<std::size_t>(rule_index)];
      for (std::size_t si = 0; si < indexed.rule->selectors.size(); ++si) {
        if (!css::MatchesSelector(element, indexed.rule->selectors[si])) {
          continue;
        }
        const css::Specificity& specificity = indexed.specificities[si];
        for (const css::Declaration& declaration : indexed.rule->declarations) {
          candidates.push_back(Candidate{&declaration, specificity, order++});
        }
      }
    }
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

  // Cascade: importance > specificity > order.  Declarations are normalized
  // to their physical properties first (logical properties expanded, see
  // NormalizeDeclaration), and the winning value per property is stored so
  // var() references can be resolved after the custom properties are known.
  struct Winner
  {
    const css::Declaration* declaration; // for the !important flag
    css::Specificity specificity;
    int order;
    std::string value; // physical (normalized) value text
  };
  std::map<std::string, Winner> winners;
  for (const Candidate& candidate : candidates) {
    const bool important = candidate.declaration->important;
    for (auto& normalized : NormalizeDeclaration(*candidate.declaration)) {
      const std::string& property = normalized.first;
      const std::string& value = normalized.second;
      auto existing = winners.find(property);
      if (existing == winners.end()) {
        winners.emplace(
            property, Winner{candidate.declaration, candidate.specificity, candidate.order, value});
        continue;
      }
      const Winner& current = existing->second;
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
        existing->second =
            Winner{candidate.declaration, candidate.specificity, candidate.order, value};
      }
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
  out.white_space = inherited.white_space;
  out.custom_properties = inherited.custom_properties;

  // CSS custom properties (CSS Custom Properties for Cascading Variables
  // Level 1 §2): inherited by default, then overridden by this element's
  // winning declarations.  var() inside a custom property value resolves
  // against the inherited set (approximation: a reference to a property
  // defined on this same element resolves only if it sorts earlier in the
  // map; chained custom properties on one element are a documented
  // limitation).
  for (const auto& entry : winners) {
    if (entry.first.rfind("--", 0) != 0) {
      continue;
    }
    if (const std::optional<std::string> resolved =
            ResolveVars(entry.second.value, out.custom_properties)) {
      out.custom_properties[entry.first] = *resolved;
    }
  }

  // Per-element resolved declarations: physical property names with var()
  // substituted.  find() returns pointers into this map, so the resolution is
  // transparent to the property handlers below.  A declaration whose var()
  // could not be resolved is dropped (invalid at computed-value time).
  std::map<std::string, css::Declaration> resolved_decls;
  for (const auto& entry : winners) {
    if (entry.first.rfind("--", 0) == 0) {
      continue; // custom properties are not applied to ComputedStyle directly
    }
    const std::optional<std::string> resolved =
        ResolveVars(entry.second.value, out.custom_properties);
    if (!resolved.has_value()) {
      continue;
    }
    css::Declaration decl;
    decl.property = entry.first;
    decl.value = *resolved;
    decl.important = entry.second.declaration->important;
    resolved_decls.emplace(entry.first, std::move(decl));
  }

  auto find = [&](std::string_view property) -> const css::Declaration* {
    const auto it = resolved_decls.find(std::string(property));
    return it == resolved_decls.end() ? nullptr : &it->second;
  };

  const bool font_size_set = find("font-size") != nullptr;
  const bool line_height_set = find("line-height") != nullptr;

  // font-size first (em/rem depend on it).  Percentages are relative to the
  // inherited size; viewport units to the engine viewport; calc()/min()/max()/
  // clamp() resolve at style time (all arguments are absolute once units are
  // applied, so the containing value is only used for percentages).
  if (const css::Declaration* d = find("font-size")) {
    const SizeContext inherited_ctx{inherited.font_size, root_font_size};
    if (const std::optional<SizeSpec> math = ParseMathFunction(d->value, inherited_ctx)) {
      out.font_size = ResolveSpec(math.value(), inherited.font_size);
    } else {
      const css::CssValue v = css::ParseCssValue(d->value);
      if (v.type == css::CssValue::Type::kLength) {
        if (v.is_percent) {
          out.font_size = inherited.font_size * v.value / 100.0f;
        } else if (v.unit == "em") {
          out.font_size = inherited.font_size * v.value;
        } else if (v.unit == "rem") {
          out.font_size = root_font_size * v.value;
        } else if (v.unit == "vw") {
          out.font_size = v.value * inherited_ctx.viewport_width / 100.0f;
        } else if (v.unit == "vh") {
          out.font_size = v.value * inherited_ctx.viewport_height / 100.0f;
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
  }

  // Length context for the remaining properties: font size is final now, so
  // em/rem and viewport units resolve against it.
  const SizeContext size_ctx{out.font_size, root_font_size};

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
    } else if (const std::optional<SizeSpec> spec = ParseSize(d->value, size_ctx)) {
      out.flex_basis = spec.value();
    }
  }

  // flex shorthand: [<'flex-grow'> <'flex-shrink'>? || <'flex-basis'>]
  // (CSS Flexbox 1 §7.1).  Omitted components reset to their initial values:
  // grow=1, shrink=1, basis=auto.
  if (const css::Declaration* d = find("flex")) {
    const std::vector<std::string> parts = SplitWhitespace(d->value);
    auto parse_number = [](const std::string& s) -> std::optional<float> {
      const css::CssValue v = css::ParseCssValue(s);
      if (v.type == css::CssValue::Type::kNumber) {
        return std::max(0.0f, v.number);
      }
      return std::nullopt;
    };
    // Reset to initial values first so omitted components are restored.
    out.flex_grow = 1;
    out.flex_shrink = 1;
    out.flex_basis = std::nullopt; // auto

    if (parts.size() == 1 && parts[0] == "none") {
      // none = 0 0 auto
      out.flex_grow = 0;
      out.flex_shrink = 0;
    } else if (parts.size() == 1 && parts[0] == "auto") {
      // auto = 1 1 auto (already the reset values)
    } else if (parts.size() == 1 && parts[0] == "initial") {
      // initial = 0 1 auto
      out.flex_grow = 0;
    } else if (parts.size() == 1) {
      // flex: <grow> == flex: <grow> 1 0
      if (const std::optional<float> g = parse_number(parts[0])) {
        out.flex_grow = g.value();
        out.flex_shrink = 1;
        out.flex_basis = SizeSpec{0, false};
      }
    } else if (parts.size() >= 2) {
      // First component is grow (a number); a non-number second is basis.
      std::size_t next = 1;
      if (const std::optional<float> g = parse_number(parts[0])) {
        out.flex_grow = g.value();
      } else {
        // grow must be a number; otherwise the shorthand is invalid.
        next = parts.size();
      }
      if (next < parts.size()) {
        if (const std::optional<float> s = parse_number(parts[next])) {
          out.flex_shrink = s.value();
          ++next;
        }
      }
      // A remaining component that is a length is the basis.  If the list is
      // exhausted after grow/shrink, the omitted basis is 0.
      if (next < parts.size()) {
        const std::string& basis = parts[next];
        if (basis != "auto") {
          if (const std::optional<SizeSpec> spec = ParseSize(basis, size_ctx)) {
            out.flex_basis = spec.value();
          }
        } else {
          out.flex_basis = std::nullopt;
        }
      } else if (parts.size() >= 2 && parse_number(parts[0]).has_value()) {
        // Two or more components with no length: basis resolves to 0.
        out.flex_basis = SizeSpec{0, false};
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
    if (const std::optional<SizeSpec> spec = ParseSize(text, size_ctx)) {
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
    out.grid_template_columns = ParseGridTrackList(d->value, size_ctx);
  }
  if (const css::Declaration* d = find("grid-template-rows")) {
    out.grid_template_rows = ParseGridTrackList(d->value, size_ctx);
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

  // box-sizing (CSS-UI-3 §6): content-box (initial) / border-box.  Unknown
  // values are ignored (stay at the initial value).
  if (const css::Declaration* d = find("box-sizing")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword && v.text == "border-box") {
      out.box_sizing = BoxSizing::kBorderBox;
    }
  }

  // white-space (CSS Text 3 §3): normal (initial) / nowrap / pre / pre-wrap /
  // pre-line.  Layout honors normal and nowrap; the pre* values are parsed
  // but treated as normal (documented limitation).
  if (const css::Declaration* d = find("white-space")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword) {
      if (v.text == "nowrap") {
        out.white_space = WhiteSpace::kNowrap;
      } else if (v.text == "pre") {
        out.white_space = WhiteSpace::kPre;
      } else if (v.text == "pre-wrap") {
        out.white_space = WhiteSpace::kPreWrap;
      } else if (v.text == "pre-line") {
        out.white_space = WhiteSpace::kPreLine;
      }
    }
  }

  // overflow (CSS Overflow 3): visible (initial) / hidden / auto / scroll.
  // The computed pair is stored on one axis for simplicity; auto/scroll are
  // treated as hidden (they clip, but no scrollable overflow is exposed).
  if (const css::Declaration* d = find("overflow")) {
    const css::CssValue v = css::ParseCssValue(d->value);
    if (v.type == css::CssValue::Type::kKeyword) {
      if (v.text == "hidden") {
        out.overflow = Overflow::kHidden;
      } else if (v.text == "auto") {
        out.overflow = Overflow::kAuto;
      } else if (v.text == "scroll") {
        out.overflow = Overflow::kScroll;
      }
    }
  }

  // width / height.
  if (const css::Declaration* d = find("width")) {
    out.width = ParseSize(d->value, size_ctx);
  }
  if (const css::Declaration* d = find("height")) {
    out.height = ParseSize(d->value, size_ctx);
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
      return ParseSize(d->value, size_ctx);
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
      } else if (const std::optional<SizeSpec> spec = ParseSize(d->value, size_ctx)) {
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
      if (const std::optional<SizeSpec> spec = ParseSize(d->value, size_ctx)) {
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
    ApplyMarginShorthand(d->value, size_ctx, sides, auto_flags);
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
    ApplyBoxShorthand(d->value, size_ctx, sides);
    out.padding_top = sides[0];
    out.padding_right = sides[1];
    out.padding_bottom = sides[2];
    out.padding_left = sides[3];
  }

  // borders.
  auto set_border_side = [&](std::string_view side, SizeSpec& width) {
    const std::string suffix(side);
    if (const css::Declaration* d = find(std::string("border-") + suffix + "-width")) {
      if (const std::optional<SizeSpec> spec = ParseSize(d->value, size_ctx)) {
        width = spec.value();
      }
    }
    if (const css::Declaration* d = find(std::string("border-") + suffix)) {
      float w = 0;
      std::optional<css::Color> color;
      BorderStyle style = BorderStyle::kNone;
      ParseBorderValue(d->value, size_ctx, w, color, style);
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
    ParseBorderValue(d->value, size_ctx, w, color, style);
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
    ParseBorderValue(d->value, size_ctx, w, color, style);
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

  // aspect-ratio (CSS Box Sizing 4): "1", "16 / 9" or "1 / 1" (single number
  // means N / 1).  Stored as the width/height ratio.
  if (const css::Declaration* d = find("aspect-ratio")) {
    std::string cleaned;
    cleaned.reserve(d->value.size());
    for (const char c : d->value) {
      if (c != ' ' && c != '\t') {
        cleaned.push_back(c);
      }
    }
    const std::size_t slash = cleaned.find('/');
    float w = 0;
    float h = 1;
    bool ok = false;
    if (slash == std::string::npos) {
      const css::CssValue v = css::ParseCssValue(cleaned);
      if (v.type == css::CssValue::Type::kNumber) {
        w = v.number;
        ok = true;
      }
    } else {
      const css::CssValue v1 = css::ParseCssValue(cleaned.substr(0, slash));
      const css::CssValue v2 = css::ParseCssValue(cleaned.substr(slash + 1));
      if (v1.type == css::CssValue::Type::kNumber && v2.type == css::CssValue::Type::kNumber) {
        w = v1.number;
        h = v2.number;
        ok = true;
      }
    }
    if (ok && w > 0 && h > 0) {
      out.aspect_ratio = w / h;
    }
  }

  // border-radius (CSS Backgrounds and Borders 3 §5): a single radius for
  // all corners (length or percentage; 1-4 value / elliptical variants are
  // not implemented).
  if (const css::Declaration* d = find("border-radius")) {
    if (const std::optional<SizeSpec> spec = ParseSize(d->value, size_ctx)) {
      out.border_radius = spec.value();
    }
  }

  // offsets.
  auto set_offset = [&](const char* name, float& target, bool& auto_flag) {
    if (const css::Declaration* d = find(name)) {
      if (const std::optional<SizeSpec> spec = ParseSize(d->value, size_ctx)) {
        if (!spec.value().percent && !spec.value().is_calc && !spec.value().is_extremum) {
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
