// neko::javascript DOM bindings — Node/CharacterData/NodeList family.
//
// Part of the dom_binding split (see binding/binding_internal.h): node tree
// mutation/query callbacks, the CharacterData accessors shared by Text and
// Comment, the NodeList item/length helpers, and DefineNodePrototype.

#include "neko/dom/element.h"
#include "neko/dom/node.h"

#include "binding_internal.h"

#include <quickjs.h>
#include <string>
#include <vector>

namespace neko::javascript {

JSValue NodeAppendChild(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* parent = UnwrapNode(this_val);
  if (impl == nullptr || parent == nullptr) {
    return JS_ThrowTypeError(ctx, "appendChild: detached node");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "appendChild requires one argument");
  }
  dom::Node* child = UnwrapNode(argv[0]);
  if (child == nullptr) {
    return JS_ThrowTypeError(ctx, "appendChild: argument is not a node");
  }
  if (child == parent) {
    return ThrowDomException(
        ctx, "HierarchyRequestError", "appendChild: cannot append a node to itself");
  }
  if (child->node_type() == dom::NodeType::kDocument) {
    return ThrowDomException(
        ctx, "HierarchyRequestError", "appendChild: cannot append a Document node");
  }
  if (IsAncestorOf(child, parent)) {
    return ThrowDomException(
        ctx, "HierarchyRequestError", "appendChild: cannot append an ancestor");
  }
  if (parent->WouldExceedMaximumTreeDepth(*child)) {
    return ThrowDomException(
        ctx, "HierarchyRequestError", "appendChild: maximum tree depth exceeded");
  }
  if (child->parent() != nullptr) {
    std::unique_ptr<dom::Node> removed = child->parent()->RemoveChild(child);
    impl->TakeOwnership(child, std::move(removed));
  }
  std::unique_ptr<dom::Node> owned = impl->ReleaseOwned(child);
  if (owned == nullptr) {
    return JS_ThrowTypeError(ctx, "appendChild: internal ownership error");
  }
  parent->AppendChild(std::move(owned));
  impl->RecordChildListMutation(parent, {child}, {});
  impl->MarkDomDirty();
  return impl->WrapNode(child);
}

// DOM Node.append(...children): appends each argument as the last child, in
// order.  Node arguments go through the same validated/adopting path as
// appendChild; string arguments are not converted to text nodes yet
// (documented limitation) and are skipped.
JSValue NodeAppend(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* parent = UnwrapNode(this_val);
  if (impl == nullptr || parent == nullptr) {
    return JS_ThrowTypeError(ctx, "append: detached node");
  }
  for (int i = 0; i < argc; ++i) {
    if (UnwrapNode(argv[i]) == nullptr) {
      continue; // non-node argument (string): not supported, skip
    }
    JSValue result = NodeAppendChild(ctx, this_val, 1, &argv[i]);
    if (JS_IsException(result)) {
      return result;
    }
    JS_FreeValue(ctx, result);
  }
  impl->MarkDomDirty();
  return JS_UNDEFINED;
}

// DOM Node.replaceChildren(...children): removes every current child, then
// appends the given children (none when called with no arguments).  Removed
// nodes stay alive in the binder's registry, so JS references remain valid.
JSValue NodeReplaceChildren(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "replaceChildren: detached node");
  }
  while (node->first_child() != nullptr) {
    dom::Node* child = node->first_child();
    std::unique_ptr<dom::Node> removed = node->RemoveChild(child);
    impl->TakeOwnership(child, std::move(removed));
  }
  if (argc > 0) {
    JSValue result = NodeAppend(ctx, this_val, argc, argv);
    if (JS_IsException(result)) {
      return result;
    }
    JS_FreeValue(ctx, result);
  }
  impl->MarkDomDirty();
  return JS_UNDEFINED;
}

