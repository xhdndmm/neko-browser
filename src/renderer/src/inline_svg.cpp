#include "inline_svg.h"

#include "neko/dom/element.h"
#include "neko/dom/node.h"
#include "neko/style/style_engine.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <string_view>

namespace neko::renderer {
namespace {

// CurrentColor in SVG is defined as the CSS `color` value on the element
// (inherited).  The standalone rasterizer has no cascade, so the serializer
// substitutes it per element.
constexpr std::string_view kCurrentColor = "currentcolor";

std::string ToLowerCopy(std::string_view text)
{
  std::string lower(text);
  for (char& c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return lower;
}

std::string SubstituteCurrentColor(std::string_view value, std::string_view color_hex)
{
  if (value.empty()) {
    return std::string(value);
  }
  const std::string lower = ToLowerCopy(value);
  if (lower.find(kCurrentColor) == std::string::npos) {
    return std::string(value);
  }
  std::string out;
  out.reserve(value.size() + 8);
  std::size_t pos = 0;
  for (;;) {
    const std::size_t at = lower.find(kCurrentColor, pos);
    if (at == std::string::npos) {
      out.append(value.substr(pos));
      return out;
    }
    out.append(value.substr(pos, at - pos));
    out.append(color_hex);
    pos = at + kCurrentColor.size();
  }
}

void AppendEscaped(std::string& out, std::string_view text, bool attribute)
{
  for (const char c : text) {
    switch (c) {
    case '&':
      out.append("&amp;");
      break;
    case '<':
      out.append("&lt;");
      break;
    case '>':
      out.append("&gt;");
      break;
    case '"':
      out.append(attribute ? "&quot;" : "\"");
      break;
    default:
      out.push_back(c);
      break;
    }
  }
}

// The computed color as #RRGGBB.  The SVG color parser accepts 3/6-digit hex
// but not 8-digit (alpha is an independent fill-opacity / stop-opacity
// concept in SVG 1.1), so alpha is dropped here.
std::string ColorToHex(const css::Color& color)
{
  char buffer[8];
  std::snprintf(buffer,
                sizeof(buffer),
                "#%02X%02X%02X",
                static_cast<unsigned>(color.r),
                static_cast<unsigned>(color.g),
                static_cast<unsigned>(color.b));
  return std::string(buffer);
}

std::string ElementColorHex(const style::StyleEngine& styles, const dom::Element& element)
{
  static constexpr css::Color kDefault{0, 0, 0, 255};
  return ColorToHex(styles.StyleFor(element).color.value_or(kDefault));
}

// The element's CSS paint value (fill or stroke) as markup, when the cascade
// sets one: `none`, a colour, or currentColor resolved against the element's
// own colour.  Empty when the declaration does not exist, so the document's
// own attribute (or the rasterizer default) stays in charge.
std::optional<std::string>
CssPaint(const style::StyleEngine& styles, const dom::Element& element, bool is_stroke)
{
  const style::ComputedStyle& computed = styles.StyleFor(element);
  if (is_stroke ? computed.stroke_none : computed.fill_none) {
    return std::string("none");
  }
  if (is_stroke ? computed.stroke_current : computed.fill_current) {
    return ElementColorHex(styles, element);
  }
  const std::optional<css::Color>& color = is_stroke ? computed.stroke : computed.fill;
  if (color.has_value()) {
    return ColorToHex(*color);
  }
  return std::nullopt;
}

void SerializeElement(std::string& out,
                      const dom::Element& element,
                      const style::StyleEngine& styles,
                      bool is_root,
                      std::string_view root_width,
                      std::string_view root_height)
{
  const std::string color = ElementColorHex(styles, element);
  const std::optional<std::string> css_fill = CssPaint(styles, element, /*is_stroke=*/false);
  const std::optional<std::string> css_stroke = CssPaint(styles, element, /*is_stroke=*/true);
  const std::string_view tag = element.tag_name();

  out.push_back('<');
  out.append(tag);
  bool has_xmlns = false;
  for (const dom::Attribute& attr : element.attributes()) {
    if (is_root && (attr.name == "width" || attr.name == "height")) {
      // The used size comes from layout (CSS width/height, shrink-to-fit);
      // the attributes alone would rasterize at the authored size.
      continue;
    }
    if ((attr.name == "fill" && css_fill.has_value()) ||
        (attr.name == "stroke" && css_stroke.has_value())) {
      continue; // the cascade wins over the document's own paint attribute
    }
    if (attr.name == "xmlns") {
      has_xmlns = true;
    }
    out.push_back(' ');
    out.append(attr.name);
    out.append("=\"");
    AppendEscaped(out, SubstituteCurrentColor(attr.value, color), /*attribute=*/true);
    out.push_back('"');
  }
  if (css_fill.has_value()) {
    out.append(" fill=\"");
    AppendEscaped(out, *css_fill, /*attribute=*/true);
    out.push_back('"');
  }
  if (css_stroke.has_value()) {
    out.append(" stroke=\"");
    AppendEscaped(out, *css_stroke, /*attribute=*/true);
    out.push_back('"');
  }
  if (is_root) {
    if (!has_xmlns) {
      out.append(" xmlns=\"http://www.w3.org/2000/svg\"");
    }
    out.append(" width=\"");
    out.append(root_width);
    out.append("\" height=\"");
    out.append(root_height);
    out.push_back('"');
  }
  out.push_back('>');

  for (const dom::Node* child : element.ChildNodes()) {
    switch (child->node_type()) {
    case dom::NodeType::kElement:
      SerializeElement(
          out, static_cast<const dom::Element&>(*child), styles, /*is_root=*/false, {}, {});
      break;
    case dom::NodeType::kText: {
      const std::string& text = static_cast<const dom::Text&>(*child).data();
      AppendEscaped(out, text, /*attribute=*/false);
      break;
    }
    default:
      break; // comments and doctypes carry nothing the rasterizer needs
    }
  }

  out.append("</");
  out.append(tag);
  out.push_back('>');
}

} // namespace

std::string SerializeInlineSvg(const dom::Element& element,
                               const style::StyleEngine& styles,
                               float width,
                               float height)
{
  char buffer_w[32];
  char buffer_h[32];
  std::snprintf(buffer_w, sizeof(buffer_w), "%g", static_cast<double>(std::max(1.0f, width)));
  std::snprintf(buffer_h, sizeof(buffer_h), "%g", static_cast<double>(std::max(1.0f, height)));
  std::string out;
  out.reserve(512);
  SerializeElement(out, element, styles, /*is_root=*/true, buffer_w, buffer_h);
  return out;
}

} // namespace neko::renderer
