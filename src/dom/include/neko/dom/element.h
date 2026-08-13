#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "neko/dom/node.h"

namespace neko::dom {

struct Attribute {
  std::string name;
  std::string value;
};

// A DOM element with a tag name and an ordered attribute list.
// Attribute names are lowercased by the HTML parser (HTML semantics).
class Element : public Node {
 public:
  explicit Element(std::string tag_name);

  std::string_view tag_name() const { return tag_name_; }
  std::string_view node_name() const override { return tag_name_; }

  bool HasAttribute(std::string_view name) const;
  std::optional<std::string_view> GetAttribute(std::string_view name) const;
  void SetAttribute(std::string_view name, std::string_view value);
  void RemoveAttribute(std::string_view name);
  const std::vector<Attribute>& attributes() const { return attributes_; }

  // Convenience accessors for the id and class attributes.
  std::optional<std::string_view> Id() const;
  std::vector<std::string_view> ClassList() const;

  std::string ToString() const override;

 private:
  std::string tag_name_;
  std::vector<Attribute> attributes_;
};

// Leaf node holding character data.
class Text : public Node {
 public:
  explicit Text(std::string data);

  const std::string& data() const { return data_; }
  // Appends text (used by the HTML parser to merge adjacent character runs).
  void AppendData(std::string_view text) { data_.append(text); }
  std::string_view node_name() const override { return "#text"; }
  std::string TextContent() const override { return data_; }
  std::string ToString() const override;

 private:
  std::string data_;
};

// Leaf node holding comment data.
class Comment : public Node {
 public:
  explicit Comment(std::string data);

  const std::string& data() const { return data_; }
  std::string_view node_name() const override { return "#comment"; }
  std::string TextContent() const override { return {}; }
  std::string ToString() const override;

 private:
  std::string data_;
};

// Container without document semantics (used by the HTML parser).
class DocumentFragment : public Node {
 public:
  DocumentFragment();

  std::string_view node_name() const override { return "#document-fragment"; }
};
class Document : public Node {
 public:
  Document();

  std::string_view node_name() const override { return "#document"; }

  // First child element, or nullptr.
  Element* document_element() const;

  // Content of the first <title> element, or empty.
  std::string Title() const;

  std::string ToString() const override;
};

// Serializes an element's opening tag, e.g. <div class="x">.
std::string SerializeOpenTag(const Element& element);

}  // namespace neko::dom