JSValue NodeInsertBefore(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* parent = UnwrapNode(this_val);
  if (impl == nullptr || parent == nullptr) {
    return JS_ThrowTypeError(ctx, "insertBefore: detached node");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "insertBefore requires at least one argument");
  }
  dom::Node* child = UnwrapNode(argv[0]);
  if (child == nullptr) {
    return JS_ThrowTypeError(ctx, "insertBefore: argument is not a node");
  }
  dom::Node* reference = argc >= 2 ? UnwrapNode(argv[1]) : nullptr;
  if (reference != nullptr && reference->parent() != parent) {
    return ThrowDomException(
        ctx, "NotFoundError", "insertBefore: reference is not a child of this node");
  }
  if (child == parent || child->node_type() == dom::NodeType::kDocument) {
    return ThrowDomException(ctx, "HierarchyRequestError", "insertBefore: invalid node");
  }
  if (parent->WouldExceedMaximumTreeDepth(*child)) {
    return ThrowDomException(
        ctx, "HierarchyRequestError", "insertBefore: maximum tree depth exceeded");
  }
  if (child->parent() != nullptr) {
    std::unique_ptr<dom::Node> removed = child->parent()->RemoveChild(child);
    impl->TakeOwnership(child, std::move(removed));
  }
  std::unique_ptr<dom::Node> owned = impl->ReleaseOwned(child);
  if (owned == nullptr) {
    return JS_ThrowTypeError(ctx, "insertBefore: internal ownership error");
  }
  parent->InsertBefore(std::move(owned), reference);
  impl->RecordChildListMutation(parent, {child}, {});
  impl->MarkDomDirty();
  return impl->WrapNode(child);
}

JSValue NodeRemoveChild(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* parent = UnwrapNode(this_val);
  if (impl == nullptr || parent == nullptr) {
    return JS_ThrowTypeError(ctx, "removeChild: detached node");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "removeChild requires one argument");
  }
  dom::Node* child = UnwrapNode(argv[0]);
  if (child == nullptr || child->parent() != parent) {
    return ThrowDomException(
        ctx, "NotFoundError", "removeChild: argument is not a child of this node");
  }
  std::unique_ptr<dom::Node> removed = parent->RemoveChild(child);
  impl->TakeOwnership(child, std::move(removed));
  impl->RecordChildListMutation(parent, {}, {child});
  impl->MarkDomDirty();
  return impl->WrapNode(child);
}

JSValue
NodeHasChildNodes(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  dom::Node* node = UnwrapNode(this_val);
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "hasChildNodes: detached node");
  }
  return JS_NewBool(ctx, node->child_count() > 0);
}

JSValue NodeCloneNode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "cloneNode: detached node");
  }
  const int deep = argc >= 1 ? JS_ToBool(ctx, argv[0]) : 0;
  if (deep < 0) {
    return JS_EXCEPTION; // pending exception from JS_ToBool
  }
  std::unique_ptr<dom::Node> clone = CloneNodeImpl(*node, deep != 0);
  if (clone == nullptr) {
    return JS_ThrowTypeError(ctx, "cloneNode: this node type cannot be cloned");
  }
  dom::Node* raw = clone.get();
  impl->created[raw] = std::move(clone);
  return impl->WrapNode(raw);
}

JSValue NodeAddEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "addEventListener: requires a node");
  }
  if (argc < 2 || !JS_IsFunction(ctx, argv[1])) {
    return JS_ThrowTypeError(ctx, "addEventListener requires (type, callback)");
  }
  bool ok = false;
  const std::string type = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  // Options: a boolean (capture) or an object with capture/once.
  bool capture = false;
  bool once = false;
  if (argc >= 3 && !JS_IsUndefined(argv[2])) {
    if (JS_IsBool(argv[2])) {
      const int v = JS_ToBool(ctx, argv[2]);
      if (v < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_EXCEPTION;
      }
      capture = v != 0;
    } else if (JS_IsObject(argv[2])) {
      JSValue c = JS_GetPropertyStr(ctx, argv[2], "capture");
      if (JS_IsBool(c)) {
        const int v = JS_ToBool(ctx, c);
        if (v < 0) {
          JS_FreeValue(ctx, c);
          JS_FreeValue(ctx, JS_GetException(ctx));
          return JS_EXCEPTION;
        }
        capture = v != 0;
      }
      JS_FreeValue(ctx, c);
      JSValue o = JS_GetPropertyStr(ctx, argv[2], "once");
      if (JS_IsBool(o)) {
        const int v = JS_ToBool(ctx, o);
        if (v < 0) {
          JS_FreeValue(ctx, o);
          JS_FreeValue(ctx, JS_GetException(ctx));
          return JS_EXCEPTION;
        }
        once = v != 0;
      }
      JS_FreeValue(ctx, o);
    }
  }
  Impl::Listener listener;
  listener.callback = JS_DupValue(ctx, argv[1]);
  listener.capture = capture;
  listener.once = once;
  impl->listeners[node][type].push_back(std::move(listener));
  return JS_UNDEFINED;
}

