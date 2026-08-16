#include "neko/dom/element.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace neko::dom {

Element::Element(std::string tag_name) : Node(NodeType::kElement), tag_name_(std::move(tag_name)) {}

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
