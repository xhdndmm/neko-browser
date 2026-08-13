#include "neko/style/style_engine.h"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "neko/base/logging.h"
#include "neko/base/string_util.h"
#include "neko/css/parser.h"
#include "neko/css/selector.h"
#include "neko/css/stylesheet.h"
#include "neko/css/value.h"
#include "neko/dom/query.h"

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
a, span, em, strong, b, i, u, s, small, sub, sup, code, label, button,
select, textarea, input, q, cite, mark, time { display: inline; }
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
std::vector<std::string> SplitWhitespace(std::string_view value) {
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
std::optional<SizeSpec> ParseSize(const std::string& text, float font_size,
                                  float root_font_size) {
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
void ApplyBoxShorthand(const std::string& value, float font_size, float root_font_size,
                       std::array<SizeSpec, 4>& sides) {
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

// Extracts width/color/style from a border value ("1px solid #000").
void ParseBorderValue(const std::string& value, float font_size, float root_font_size,
                      float& width, std::optional<css::Color>& color,
                      BorderStyle& style) {
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

bool MediaQueryMatches(std::string_view prelude) {
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

}  // namespace

void StyleEngine::ApplyStyles(dom::Document& document) {
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

const ComputedStyle& StyleEngine::StyleFor(const dom::Element& element) const {
  const auto it = styles_.find(&element);
  if (it != styles_.end()) {
    return it->second;
  }
  static const ComputedStyle kDefault;
  return kDefault;
}

void StyleEngine::ComputeElement(dom::Element& element, const ComputedStyle& inherited,
                                 float root_font_size) {
  // Collect all matching declarations for this element.
  struct Candidate {
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
      static const std::map<std::string, float> kSizes = {
          {"xx-small", 9.0f}, {"x-small", 10.0f}, {"small", 13.0f}, {"medium", 16.0f},
          {"large", 18.0f},   {"x-large", 24.0f}, {"xx-large", 32.0f}};
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
      if (v.text == "block" || v.text == "flex" || v.text == "grid") {
        out.display = Display::kBlock;
      } else if (v.text == "inline" || v.text == "inline-block") {
        out.display = Display::kInline;
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

  // width / height.
  if (const css::Declaration* d = find("width")) {
    out.width = ParseSize(d->value, out.font_size, root_font_size);
  }
  if (const css::Declaration* d = find("height")) {
    out.height = ParseSize(d->value, out.font_size, root_font_size);
  }

  // margin / padding.
  auto set_margin = [&](const char* name, SizeSpec& target) {
    if (const css::Declaration* d = find(name)) {
      if (const std::optional<SizeSpec> spec = ParseSize(d->value, out.font_size, root_font_size)) {
        target = spec.value();
      }
    }
  };
  set_margin("margin-top", out.margin_top);
  set_margin("margin-right", out.margin_right);
  set_margin("margin-bottom", out.margin_bottom);
  set_margin("margin-left", out.margin_left);
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
    std::array<SizeSpec, 4> sides = {out.margin_top, out.margin_right, out.margin_bottom,
                                     out.margin_left};
    ApplyBoxShorthand(d->value, out.font_size, root_font_size, sides);
    out.margin_top = sides[0];
    out.margin_right = sides[1];
    out.margin_bottom = sides[2];
    out.margin_left = sides[3];
  }
  if (const css::Declaration* d = find("padding")) {
    std::array<SizeSpec, 4> sides = {out.padding_top, out.padding_right, out.padding_bottom,
                                     out.padding_left};
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
      out.border_top.value = out.border_right.value = out.border_bottom.value = out.border_left.value = w;
      out.border_top.percent = out.border_right.percent = out.border_bottom.percent = out.border_left.percent = false;
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
      out.border_top.value = out.border_right.value = out.border_bottom.value = out.border_left.value = w;
      out.border_top.percent = out.border_right.percent = out.border_bottom.percent = out.border_left.percent = false;
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
  auto set_offset = [&](const char* name, float& target) {
    if (const css::Declaration* d = find(name)) {
      if (const std::optional<SizeSpec> spec = ParseSize(d->value, out.font_size, root_font_size)) {
        if (!spec.value().percent) {
          target = spec.value().value;
        }
      }
    }
  };
  set_offset("left", out.left);
  set_offset("top", out.top);

  // Store and recurse.
  styles_[&element] = out;
  for (dom::Node* child : element.ChildNodes()) {
    if (child->node_type() != dom::NodeType::kElement) {
      continue;
    }
    ComputeElement(*static_cast<dom::Element*>(child), out, root_font_size);
  }
}

}  // namespace neko::style