JSValue NodeRemoveEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "removeEventListener: requires a node");
  }
  if (argc < 2) {
    return JS_UNDEFINED;
  }
  bool ok = false;
  const std::string type = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  // Removal matches the callback plus the capture flag (like browsers).
  bool capture = false;
  if (argc >= 3 && !JS_IsUndefined(argv[2])) {
    if (JS_IsBool(argv[2])) {
      const int v = JS_ToBool(ctx, argv[2]);
      if (v < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_EXCEPTION;
      }
      capture = v != 0;
    } else if (JS_IsObject(argv[2])) {
      JSValue c = JS_GetPropertyStr(ctx, argv[2], "capture");
      if (JS_IsBool(c)) {
        const int v = JS_ToBool(ctx, c);
        if (v < 0) {
          JS_FreeValue(ctx, c);
          JS_FreeValue(ctx, JS_GetException(ctx));
          return JS_EXCEPTION;
        }
        capture = v != 0;
      }
      JS_FreeValue(ctx, c);
    }
  }
  const auto el_it = impl->listeners.find(node);
  if (el_it == impl->listeners.end()) {
    return JS_UNDEFINED;
  }
  auto type_it = el_it->second.find(type);
  if (type_it == el_it->second.end()) {
    return JS_UNDEFINED;
  }
  std::vector<Impl::Listener>& listeners = type_it->second;
  for (auto it = listeners.begin(); it != listeners.end(); ++it) {
    if (it->capture == capture && JS_IsStrictEqual(ctx, it->callback, argv[1])) {
      JS_FreeValue(ctx, it->callback);
      listeners.erase(it);
      break;
    }
  }
  if (listeners.empty()) {
    el_it->second.erase(type_it);
  }
  return JS_UNDEFINED;
}

JSValue NodeDispatchEvent(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "dispatchEvent: requires a node");
  }
  if (argc < 1 || !JS_IsObject(argv[0])) {
    return JS_ThrowTypeError(ctx, "dispatchEvent requires an event object");
  }
  // The caller may pass a real Event object (new Event(...)) or a plain
  // {type: ...} object (legacy shortcut).  Plain objects are dispatched as a
  // non-bubbling, non-cancelable event; real events keep their options.
  JSValue event = JS_UNDEFINED;
  if (IsEvent(argv[0])) {
    event = JS_DupValue(ctx, argv[0]);
  } else {
    std::string type;
    if (!EventTypeOf(ctx, argv[0], &type)) {
      return JS_EXCEPTION;
    }
    event = impl->MakeEvent(std::move(type), /*bubbles=*/false, /*cancelable=*/false);
  }
  const bool not_canceled = impl->DispatchPropagated(node, event);
  JS_FreeValue(ctx, event);
  return JS_NewBool(ctx, not_canceled);
}

JSValue NodeGetNodeType(JSContext* ctx, JSValueConst this_val)
{
  dom::Node* node = UnwrapNode(this_val);
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  return JS_NewInt32(ctx, NodeTypeNumber(node->node_type()));
}

JSValue NodeGetNodeName(JSContext* ctx, JSValueConst this_val)
{
  dom::Node* node = UnwrapNode(this_val);
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  const std::string name = NodeNameOf(*node);
  return JS_NewStringLen(ctx, name.data(), name.size());
}

