#include "neko/dom/node.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "neko/dom/element.h"

namespace neko::dom {
namespace {

// Escapes text content for serialization.
std::string EscapeText(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

void FindFirstTitle(const Node* node, const Element** out) {
  if (*out != nullptr) {
    return;
  }
  for (Node* child : node->ChildNodes()) {
    if (child->node_type() != NodeType::kElement) {
      continue;
    }
    const Element* element = static_cast<const Element*>(child);
    if (element->tag_name() == "title") {
      *out = element;
      return;
    }
    FindFirstTitle(element, out);
  }
}

}  // namespace

Node::~Node() = default;

Node::Node(NodeType type) : node_type_(type) {}

Node* Node::first_child() const { return children_.empty() ? nullptr : children_.front().get(); }

Node* Node::last_child() const { return children_.empty() ? nullptr : children_.back().get(); }

Node* Node::child_at(std::size_t index) const {
  return index < children_.size() ? children_[index].get() : nullptr;
}

void Node::AppendChild(std::unique_ptr<Node> child) {
  child->SetParent(this);
  children_.push_back(std::move(child));
}

void Node::InsertBefore(std::unique_ptr<Node> child, Node* reference) {
  const auto it = std::find_if(children_.begin(), children_.end(),
                               [&](const std::unique_ptr<Node>& c) { return c.get() == reference; });
  if (it == children_.end()) {
    // Reference not found: append at the end (WHATWG: reference is nullptr).
    child->SetParent(this);
    children_.push_back(std::move(child));
    return;
  }
  child->SetParent(this);
  children_.insert(it, std::move(child));
}

std::unique_ptr<Node> Node::RemoveChild(Node* child) {
  for (auto it = children_.begin(); it != children_.end(); ++it) {
    if (it->get() == child) {
      std::unique_ptr<Node> removed = std::move(*it);
      children_.erase(it);
      removed->SetParent(nullptr);
      return removed;
    }
  }
  return nullptr;
}

std::string Node::TextContent() const {
  std::string out;
  for (Node* child : ChildNodes()) {
    out += child->TextContent();
  }
  return out;
}

std::string Node::ToString() const {
  std::string out;
  for (Node* child : ChildNodes()) {
    out += child->ToString();
  }
  return out;
}

// ---- Leaf nodes -------------------------------------------------------------

Text::Text(std::string data) : Node(NodeType::kText), data_(std::move(data)) {}

std::string Text::ToString() const { return EscapeText(data_); }

Comment::Comment(std::string data) : Node(NodeType::kComment), data_(std::move(data)) {}

std::string Comment::ToString() const { return "<!--" + data_ + "-->"; }

DocumentFragment::DocumentFragment() : Node(NodeType::kDocumentFragment) {}

Document::Document() : Node(NodeType::kDocument) {}

Element* Document::document_element() const {
  for (Node* child : ChildNodes()) {
    if (child->node_type() == NodeType::kElement) {
      return static_cast<Element*>(child);
    }
  }
  return nullptr;
}

std::string Document::Title() const {
  const Element* title = nullptr;
  FindFirstTitle(this, &title);
  return title != nullptr ? title->TextContent() : std::string();
}

std::string Document::ToString() const { return Node::ToString(); }

}  // namespace neko::dom
