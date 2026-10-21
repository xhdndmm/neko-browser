#pragma once

#include "neko/dom/node.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neko::dom {

class HTMLTemplateElement;
class Element;

struct Attribute
{
  std::string name;
  std::string value;
};

// Direction of a text control selection (WHATWG HTML §4.10.20).  "none" means
// no particular end of the selection owns the caret yet (the platform
// default); "backward" means the caret sits at the start of the selection.
enum class SelectionDirection
{
  kNone,
  kForward,
  kBackward,
};

// Per-element state for the text control selection APIs (WHATWG HTML
// §4.10.20).  Both the browser's keyboard editing and the JavaScript bindings
// read and write this one object; there is no second copy in either layer.
//
// Offsets are UTF-16 code unit offsets into the control's relevant value (the
// `value` content attribute for <input>, the text content for <textarea>),
// because the HTML API is defined that way and a surrogate pair counts as two
// code units.
struct TextControlState
{
  std::size_t selection_start = 0;
  std::size_t selection_end = 0;
  SelectionDirection selection_direction = SelectionDirection::kNone;
  // HTML's "dirty value flag": true once the value was changed by the user or
  // by script.  The engine keeps a single value store (the input `value`
  // content attribute / the textarea text child), so this is bookkeeping for
  // the maxlength/minlength distinction, not a second value.
  bool dirty_value = false;
};

// --- UTF-16 offset helpers over UTF-8 text --------------------------------
// The HTML selection API is defined in UTF-16 code units while the engine
// stores UTF-8.  These helpers are shared by the DOM, JavaScript and browser
// layers; malformed UTF-8 bytes count as one code unit each so the functions
// never split a valid scalar value and always terminate.

std::size_t Utf16Length(std::string_view utf8);
// Byte offset of the code unit boundary at |utf16_offset| (clamped; an offset
// inside a surrogate pair maps to the start of that pair).
std::size_t Utf8OffsetForUtf16Offset(std::string_view utf8, std::size_t utf16_offset);
// Boundary of the code point before / after |utf16_offset| (clamped).
std::size_t PreviousUtf16Offset(std::string_view utf8, std::size_t utf16_offset);
std::size_t NextUtf16Offset(std::string_view utf8, std::size_t utf16_offset);
// Replaces the UTF-16 range [start, end) of |value| with |replacement|.
std::string ReplaceUtf16Range(std::string_view value,
                              std::size_t start,
                              std::size_t end,
                              std::string_view replacement);

// True when the element is a text control to which the selection APIs apply:
// <textarea>, or <input> in the Text, Search, URL, Telephone, Email or
// Password state (WHATWG HTML §4.10.20's "do not apply" list excludes number,
// date/time, checkbox, radio, file, range, color, hidden, submit, reset,
// button and image).  A missing or unknown type value is the Text state.
bool SupportsTextControlSelection(const Element& element);

// The control's relevant value: the `value` content attribute for <input>,
// the text content for <textarea>.  This is the one value store the browser
// keyboard layer and the JavaScript bindings edit.
std::string TextControlValue(const Element& element);

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

  // Text control selection state (WHATWG HTML §4.10.20), allocated on first
  // use.  The browser keyboard layer and the JavaScript bindings both use
  // this object, so a selection made by script is honoured by the next key
  // press and vice versa.
  TextControlState& EnsureTextControlState()
  {
    if (text_control_state_ == nullptr) {
      text_control_state_ = std::make_unique<TextControlState>();
    }
    return *text_control_state_;
  }
  TextControlState* text_control_state()
  {
    return text_control_state_.get();
  }
  const TextControlState* text_control_state() const
  {
    return text_control_state_.get();
  }

  std::string ToString() const override;

private:
  std::string tag_name_;
  std::string namespace_uri_;
  std::vector<Attribute> attributes_;
  std::unique_ptr<TextControlState> text_control_state_;
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

// A DOCTYPE node (WHATWG DOM §4.3.1 DocumentType): name/publicId/systemId.
// The parser inserts one before the document element; it never has children.
// Per spec its nodeName is its name (e.g. "html") and its textContent is
// the empty string.
class DocumentType : public Node
{
public:
  DocumentType(std::string name, std::string public_id, std::string system_id);

  const std::string& name() const
  {
    return name_;
  }
  const std::string& public_id() const
  {
    return public_id_;
  }
  const std::string& system_id() const
  {
    return system_id_;
  }
  std::string_view node_name() const override
  {
    return name_;
  }
  std::string ToString() const override;

private:
  std::string name_;
  std::string public_id_;
  std::string system_id_;
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