JSValue NodeGetTextContent(JSContext* ctx, JSValueConst this_val)
{
  dom::Node* node = UnwrapNode(this_val);
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  const std::string text = node->TextContent();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

JSValue NodeSetTextContent(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  // Per DOM spec, setting textContent on a Text or Comment node replaces its
  // data; it must not create child nodes (which a Text/Comment cannot have).
  if (node->node_type() == dom::NodeType::kText) {
    static_cast<dom::Text*>(node)->SetData(text);
    impl->MarkDomDirty();
    return JS_UNDEFINED;
  }
  if (node->node_type() == dom::NodeType::kComment) {
    static_cast<dom::Comment*>(node)->SetData(text);
    impl->MarkDomDirty();
    return JS_UNDEFINED;
  }
  while (node->first_child() != nullptr) {
    std::unique_ptr<dom::Node> removed = node->RemoveChild(node->first_child());
    impl->TakeOwnership(removed.get(), std::move(removed));
  }
  if (!text.empty()) {
    node->AppendChild(std::make_unique<dom::Text>(text));
  }
  impl->MarkDomDirty();
  return JS_UNDEFINED;
}

JSValue NodeGetNodeValue(JSContext* ctx, JSValueConst this_val)
{
  dom::Node* node = UnwrapNode(this_val);
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  const std::string* value = node->NodeValue();
  if (value == nullptr) {
    return JS_NULL;
  }
  return JS_NewStringLen(ctx, value->data(), value->size());
}

JSValue NodeSetNodeValue(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Node* node = UnwrapNode(this_val);
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (node->node_type() == dom::NodeType::kText) {
    static_cast<dom::Text*>(node)->SetData(text);
  } else if (node->node_type() == dom::NodeType::kComment) {
    static_cast<dom::Comment*>(node)->SetData(text);
  }
  return JS_UNDEFINED;
}

// CharacterData.data getter/setter (DOM spec §4.7): only Text and Comment
// carry data; on other nodes the accessor is absent, so these are only wired
// to the text and comment prototypes.
JSValue CharacterDataGetData(JSContext* ctx, JSValueConst this_val)
{
  dom::Node* node = UnwrapNode(this_val);
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  const std::string* value = node->NodeValue();
  if (value == nullptr) {
    return JS_UNDEFINED;
  }
  return JS_NewStringLen(ctx, value->data(), value->size());
}

JSValue CharacterDataSetData(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Node* node = UnwrapNode(this_val);
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (node->node_type() == dom::NodeType::kText) {
    static_cast<dom::Text*>(node)->SetData(text);
  } else if (node->node_type() == dom::NodeType::kComment) {
    static_cast<dom::Comment*>(node)->SetData(text);
  }
  return JS_UNDEFINED;
}

JSValue NodeGetParentNode(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  return impl->WrapNode(node->parent());
}

JSValue NodeGetFirstChild(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  return impl->WrapNode(node->first_child());
}

JSValue NodeGetLastChild(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  return impl->WrapNode(node->last_child());
}

JSValue NodeGetChildNodes(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  std::vector<dom::Node*> nodes;
  nodes.reserve(node->child_count());
  for (dom::Node* child : node->ChildNodes()) {
    nodes.push_back(child);
  }
  return impl->MakeNodeArray(nodes);
}

// Returns the sibling at |offset| (+1 next, -1 previous), or nullptr.
dom::Node* SiblingOf(dom::Node* node, int offset)
{
  if (node == nullptr || node->parent() == nullptr) {
    return nullptr;
  }
  dom::Node* parent = node->parent();
  for (dom::Node* child : parent->ChildNodes()) {
    if (child == node) {
      // Walk from the located child.
      dom::Node* current = node;
      while (offset > 0) {
        bool found = false;
        for (dom::Node* n : parent->ChildNodes()) {
          if (found) {
            return n;
          }
          if (n == current) {
            found = true;
          }
        }
        return nullptr; // no more siblings
      }
      while (offset < 0) {
        dom::Node* prev = nullptr;
        for (dom::Node* n : parent->ChildNodes()) {
          if (n == current) {
            return prev;
          }
          prev = n;
        }
        return nullptr;
      }
      return current;
    }
  }
  return nullptr;
}

JSValue NodeGetNextSibling(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  return impl->WrapNode(SiblingOf(node, +1));
}

JSValue NodeGetPreviousSibling(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  return impl->WrapNode(SiblingOf(node, -1));
}

JSValue NodeGetParentElement(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  return impl->WrapNode(AsElement(node->parent()));
}

JSValue NodeGetOwnerDocument(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  // Per spec, Document.ownerDocument is null.
  if (node->node_type() == dom::NodeType::kDocument) {
    return JS_NULL;
  }
  return impl->WrapNode(&impl->document);
}

JSValue NodeGetIsConnected(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  bool connected = false;
  for (dom::Node* p = node; p != nullptr; p = p->parent()) {
    if (p == &impl->document) {
      connected = true;
      break;
    }
  }
  return JS_NewBool(ctx, connected);
}

JSValue NodeContains(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Node* node = UnwrapNode(this_val);
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "contains: detached node");
  }
  if (argc < 1) {
    return JS_NewBool(ctx, false);
  }
  dom::Node* other = UnwrapNode(argv[0]);
  if (other == nullptr) {
    return JS_NewBool(ctx, false);
  }
  if (other == node) {
    return JS_NewBool(ctx, true);
  }
  return JS_NewBool(ctx, IsAncestorOf(node, other));
}

