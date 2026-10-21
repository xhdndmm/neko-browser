#include "neko/dom/element.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace neko::dom {

namespace {

struct DecodedCodePoint
{
  std::uint32_t value = 0;
  std::size_t bytes = 1;
};

// Decodes one UTF-8 scalar value.  Invalid sequences (truncated, overlong,
// surrogate, out of range) are reported as a single byte so offset arithmetic
// always advances and never splits a valid multi-byte code point.
DecodedCodePoint DecodeCodePoint(std::string_view text, std::size_t offset)
{
  const auto byte_at = [text](std::size_t index) {
    return static_cast<unsigned char>(text[index]);
  };
  const unsigned char lead = byte_at(offset);
  if (lead < 0x80U) {
    return {lead, 1};
  }
  std::size_t length = 0;
  std::uint32_t value = 0;
  if ((lead & 0xE0U) == 0xC0U) {
    length = 2;
    value = lead & 0x1FU;
  } else if ((lead & 0xF0U) == 0xE0U) {
    length = 3;
    value = lead & 0x0FU;
  } else if ((lead & 0xF8U) == 0xF0U) {
    length = 4;
    value = lead & 0x07U;
  } else {
    return {lead, 1};
  }
  if (offset + length > text.size()) {
    return {lead, 1};
  }
  for (std::size_t i = 1; i < length; ++i) {
    const unsigned char continuation = byte_at(offset + i);
    if ((continuation & 0xC0U) != 0x80U) {
      return {lead, 1};
    }
    value = (value << 6U) | (continuation & 0x3FU);
  }
  const bool overlong = (length == 2 && value < 0x80U) || (length == 3 && value < 0x800U) ||
                        (length == 4 && value < 0x10000U);
  const bool surrogate = value >= 0xD800U && value <= 0xDFFFU;
  if (overlong || surrogate || value > 0x10FFFFU) {
    return {lead, 1};
  }
  return {value, length};
}

std::size_t Utf16Units(const DecodedCodePoint& code_point)
{
  return code_point.value >= 0x10000U ? 2 : 1;
}

std::string AsciiLower(std::string_view text)
{
  std::string lower;
  lower.reserve(text.size());
  for (const char c : text) {
    lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return lower;
}

} // namespace

Element::Element(std::string tag_name, std::string namespace_uri)
    : Node(NodeType::kElement), tag_name_(std::move(tag_name)),
      namespace_uri_(std::move(namespace_uri))
{}

std::size_t Utf16Length(std::string_view utf8)
{
  std::size_t units = 0;
  std::size_t offset = 0;
  while (offset < utf8.size()) {
    const DecodedCodePoint code_point = DecodeCodePoint(utf8, offset);
    units += Utf16Units(code_point);
    offset += code_point.bytes;
  }
  return units;
}

std::size_t Utf8OffsetForUtf16Offset(std::string_view utf8, std::size_t utf16_offset)
{
  std::size_t units = 0;
  std::size_t offset = 0;
  while (offset < utf8.size() && units < utf16_offset) {
    const DecodedCodePoint code_point = DecodeCodePoint(utf8, offset);
    if (units + Utf16Units(code_point) > utf16_offset) {
      break; // The offset falls inside a surrogate pair: use its start.
    }
    units += Utf16Units(code_point);
    offset += code_point.bytes;
  }
  return offset;
}

std::size_t PreviousUtf16Offset(std::string_view utf8, std::size_t utf16_offset)
{
  std::size_t units = 0;
  std::size_t offset = 0;
  std::size_t previous = 0;
  while (offset < utf8.size() && units < utf16_offset) {
    previous = units;
    const DecodedCodePoint code_point = DecodeCodePoint(utf8, offset);
    units += Utf16Units(code_point);
    offset += code_point.bytes;
    if (units >= utf16_offset) {
      return previous;
    }
  }
  return previous;
}

std::size_t NextUtf16Offset(std::string_view utf8, std::size_t utf16_offset)
{
  std::size_t units = 0;
  std::size_t offset = 0;
  while (offset < utf8.size()) {
    const DecodedCodePoint code_point = DecodeCodePoint(utf8, offset);
    const std::size_t next = units + Utf16Units(code_point);
    if (next > utf16_offset) {
      return next;
    }
    units = next;
    offset += code_point.bytes;
  }
  return units;
}

std::string ReplaceUtf16Range(std::string_view value,
                              std::size_t start,
                              std::size_t end,
                              std::string_view replacement)
{
  const std::size_t start_byte = Utf8OffsetForUtf16Offset(value, start);
  const std::size_t end_byte = Utf8OffsetForUtf16Offset(value, end);
  std::string result;
  result.reserve(value.size() + replacement.size());
  result.append(value.substr(0, start_byte));
  result.append(replacement);
  if (end_byte < value.size()) {
    result.append(value.substr(end_byte));
  }
  return result;
}

bool SupportsTextControlSelection(const Element& element)
{
  const std::string_view tag = element.tag_name();
  if (tag == "textarea") {
    return true;
  }
  if (tag != "input") {
    return false;
  }
  const std::string type = AsciiLower(element.GetAttribute("type").value_or("text"));
  if (type == "text" || type == "search" || type == "url" || type == "tel" || type == "email" ||
      type == "password") {
    return true;
  }
  // HTML's invalid value default for the type attribute is the Text state, so
  // an empty or unknown type is a text control and supports selection.
  static constexpr std::string_view kNonTextTypes[] = {"hidden",
                                                       "date",
                                                       "month",
                                                       "week",
                                                       "time",
                                                       "datetime-local",
                                                       "number",
                                                       "range",
                                                       "color",
                                                       "checkbox",
                                                       "radio",
                                                       "file",
                                                       "submit",
                                                       "image",
                                                       "reset",
                                                       "button"};
  for (const std::string_view non_text : kNonTextTypes) {
    if (type == non_text) {
      return false;
    }
  }
  return true;
}

std::string TextControlValue(const Element& element)
{
  if (element.tag_name() == "textarea") {
    return element.TextContent();
  }
  return std::string(element.GetAttribute("value").value_or(""));
}

HTMLTemplateElement::HTMLTemplateElement()
    : Element("template"), content_(std::make_unique<DocumentFragment>())
{}

std::string HTMLTemplateElement::ToString() const
{
  std::string out = SerializeOpenTag(*this);
  for (const dom::Node* child : content_->ChildNodes()) {
    out += child->ToString();
  }
  out += "</";
  out += tag_name();
  out += ">";
  return out;
}

bool Element::HasAttribute(std::string_view name) const
{
  return std::any_of(
      attributes_.begin(), attributes_.end(), [&](const Attribute& a) { return a.name == name; });
}

std::optional<std::string_view> Element::GetAttribute(std::string_view name) const
{
  for (const Attribute& a : attributes_) {
    if (a.name == name) {
      return std::string_view(a.value);
    }
  }
  return std::nullopt;
}

void Element::SetAttribute(std::string_view name, std::string_view value)
{
  for (Attribute& a : attributes_) {
    if (a.name == name) {
      a.value = std::string(value);
      return;
    }
  }
  attributes_.push_back(Attribute{std::string(name), std::string(value)});
}

void Element::RemoveAttribute(std::string_view name)
{
  const auto it = std::remove_if(
      attributes_.begin(), attributes_.end(), [&](const Attribute& a) { return a.name == name; });
  attributes_.erase(it, attributes_.end());
}

std::optional<std::string_view> Element::Id() const
{
  return GetAttribute("id");
}

std::vector<std::string_view> Element::ClassList() const
{
  std::vector<std::string_view> classes;
  const std::optional<std::string_view> attr = GetAttribute("class");
  if (!attr.has_value()) {
    return classes;
  }
  std::string_view text = attr.value();
  for (;;) {
    const std::size_t first = text.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string_view::npos) {
      break;
    }
    text.remove_prefix(first);
    const std::size_t end = text.find_first_of(" \t\r\n\f\v");
    if (end == std::string_view::npos) {
      classes.push_back(text);
      break;
    }
    classes.push_back(text.substr(0, end));
    text.remove_prefix(end);
  }
  return classes;
}

std::string Element::ToString() const
{
  std::string out = SerializeOpenTag(*this);
  out += Node::ToString();
  out += "</";
  out += tag_name_;
  out += ">";
  return out;
}

std::string SerializeOpenTag(const Element& element)
{
  std::string out = "<";
  out += element.tag_name();
  for (const Attribute& attr : element.attributes()) {
    out += ' ';
    out += attr.name;
    out += "=\"";
    for (const char c : attr.value) {
      if (c == '&') {
        out += "&amp;";
      } else if (c == '"') {
        out += "&quot;";
      } else if (c == '<') {
        out += "&lt;";
      } else {
        out.push_back(c);
      }
    }
    out += "\"";
  }
  out += ">";
  return out;
}

} // namespace neko::dom
