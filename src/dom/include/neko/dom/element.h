#pragma once

#include "neko/dom/node.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neko::dom {

class HTMLTemplateElement;

struct Attribute
{
  std::string name;
  std::string value;
};

// A DOM element with a tag name and an ordered attribute list.
// Attribute names are lowercased by the HTML parser (HTML semantics).
class Element : public Node
{
public:
  explicit Element(std::string tag_name,
                   std::string namespace_uri = "http://www.w3.org/1999/xhtml");

  std::string_view tag_name() const
  {
    return tag_name_;
  }
  std::string_view node_name() const override
  {
    return tag_name_;
  }
  std::string_view namespace_uri() const
  {
    return namespace_uri_;
  }

  bool HasAttribute(std::string_view name) const;
  std::optional<std::string_view> GetAttribute(std::string_view name) const;
  void SetAttribute(std::string_view name, std::string_view value);
  void RemoveAttribute(std::string_view name);
  const std::vector<Attribute>& attributes() const
  {
    return attributes_;
  }

  // Convenience accessors for the id and class attributes.
  std::optional<std::string_view> Id() const;
  std::vector<std::string_view> ClassList() const;

  // Returns this element as an HTMLTemplateElement when it is one (nullptr
  // otherwise).  The parser's "appropriate place for inserting a node" rule
  // (WHATWG HTML 13.2.5.3) routes children of <template> into its contents
  // fragment through this accessor; no RTTI is involved.
  virtual HTMLTemplateElement* AsTemplate()
  {
    return nullptr;
  }
  virtual const HTMLTemplateElement* AsTemplate() const
  {
    return nullptr;
  }

  std::string ToString() const override;

private:
  std::string tag_name_;
  std::string namespace_uri_;
  std::vector<Attribute> attributes_;
};

// Leaf node holding character data.
class Text : public Node
{
public:
  explicit Text(std::string data);

  const std::string& data() const
  {
    return data_;
  }
  // Appends text (used by the HTML parser to merge adjacent character runs).
  void AppendData(std::string_view text)
  {
    data_.append(text);
  }
  // Replaces the data (the DOM CharacterData.data / Node.nodeValue setter).
  void SetData(std::string_view text)
  {
    data_.assign(text);
  }
  std::string_view node_name() const override
  {
    return "#text";
  }
  std::string TextContent() const override
  {
    return data_;
  }
  const std::string* NodeValue() const override
  {
    return &data_;
  }
  std::string ToString() const override;

private:
  std::string data_;
};

// Leaf node holding comment data.
class Comment : public Node
{
public:
  explicit Comment(std::string data);

  const std::string& data() const
  {
    return data_;
  }
  std::string_view node_name() const override
  {
    return "#comment";
  }
  // Per DOM spec the textContent of a Comment is its data.
  std::string TextContent() const override
  {
    return data_;
  }
  const std::string* NodeValue() const override
  {
    return &data_;
  }
  void SetData(std::string_view text)
  {
    data_.assign(text);
  }
  std::string ToString() const override;

private:
  std::string data_;
};

// Container without document semantics (used by the HTML parser).
class DocumentFragment : public Node
{
public:
  DocumentFragment();

  std::string_view node_name() const override
  {
    return "#document-fragment";
  }
};

// HTMLTemplateElement (WHATWG HTML 4.12.3): the element's parsed children and
// innerHTML live in a separate "template contents" DocumentFragment exposed as
// `content`.  Scripts build fragments via createElement('template') +
// innerHTML and clone template.content; Vue's runtime DOM does exactly that,
// and bilibili's hydration crashed with "cannot read property 'firstChild' of
// undefined" while `content` was missing entirely.
//
// The DOM appendChild on a template element still targets the element itself
// (browser behavior); only parsed children and innerHTML use the contents.
class HTMLTemplateElement : public Element
{
public:
  HTMLTemplateElement();

  HTMLTemplateElement* AsTemplate() override
  {
    return this;
  }
  const HTMLTemplateElement* AsTemplate() const override
  {
    return this;
  }

  DocumentFragment* content()
  {
    return content_.get();
  }
  const DocumentFragment* content() const
  {
    return content_.get();
  }

  // Serializes as <template> with the contents as its markup (browser
  // behavior: outerHTML includes the template contents).
  std::string ToString() const override;

private:
  std::unique_ptr<DocumentFragment> content_;
};
class Document : public Node
{
public:
  Document();

  std::string_view node_name() const override
  {
    return "#document";
  }

  // First child element, or nullptr.
  Element* document_element() const;

  // Content of the first <title> element, or empty.
  std::string Title() const;

  std::string ToString() const override;
};

// Serializes an element's opening tag, e.g. <div class="x">.
std::string SerializeOpenTag(const Element& element);

} // namespace neko::dom