JSValue NodeReplaceChild(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* parent = UnwrapNode(this_val);
  if (impl == nullptr || parent == nullptr) {
    return JS_ThrowTypeError(ctx, "replaceChild: detached node");
  }
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "replaceChild requires (newChild, oldChild)");
  }
  dom::Node* new_child = UnwrapNode(argv[0]);
  dom::Node* old_child = UnwrapNode(argv[1]);
  if (new_child == nullptr || old_child == nullptr) {
    return JS_ThrowTypeError(ctx, "replaceChild: arguments are not nodes");
  }
  if (old_child->parent() != parent) {
    return ThrowDomException(
        ctx, "NotFoundError", "replaceChild: oldChild is not a child of this node");
  }
  if (new_child == parent || new_child->node_type() == dom::NodeType::kDocument ||
      IsAncestorOf(new_child, parent)) {
    return ThrowDomException(ctx, "HierarchyRequestError", "replaceChild: invalid newChild");
  }
  // Insert the new child before the old one, then remove the old one.
  if (new_child->parent() != nullptr) {
    std::unique_ptr<dom::Node> removed = new_child->parent()->RemoveChild(new_child);
    impl->TakeOwnership(new_child, std::move(removed));
  }
  std::unique_ptr<dom::Node> owned = impl->ReleaseOwned(new_child);
  if (owned == nullptr) {
    return JS_ThrowTypeError(ctx, "replaceChild: internal ownership error");
  }
  parent->InsertBefore(std::move(owned), old_child);
  std::unique_ptr<dom::Node> removed_old = parent->RemoveChild(old_child);
  impl->TakeOwnership(old_child, std::move(removed_old));
  return impl->WrapNode(new_child);
}

JSValue NodeNormalize(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "normalize: detached node");
  }
  // Snapshot the children: merging calls RemoveChild, which mutates the
  // children vector, and range-iterating a view over it while erasing would
  // invalidate the end iterator (out-of-bounds dereference).
  std::vector<dom::Node*> children;
  children.reserve(node->child_count());
  for (dom::Node* child : node->ChildNodes()) {
    children.push_back(child);
  }
  // Merge adjacent Text nodes (DOM spec: normalize()).
  for (dom::Node* child : children) {
    if (child->node_type() != dom::NodeType::kText) {
      continue;
    }
    while (true) {
      dom::Node* next = SiblingOf(child, +1);
      if (next == nullptr || next->node_type() != dom::NodeType::kText) {
        break;
      }
      auto* text = static_cast<dom::Text*>(child);
      auto* next_text = static_cast<dom::Text*>(next);
      text->AppendData(next_text->data());
      std::unique_ptr<dom::Node> removed = node->RemoveChild(next);
      impl->TakeOwnership(next, std::move(removed));
    }
  }
  // Recurse into element children (normalize() applies to the whole subtree).
  for (dom::Node* child : children) {
    if (child->node_type() == dom::NodeType::kElement) {
      JSValue child_wrap = impl->WrapNode(child);
      JSValue result = NodeNormalize(ctx, child_wrap, 0, nullptr);
      JS_FreeValue(ctx, child_wrap);
      if (JS_IsException(result)) {
        return result;
      }
      JS_FreeValue(ctx, result);
    }
  }
  return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Element methods and accessors.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Prototype construction.
