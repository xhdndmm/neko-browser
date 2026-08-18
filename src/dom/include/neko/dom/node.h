#pragma once

#include "neko/base/macros.h"

#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neko::dom {

class Node;

enum class NodeType
{
  kDocument,
  kElement,
  kText,
  kComment,
  kDocumentFragment
};

// Lightweight view over a node's children, exposing raw Node* pointers.
class ChildList
{
public:
  explicit ChildList(const std::vector<std::unique_ptr<Node>>& children) : children_(children) {}

  class Iterator
  {
  public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = Node*;
    using difference_type = std::ptrdiff_t;
    using pointer = Node*;
    using reference = Node*;

    explicit Iterator(std::vector<std::unique_ptr<Node>>::const_iterator it) : it_(it) {}

    Node* operator*() const
    {
      return it_->get();
    }
    Iterator& operator++()
    {
      ++it_;
      return *this;
    }
    Iterator operator++(int)
    {
      Iterator tmp = *this;
      ++it_;
      return tmp;
    }
    bool operator==(const Iterator& other) const
    {
      return it_ == other.it_;
    }
    bool operator!=(const Iterator& other) const
    {
      return it_ != other.it_;
    }

  private:
    std::vector<std::unique_ptr<Node>>::const_iterator it_;
  };

  Iterator begin() const
  {
    return Iterator(children_.begin());
  }
  Iterator end() const
  {
    return Iterator(children_.end());
  }

private:
  const std::vector<std::unique_ptr<Node>>& children_;
};

// Base class for all DOM nodes.  Ownership is strictly hierarchical: a parent
// owns its children; children reference their parent non-owningly.
class Node
{
public:
  virtual ~Node();

  NEKO_DISALLOW_COPY_AND_MOVE(Node)

  NodeType node_type() const
  {
    return node_type_;
  }
  Node* parent() const
  {
    return parent_;
  }

  Node* first_child() const;
  Node* last_child() const;
  std::size_t child_count() const
  {
    return children_.size();
  }
  Node* child_at(std::size_t index) const;
  ChildList ChildNodes() const
  {
    return ChildList(children_);
  }

  // The maximum number of parent links from a node to the document root.
  // Keeping this bound at the DOM mutation boundary protects recursive DOM,
  // style, layout, and paint traversals from script-created deep trees.
  static constexpr std::size_t kMaximumTreeDepth = 512;

  // Returns whether |child|'s subtree would exceed the tree-depth budget when
  // inserted here. This does not require |child| to be detached, so callers
  // can validate a move before releasing its current owner.
  bool WouldExceedMaximumTreeDepth(const Node& child) const;

  // Returns whether a detached |child| can be inserted without violating this
  // tree's ownership, cycle, or depth invariants.
  bool CanAcceptChild(const Node& child) const;

  // Tree mutation. The parent takes ownership of the inserted node. Returns
  // false without modifying this tree for null, already-attached, or cyclic
  // nodes; InsertBefore also rejects a non-null reference outside this parent.
  // Inserting a DocumentFragment inserts its children instead (DOM spec
  // "insert" algorithm).
  bool AppendChild(std::unique_ptr<Node> child);
  bool InsertBefore(std::unique_ptr<Node> child, Node* reference);
  std::unique_ptr<Node> RemoveChild(Node* child);

  // The DOM "node value" (nodeValue): the data of a Text or Comment node,
  // null for all other node types.  Returns nullptr when there is no value.
  virtual const std::string* NodeValue() const;

  // Human-readable name ("div", "#text", ...).
  virtual std::string_view node_name() const = 0;

  // Concatenated text of this node and all descendants.
  virtual std::string TextContent() const;

  // DOM serialization (used by --dump-dom and tests).
  virtual std::string ToString() const;

protected:
  explicit Node(NodeType type);
  void SetParent(Node* parent)
  {
    parent_ = parent;
  }

 private:
  NodeType node_type_;
  Node* parent_ = nullptr;
  std::vector<std::unique_ptr<Node>> children_;
};

} // namespace neko::dom