// ---------------------------------------------------------------------------

void DefineNodePrototype(JSContext* ctx, Impl& impl)
{
  static const std::array<JSCFunctionListEntry, 13> kMethods = {{
      JS_CFUNC_DEF("appendChild", 1, NodeAppendChild),
      JS_CFUNC_DEF("append", 0, NodeAppend),
      JS_CFUNC_DEF("replaceChildren", 0, NodeReplaceChildren),
      JS_CFUNC_DEF("insertBefore", 2, NodeInsertBefore),
      JS_CFUNC_DEF("replaceChild", 2, NodeReplaceChild),
      JS_CFUNC_DEF("removeChild", 1, NodeRemoveChild),
      JS_CFUNC_DEF("hasChildNodes", 0, NodeHasChildNodes),
      JS_CFUNC_DEF("cloneNode", 1, NodeCloneNode),
      JS_CFUNC_DEF("contains", 1, NodeContains),
      JS_CFUNC_DEF("normalize", 0, NodeNormalize),
      JS_CFUNC_DEF("addEventListener", 2, NodeAddEventListener),
      JS_CFUNC_DEF("removeEventListener", 2, NodeRemoveEventListener),
      JS_CFUNC_DEF("dispatchEvent", 1, NodeDispatchEvent),
  }};
  JS_SetPropertyFunctionList(
      ctx, impl.node_proto, kMethods.data(), static_cast<int>(kMethods.size()));

  DefineGetter(ctx, impl.node_proto, "nodeType", MakeGetter(ctx, "nodeType", NodeGetNodeType));
  DefineGetter(ctx, impl.node_proto, "nodeName", MakeGetter(ctx, "nodeName", NodeGetNodeName));
  DefineAccessor(ctx,
                 impl.node_proto,
                 "textContent",
                 MakeGetter(ctx, "textContent", NodeGetTextContent),
                 MakeSetter(ctx, "textContent", NodeSetTextContent));
  DefineGetter(
      ctx, impl.node_proto, "parentNode", MakeGetter(ctx, "parentNode", NodeGetParentNode));
  DefineGetter(ctx,
               impl.node_proto,
               "parentElement",
               MakeGetter(ctx, "parentElement", NodeGetParentElement));
  DefineGetter(
      ctx, impl.node_proto, "firstChild", MakeGetter(ctx, "firstChild", NodeGetFirstChild));
  DefineGetter(ctx, impl.node_proto, "lastChild", MakeGetter(ctx, "lastChild", NodeGetLastChild));
  DefineGetter(
      ctx, impl.node_proto, "childNodes", MakeGetter(ctx, "childNodes", NodeGetChildNodes));
  DefineGetter(
      ctx, impl.node_proto, "nextSibling", MakeGetter(ctx, "nextSibling", NodeGetNextSibling));
  DefineGetter(ctx,
               impl.node_proto,
               "previousSibling",
               MakeGetter(ctx, "previousSibling", NodeGetPreviousSibling));
  DefineGetter(ctx,
               impl.node_proto,
               "ownerDocument",
               MakeGetter(ctx, "ownerDocument", NodeGetOwnerDocument));
  DefineGetter(
      ctx, impl.node_proto, "isConnected", MakeGetter(ctx, "isConnected", NodeGetIsConnected));
  DefineAccessor(ctx,
                 impl.node_proto,
                 "nodeValue",
                 MakeGetter(ctx, "nodeValue", NodeGetNodeValue),
                 MakeSetter(ctx, "nodeValue", NodeSetNodeValue));
}

JSValue NodeListItem(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  if (argc < 1) {
    return JS_NULL;
  }
  int32_t index = -1;
  (void)JS_ToInt32(ctx, &index, argv[0]);
  if (index < 0) {
    return JS_NULL;
  }
  return JS_GetPropertyUint32(ctx, this_val, static_cast<uint32_t>(index));
}

JSValue NodeListLength(JSContext* ctx, JSValueConst this_val)
{
  JSValue value = JS_GetPropertyStr(ctx, this_val, "length");
  return value;
}

} // namespace neko::javascript
