// neko::javascript::DomBinder — binds a DOM document into a JavaScript
// runtime (Phase 8 M2).  Exposes a practical subset of the DOM APIs and a
// minimal event loop (setTimeout/setInterval + addEventListener), all behind
// the project-owned ScriptEngine.
//
// Threading: thread-confined, like ScriptEngine.  Use a binder from one
// thread at a time.
//
// Ownership model (see dom_binding.h for the full contract):
//   * The binder owns its ScriptEngine (one runtime per document).
//   * Node wrappers are tracked in a registry that keeps each wrapper alive
//     for the binder's lifetime; the wrapper's opaque data holds the raw
//     dom::Node*.
//   * Nodes removed from the tree are retained (moved into |retained|) so
//     their memory stays valid and JS can re-insert them; they are freed when
//     the binder is destroyed.
//   * Elements created via createElement/createTextNode are owned by the
//     binder (|created|) until appended into the document.

#include "neko/javascript/dom_binding.h"

#include "neko/base/status.h"
#include "neko/css/parser.h"
#include "neko/dom/query.h"
#include "neko/html/parser.h"
#include "neko/javascript/script_engine_internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <quickjs.h>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace neko::javascript {

namespace {

// ---------------------------------------------------------------------------
// Process-wide QuickJS plumbing.
//
// Every DomBinder creates its own runtime, but the node-wrapper class id and
// the ctx -> Impl registry are shared across runtimes.  The class id is
// assigned once (JS_NewClassID is idempotent for a non-zero output) and the
// class is registered once per runtime.
// ---------------------------------------------------------------------------

JSClassID g_node_class_id = 0;
std::mutex g_class_mutex;
std::unordered_set<JSRuntime*> g_class_registered;

// Opaque data attached to every node wrapper (also reused for style objects,
// whose opaque carries the owning element).
struct NodeWrapper
{
  Impl* impl = nullptr;
  dom::Node* node = nullptr; // null when detached
};

void NodeFinalizer(JSRuntime* /*rt*/, JSValue obj)
{
  auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(obj, g_node_class_id));
  delete w;
}

std::mutex g_ctx_mutex;
std::unordered_map<JSContext*, Impl*> g_ctx_to_impl;

// Resolves the owning Impl for a C callback.  Prefers the opaque wrapper on
// |this_val| (node methods), then falls back to the ctx registry (global
// functions like setTimeout, whose this_val is the global object).
Impl* ImplFor(JSContext* ctx, JSValueConst this_val)
{
  auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, g_node_class_id));
  if (w != nullptr && w->impl != nullptr) {
    return w->impl;
  }
  std::lock_guard<std::mutex> lock(g_ctx_mutex);
  const auto it = g_ctx_to_impl.find(ctx);
  return it != g_ctx_to_impl.end() ? it->second : nullptr;
}

dom::Node* UnwrapNode(JSValueConst this_val)
{
  auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, g_node_class_id));
  return w != nullptr ? w->node : nullptr;
}

dom::Element* AsElement(dom::Node* node)
{
  return node != nullptr && node->node_type() == dom::NodeType::kElement
             ? static_cast<dom::Element*>(node)
             : nullptr;
}

int32_t NodeTypeNumber(dom::NodeType type)
{
  switch (type) {
  case dom::NodeType::kDocument:
    return 9;
  case dom::NodeType::kElement:
    return 1;
  case dom::NodeType::kText:
    return 3;
  case dom::NodeType::kComment:
    return 8;
  case dom::NodeType::kDocumentFragment:
    return 11;
  }
  return 0;
}

// The engine's reported platform string (navigator.platform), matching what
// mainstream browsers report per OS.
const char* NavigatorPlatform()
{
#if defined(_WIN32)
  return "Win32";
#elif defined(__APPLE__)
  return "MacIntel";
#else
  return "Linux";
#endif
}

std::string NodeNameOf(const dom::Node& node)
{
  switch (node.node_type()) {
  case dom::NodeType::kElement: {
    const auto& el = static_cast<const dom::Element&>(node);
    std::string out;
    out.reserve(el.tag_name().size());
    for (const char c : el.tag_name()) {
      out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
  }
  case dom::NodeType::kDocument:
    return "#document";
  case dom::NodeType::kText:
    return "#text";
  case dom::NodeType::kComment:
    return "#comment";
  case dom::NodeType::kDocumentFragment:
    return "#document-fragment";
  }
  return "";
}

// Converts an argument to a string (JS ToString semantics).  On conversion
// failure a pending exception is cleared and false is returned.
std::string ArgString(JSContext* ctx, JSValueConst value, bool* ok)
{
  *ok = true;
  std::size_t len = 0;
  const char* s = JS_ToCStringLen(ctx, &len, value);
  if (s == nullptr) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    *ok = false;
    return {};
  }
  std::string out(s, len);
  JS_FreeCString(ctx, s);
  return out;
}

std::string ToLower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

bool IsAncestorOf(const dom::Node* ancestor, const dom::Node* node)
{
  for (const dom::Node* p = node->parent(); p != nullptr; p = p->parent()) {
    if (p == ancestor) {
      return true;
    }
  }
  return false;
}

// Deep or shallow clone of a node (cloneNode).  Documents cannot be cloned.
std::unique_ptr<dom::Node> CloneNodeImpl(const dom::Node& source, bool deep)
{
  switch (source.node_type()) {
  case dom::NodeType::kElement: {
    const auto& el = static_cast<const dom::Element&>(source);
    auto clone = std::make_unique<dom::Element>(std::string(el.tag_name()));
    for (const dom::Attribute& attr : el.attributes()) {
      clone->SetAttribute(attr.name, attr.value);
    }
    if (deep) {
      for (dom::Node* child : source.ChildNodes()) {
        clone->AppendChild(CloneNodeImpl(*child, true));
      }
    }
    return clone;
  }
  case dom::NodeType::kText:
    return std::make_unique<dom::Text>(static_cast<const dom::Text&>(source).data());
  case dom::NodeType::kComment:
    return std::make_unique<dom::Comment>(static_cast<const dom::Comment&>(source).data());
  case dom::NodeType::kDocumentFragment: {
    auto clone = std::make_unique<dom::DocumentFragment>();
    if (deep) {
      for (dom::Node* child : source.ChildNodes()) {
        clone->AppendChild(CloneNodeImpl(*child, true));
      }
    }
    return clone;
  }
  case dom::NodeType::kDocument:
    return nullptr;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Inline style attribute helpers (backing CSSStyleDeclaration).
// ---------------------------------------------------------------------------

struct InlineDecl
{
  std::string property; // lowercased
  std::string value;    // trimmed, without !important
  bool important = false;
};

std::vector<InlineDecl> ParseInlineStyle(std::string_view attr)
{
  std::vector<InlineDecl> out;
  for (const css::Declaration& d : css::ParseDeclarationBlock(attr)) {
    out.push_back(InlineDecl{d.property, d.value, d.important});
  }
  return out;
}

std::string SerializeInlineStyle(const std::vector<InlineDecl>& decls)
{
  std::string out;
  for (std::size_t i = 0; i < decls.size(); ++i) {
    if (i > 0) {
      out += "; ";
    }
    out += decls[i].property;
    out += ": ";
    out += decls[i].value;
    if (decls[i].important) {
      out += " !important";
    }
  }
  return out;
}

std::string CurrentStyleAttr(const dom::Element& element)
{
  const std::optional<std::string_view> attr = element.GetAttribute("style");
  return attr.has_value() ? std::string(attr.value()) : std::string();
}

void SetStyleAttr(dom::Element& element, const std::string& value)
{
  if (value.empty()) {
    element.RemoveAttribute("style");
  } else {
    element.SetAttribute("style", value);
  }
}

// The CSS property names reachable through the direct style accessors.  The
// magic value of a getter/setter indexes this table.
constexpr std::array<std::string_view, 17> kStyleProps = {
    "width",
    "height",
    "color",
    "background-color",
    "font-size",
    "font-family",
    "line-height",
    "text-align",
    "display",
    "margin-top",
    "margin-right",
    "margin-bottom",
    "margin-left",
    "padding-top",
    "padding-right",
    "padding-bottom",
    "padding-left",
};

// ---------------------------------------------------------------------------
// Accessor/method registration helpers.
//
// QuickJS's C API stores every native callback as a JSCFunction* and selects
// the actual calling convention through the cproto argument.  Casting the
// (differently-typed) getter/setter/magic functions to JSCFunction* is the
// documented extension pattern; the cast-function-type warning is suppressed
// locally because the conversion is required by the QuickJS API.
// ---------------------------------------------------------------------------

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-function-type"
#endif

void DefineAccessor(JSContext* ctx, JSValue proto, const char* name, JSValue getter, JSValue setter)
{
  JSAtom atom = JS_NewAtom(ctx, name);
  JS_DefinePropertyGetSet(ctx, proto, atom, getter, setter, JS_PROP_C_W_E);
  JS_FreeAtom(ctx, atom);
}

template <typename Fn> JSValue MakeGetter(JSContext* ctx, const char* name, Fn fn)
{
  return JS_NewCFunction2(ctx, reinterpret_cast<JSCFunction*>(fn), name, 0, JS_CFUNC_getter, 0);
}

template <typename Fn> JSValue MakeSetter(JSContext* ctx, const char* name, Fn fn)
{
  return JS_NewCFunction2(ctx, reinterpret_cast<JSCFunction*>(fn), name, 0, JS_CFUNC_setter, 0);
}

template <typename Fn> JSValue MakeGetterMagic(JSContext* ctx, const char* name, Fn fn, int magic)
{
  return JS_NewCFunction2(
      ctx, reinterpret_cast<JSCFunction*>(fn), name, 0, JS_CFUNC_getter_magic, magic);
}

template <typename Fn> JSValue MakeSetterMagic(JSContext* ctx, const char* name, Fn fn, int magic)
{
  return JS_NewCFunction2(
      ctx, reinterpret_cast<JSCFunction*>(fn), name, 0, JS_CFUNC_setter_magic, magic);
}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

void DefineGetter(JSContext* ctx, JSValue proto, const char* name, JSValue getter)
{
  DefineAccessor(ctx, proto, name, getter, JS_UNDEFINED);
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct Impl
{
  dom::Document& document;
  ScriptEngine engine;
  JSContext* ctx = nullptr;

  // Prototypes (own references, freed in the destructor).
  JSValue node_proto = JS_UNDEFINED;
  JSValue element_proto = JS_UNDEFINED;
  JSValue text_proto = JS_UNDEFINED;
  JSValue comment_proto = JS_UNDEFINED;
  JSValue document_proto = JS_UNDEFINED;
  JSValue fragment_proto = JS_UNDEFINED;
  JSValue style_proto = JS_UNDEFINED;
  JSValue window = JS_UNDEFINED;

  // Wrapper registry: node -> kept-alive wrapper (JS_DupValue'd).  Wrappers
  // live for the binder's lifetime so a detached node can never dangle.
  std::unordered_map<const dom::Node*, JSValue> wrappers;

  // Nodes created via createElement/createTextNode/cloneNode and not yet in a
  // tree.  Owned here until appended into the document.
  std::unordered_map<dom::Node*, std::unique_ptr<dom::Node>> created;

  // Nodes removed from a tree (and their subtrees).  Owned here so JS-held
  // wrappers keep pointing at valid memory; freed when the binder dies.
  std::vector<std::unique_ptr<dom::Node>> retained;

  // Timers (setTimeout/setInterval).  Callback JSValues are Dup'd.
  struct Timer
  {
    int64_t id = 0;
    JSValue callback = JS_UNDEFINED;
    std::chrono::steady_clock::time_point due;
    std::chrono::milliseconds interval{0};
    bool repeating = false;
  };
  std::vector<Timer> timers;
  int64_t next_timer_id = 1;

  // Event listeners: node -> (type -> callbacks).  Keyed by node (elements
  // and the document; window-level listeners are stored under the document,
  // since the window and the document are the same event target here).
  // Callbacks are Dup'd; nodes stay alive through |wrappers|/|retained| for
  // the binder's life.
  using ListenerMap = std::unordered_map<std::string, std::vector<JSValue>>;
  std::unordered_map<const dom::Node*, ListenerMap> listeners;

  explicit Impl(dom::Document& doc);
  ~Impl();

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  JSValue WrapNode(dom::Node* node);
  JSValue PrototypeFor(const dom::Node* node) const;
  JSValue MakeNodeArray(const std::vector<dom::Node*>& nodes);
  JSValue MakeElementArray(const std::vector<dom::Element*>& elements);

  void TakeOwnership(dom::Node* node, std::unique_ptr<dom::Node> owned);
  std::unique_ptr<dom::Node> ReleaseOwned(dom::Node* node);

  int RunPendingTimers();
  std::optional<std::chrono::steady_clock::time_point> NextTimerDeadline() const;
  void DispatchToNode(dom::Node* node, std::string_view type);
  void DispatchEvent(dom::Element& element, std::string_view type);
  void DispatchDocumentEvent(std::string_view type);
};

namespace {

// ---------------------------------------------------------------------------
// Node methods and accessors.
// ---------------------------------------------------------------------------

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
    return JS_ThrowTypeError(ctx, "appendChild: cannot append a node to itself");
  }
  if (child->node_type() == dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "appendChild: cannot append a Document node");
  }
  if (IsAncestorOf(child, parent)) {
    return JS_ThrowTypeError(ctx, "appendChild: cannot append an ancestor");
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
    return JS_ThrowTypeError(ctx, "insertBefore: reference is not a child of this node");
  }
  if (child == parent || child->node_type() == dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "insertBefore: invalid node");
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
    return JS_ThrowTypeError(ctx, "removeChild: argument is not a child of this node");
  }
  std::unique_ptr<dom::Node> removed = parent->RemoveChild(child);
  impl->TakeOwnership(child, std::move(removed));
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
  impl->listeners[node][type].push_back(JS_DupValue(ctx, argv[1]));
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
  const auto el_it = impl->listeners.find(node);
  if (el_it == impl->listeners.end()) {
    return JS_UNDEFINED;
  }
  auto type_it = el_it->second.find(type);
  if (type_it == el_it->second.end()) {
    return JS_UNDEFINED;
  }
  std::vector<JSValue>& callbacks = type_it->second;
  for (auto it = callbacks.begin(); it != callbacks.end(); ++it) {
    if (JS_IsStrictEqual(ctx, *it, argv[1])) {
      JS_FreeValue(ctx, *it);
      callbacks.erase(it);
      break;
    }
  }
  if (callbacks.empty()) {
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
  JSValue type_val = JS_GetPropertyStr(ctx, argv[0], "type");
  bool ok = false;
  const std::string type = ArgString(ctx, type_val, &ok);
  JS_FreeValue(ctx, type_val);
  if (!ok) {
    return JS_EXCEPTION;
  }
  // Populate target/currentTarget on the caller's event object.
  JSValue target = impl->WrapNode(node);
  JS_SetPropertyStr(ctx, argv[0], "target", JS_DupValue(ctx, target));
  JS_SetPropertyStr(ctx, argv[0], "currentTarget", JS_DupValue(ctx, target));
  JS_FreeValue(ctx, target);

  const auto el_it = impl->listeners.find(node);
  if (el_it != impl->listeners.end()) {
    const auto type_it = el_it->second.find(type);
    if (type_it != el_it->second.end()) {
      // Snapshot so listeners added/removed during dispatch don't mutate the
      // list we iterate.
      std::vector<JSValue> callbacks;
      callbacks.reserve(type_it->second.size());
      for (JSValue cb : type_it->second) {
        callbacks.push_back(JS_DupValue(ctx, cb));
      }
      JSValue this_wrap = impl->WrapNode(node);
      for (JSValue cb : callbacks) {
        JSValue result = JS_Call(ctx, cb, this_wrap, 1, argv);
        if (JS_IsException(result)) {
          JS_FreeValue(ctx, JS_GetException(ctx));
        } else {
          JS_FreeValue(ctx, result);
        }
        JS_FreeValue(ctx, cb);
      }
      JS_FreeValue(ctx, this_wrap);
    }
  }
  return JS_NewBool(ctx, true);
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
  while (node->first_child() != nullptr) {
    std::unique_ptr<dom::Node> removed = node->RemoveChild(node->first_child());
    impl->TakeOwnership(removed.get(), std::move(removed));
  }
  if (!text.empty()) {
    node->AppendChild(std::make_unique<dom::Text>(text));
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

// ---------------------------------------------------------------------------
// Element methods and accessors.
// ---------------------------------------------------------------------------

JSValue ElementGetTagName(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string name = NodeNameOf(*element);
  return JS_NewStringLen(ctx, name.data(), name.size());
}

JSValue ElementGetId(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::optional<std::string_view> id = element->Id();
  if (id.has_value()) {
    return JS_NewStringLen(ctx, id->data(), id->size());
  }
  return JS_NewStringLen(ctx, "", 0);
}

JSValue ElementSetId(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string id = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (id.empty()) {
    element->RemoveAttribute("id");
  } else {
    element->SetAttribute("id", id);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetClassName(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::optional<std::string_view> cls = element->GetAttribute("class");
  if (cls.has_value()) {
    return JS_NewStringLen(ctx, cls->data(), cls->size());
  }
  return JS_NewStringLen(ctx, "", 0);
}

JSValue ElementSetClassName(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string cls = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (cls.empty()) {
    element->RemoveAttribute("class");
  } else {
    element->SetAttribute("class", cls);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetAttributes(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  JSValue arr = JS_NewArray(ctx);
  const auto& attrs = element->attributes();
  for (std::size_t i = 0; i < attrs.size(); ++i) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(
        ctx, obj, "name", JS_NewStringLen(ctx, attrs[i].name.data(), attrs[i].name.size()));
    JS_SetPropertyStr(
        ctx, obj, "value", JS_NewStringLen(ctx, attrs[i].value.data(), attrs[i].value.size()));
    JS_SetPropertyUint32(ctx, // NOLINT: (this_obj, index, value); steals obj
                         arr,
                         static_cast<uint32_t>(i),
                         obj);
  }
  return arr;
}

JSValue ElementGetChildren(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  std::vector<dom::Element*> elements;
  for (dom::Node* child : element->ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      elements.push_back(el);
    }
  }
  return impl->MakeElementArray(elements);
}

JSValue ElementGetFirstElementChild(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  for (dom::Node* child : element->ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      return impl->WrapNode(el);
    }
  }
  return JS_NULL;
}

JSValue ElementGetInnerHTML(JSContext* ctx, JSValueConst this_val)
{
  dom::Node* node = UnwrapNode(this_val);
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  std::string out;
  for (dom::Node* child : node->ChildNodes()) {
    out += child->ToString();
  }
  return JS_NewStringLen(ctx, out.data(), out.size());
}

JSValue ElementSetInnerHTML(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string html = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  // Parse the fragment through the full document parser and move the <body>
  // children into this element.  Scripts in the fragment are not executed.
  std::unique_ptr<dom::Document> parsed = html::Parser(html).Parse();
  dom::Element* body = nullptr;
  for (dom::Node* child : parsed->ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      if (el->tag_name() == "body") {
        body = el;
        break;
      }
      for (dom::Node* inner : el->ChildNodes()) {
        if (dom::Element* in_el = AsElement(inner)) {
          if (in_el->tag_name() == "body") {
            body = in_el;
            break;
          }
        }
      }
      if (body != nullptr) {
        break;
      }
    }
  }
  while (element->first_child() != nullptr) {
    std::unique_ptr<dom::Node> removed = element->RemoveChild(element->first_child());
    impl->TakeOwnership(removed.get(), std::move(removed));
  }
  if (body != nullptr) {
    while (body->first_child() != nullptr) {
      std::unique_ptr<dom::Node> child = body->RemoveChild(body->first_child());
      element->AppendChild(std::move(child));
    }
  }
  return JS_UNDEFINED;
}

JSValue ElementGetStyle(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // A fresh style object per access; its opaque carries the element so the
  // style methods can read/write the element's style attribute.
  auto* w = new NodeWrapper{impl, element};
  JSValue style = JS_NewObjectClass(ctx, g_node_class_id);
  JS_SetOpaque(style, w);
  JS_SetPrototype(ctx, style, impl->style_proto);
  return style;
}

JSValue ElementGetAttribute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "getAttribute requires a name");
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::optional<std::string_view> value = element->GetAttribute(name);
  if (value.has_value()) {
    return JS_NewStringLen(ctx, value->data(), value->size());
  }
  return JS_NULL;
}

JSValue ElementSetAttribute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "setAttribute requires (name, value)");
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::string value = ArgString(ctx, argv[1], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute(name, value);
  return JS_UNDEFINED;
}

JSValue ElementRemoveAttribute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (argc < 1) {
    return JS_UNDEFINED;
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->RemoveAttribute(name);
  return JS_UNDEFINED;
}

JSValue ElementHasAttribute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (argc < 1) {
    return JS_NewBool(ctx, false);
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  return JS_NewBool(ctx, element->HasAttribute(name));
}

JSValue ElementQuerySelector(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "querySelector requires a selector");
  }
  bool ok = false;
  const std::string selector = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  return impl->WrapNode(dom::QuerySelector(*node, selector));
}

JSValue ElementQuerySelectorAll(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "querySelectorAll requires a selector");
  }
  bool ok = false;
  const std::string selector = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  return impl->MakeElementArray(dom::QuerySelectorAll(*node, selector));
}

// Collects descendant elements whose tag matches |name| ("*" matches all).
void CollectByTag(const dom::Node& root, std::string_view name, std::vector<dom::Element*>& out)
{
  for (dom::Node* child : root.ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      if (name == "*" || el->tag_name() == name) {
        out.push_back(el);
      }
      CollectByTag(*el, name, out);
    }
  }
}

// Collects descendant elements that carry every class in |classes|.
void CollectByClass(const dom::Node& root,
                    const std::vector<std::string_view>& classes,
                    std::vector<dom::Element*>& out)
{
  for (dom::Node* child : root.ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      const std::vector<std::string_view> el_classes = el->ClassList();
      bool match = true;
      for (std::string_view cls : classes) {
        const bool found = std::any_of(
            el_classes.begin(), el_classes.end(), [&](std::string_view c) { return c == cls; });
        if (!found) {
          match = false;
          break;
        }
      }
      if (match) {
        out.push_back(el);
      }
      CollectByClass(*el, classes, out);
    }
  }
}

JSValue
ElementGetElementsByTagName(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "getElementsByTagName requires a name");
  }
  bool ok = false;
  const std::string name = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    return JS_EXCEPTION;
  }
  std::vector<dom::Element*> out;
  CollectByTag(*node, name, out);
  return impl->MakeElementArray(out);
}

JSValue
ElementGetElementsByClassName(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "getElementsByClassName requires a name");
  }
  bool ok = false;
  const std::string arg = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  std::vector<std::string_view> classes;
  std::size_t i = 0;
  while (i < arg.size()) {
    while (i < arg.size() && (arg[i] == ' ' || arg[i] == '\t')) {
      ++i;
    }
    const std::size_t start = i;
    while (i < arg.size() && arg[i] != ' ' && arg[i] != '\t') {
      ++i;
    }
    if (i > start) {
      classes.emplace_back(arg.data() + start, i - start);
    }
  }
  std::vector<dom::Element*> out;
  CollectByClass(*node, classes, out);
  return impl->MakeElementArray(out);
}

// ---------------------------------------------------------------------------
// Document methods and accessors.
// ---------------------------------------------------------------------------

dom::Element* FindElementByTag(const dom::Node& root, std::string_view tag);

JSValue DocGetDocumentElement(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr || node->node_type() != dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  return impl->WrapNode(static_cast<dom::Document*>(node)->document_element());
}

JSValue DocGetBody(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr || node->node_type() != dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  for (dom::Node* child : node->ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      if (el->tag_name() == "body") {
        return impl->WrapNode(el);
      }
      for (dom::Node* inner : el->ChildNodes()) {
        if (dom::Element* in_el = AsElement(inner)) {
          if (in_el->tag_name() == "body") {
            return impl->WrapNode(in_el);
          }
        }
      }
    }
  }
  return JS_NULL;
}

JSValue DocGetHead(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr || node->node_type() != dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  dom::Element* head = FindElementByTag(*node, "head");
  return head != nullptr ? impl->WrapNode(head) : JS_NULL;
}

JSValue DocGetReadyState(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr || node->node_type() != dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  // Scripts run after parsing completes, so the document is always fully
  // loaded by the time any page script can observe it.
  return JS_NewString(ctx, "complete");
}

JSValue DocGetTitle(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr || node->node_type() != dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  const std::string title = static_cast<dom::Document*>(node)->Title();
  return JS_NewStringLen(ctx, title.data(), title.size());
}

JSValue DocSetTitle(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr || node->node_type() != dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  bool ok = false;
  const std::string title = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  // Find the existing <title> element (any depth); create one in <head> when
  // missing (or leave it detached when the document has no <head> yet).
  dom::Element* title_el = FindElementByTag(*node, "title");
  if (title_el == nullptr) {
    auto new_title = std::make_unique<dom::Element>("title");
    title_el = new_title.get();
    dom::Element* head = FindElementByTag(*node, "head");
    if (head != nullptr) {
      head->AppendChild(std::move(new_title));
    } else {
      // No head: keep a detached owned element (title still reports).
      impl->created[title_el] = std::move(new_title);
    }
  }
  while (title_el->first_child() != nullptr) {
    std::unique_ptr<dom::Node> removed = title_el->RemoveChild(title_el->first_child());
    impl->TakeOwnership(removed.get(), std::move(removed));
  }
  if (!title.empty()) {
    title_el->AppendChild(std::make_unique<dom::Text>(title));
  }
  return JS_UNDEFINED;
}

dom::Element* FindById(const dom::Node& root, std::string_view id)
{
  for (dom::Node* child : root.ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      const std::optional<std::string_view> el_id = el->Id();
      if (el_id.has_value() && el_id.value() == id) {
        return el;
      }
      if (dom::Element* found = FindById(*el, id)) {
        return found;
      }
    }
  }
  return nullptr;
}

// First element with the given tag name in pre-order, or nullptr.
dom::Element* FindElementByTag(const dom::Node& root, std::string_view tag)
{
  for (dom::Node* child : root.ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      if (el->tag_name() == tag) {
        return el;
      }
      if (dom::Element* found = FindElementByTag(*el, tag)) {
        return found;
      }
    }
  }
  return nullptr;
}

JSValue DocGetElementById(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  if (argc < 1) {
    return JS_NULL;
  }
  bool ok = false;
  const std::string id = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  return impl->WrapNode(FindById(*node, id));
}

JSValue DocCreateElement(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "createElement requires a tag name");
  }
  bool ok = false;
  const std::string tag = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (tag.empty()) {
    return JS_ThrowTypeError(ctx, "createElement: empty tag name");
  }
  auto element = std::make_unique<dom::Element>(tag);
  dom::Element* raw = element.get();
  impl->created[raw] = std::move(element);
  return impl->WrapNode(raw);
}

JSValue DocCreateTextNode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  bool ok = false;
  const std::string text = argc >= 1 ? ArgString(ctx, argv[0], &ok) : std::string();
  if (!ok) {
    return JS_EXCEPTION;
  }
  auto text_node = std::make_unique<dom::Text>(text);
  dom::Text* raw = text_node.get();
  impl->created[raw] = std::move(text_node);
  return impl->WrapNode(raw);
}

JSValue DocQuerySelector(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "querySelector requires a selector");
  }
  bool ok = false;
  const std::string selector = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  return impl->WrapNode(dom::QuerySelector(*node, selector));
}

JSValue DocQuerySelectorAll(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "querySelectorAll requires a selector");
  }
  bool ok = false;
  const std::string selector = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  return impl->MakeElementArray(dom::QuerySelectorAll(*node, selector));
}

// ---------------------------------------------------------------------------
// CSSStyleDeclaration methods and accessors.
// ---------------------------------------------------------------------------

JSValue StyleSetProperty(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not a style declaration");
  }
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "setProperty requires (property, value)");
  }
  bool ok = false;
  const std::string name = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::string value = ArgString(ctx, argv[1], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  std::vector<InlineDecl> decls = ParseInlineStyle(CurrentStyleAttr(*element));
  decls.erase(std::remove_if(decls.begin(),
                             decls.end(),
                             [&](const InlineDecl& d) { return d.property == name; }),
              decls.end());
  if (!value.empty()) {
    decls.push_back(InlineDecl{name, value, false});
  }
  SetStyleAttr(*element, SerializeInlineStyle(decls));
  return JS_UNDEFINED;
}

JSValue StyleGetPropertyValue(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not a style declaration");
  }
  if (argc < 1) {
    return JS_NewStringLen(ctx, "", 0);
  }
  bool ok = false;
  const std::string name = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    return JS_EXCEPTION;
  }
  for (const InlineDecl& d : ParseInlineStyle(CurrentStyleAttr(*element))) {
    if (d.property == name) {
      return JS_NewStringLen(ctx, d.value.data(), d.value.size());
    }
  }
  return JS_NewStringLen(ctx, "", 0);
}

JSValue StyleRemoveProperty(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not a style declaration");
  }
  bool ok = false;
  const std::string name = argc >= 1 ? ToLower(ArgString(ctx, argv[0], &ok)) : std::string();
  if (!ok) {
    return JS_EXCEPTION;
  }
  std::vector<InlineDecl> decls = ParseInlineStyle(CurrentStyleAttr(*element));
  std::string removed;
  decls.erase(std::remove_if(decls.begin(),
                             decls.end(),
                             [&](const InlineDecl& d) {
                               if (d.property == name) {
                                 removed = d.value;
                                 return true;
                               }
                               return false;
                             }),
              decls.end());
  SetStyleAttr(*element, SerializeInlineStyle(decls));
  return JS_NewStringLen(ctx, removed.data(), removed.size());
}

JSValue StyleGetProperty(JSContext* ctx, JSValueConst this_val, int magic)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not a style declaration");
  }
  if (magic < 0 || magic >= static_cast<int>(kStyleProps.size())) {
    return JS_NewStringLen(ctx, "", 0);
  }
  const std::string_view prop = kStyleProps[static_cast<std::size_t>(magic)];
  for (const InlineDecl& d : ParseInlineStyle(CurrentStyleAttr(*element))) {
    if (d.property == prop) {
      return JS_NewStringLen(ctx, d.value.data(), d.value.size());
    }
  }
  return JS_NewStringLen(ctx, "", 0);
}

JSValue StyleSetPropertyDirect(JSContext* ctx, JSValueConst this_val, JSValueConst value, int magic)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not a style declaration");
  }
  bool ok = false;
  const std::string prop_value = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (magic < 0 || magic >= static_cast<int>(kStyleProps.size())) {
    return JS_UNDEFINED;
  }
  const std::string_view prop = kStyleProps[static_cast<std::size_t>(magic)];
  std::vector<InlineDecl> decls = ParseInlineStyle(CurrentStyleAttr(*element));
  decls.erase(std::remove_if(decls.begin(),
                             decls.end(),
                             [&](const InlineDecl& d) { return d.property == prop; }),
              decls.end());
  if (!prop_value.empty()) {
    decls.push_back(InlineDecl{std::string(prop), prop_value, false});
  }
  SetStyleAttr(*element, SerializeInlineStyle(decls));
  return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Global timer functions (setTimeout/setInterval/clearTimeout/clearInterval).
// ---------------------------------------------------------------------------

JSValue TimerCreate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
    return JS_ThrowTypeError(ctx, "the first argument must be a function");
  }
  int64_t ms = 0;
  if (argc >= 2) {
    if (JS_ToInt64(ctx, &ms, argv[1]) != 0) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return JS_EXCEPTION;
    }
  }
  if (ms < 0) {
    ms = 0;
  }
  Impl::Timer timer;
  timer.id = impl->next_timer_id++;
  timer.callback = JS_DupValue(ctx, argv[0]);
  timer.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  timer.interval = std::chrono::milliseconds(ms);
  timer.repeating = magic == 1;
  const int64_t timer_id = timer.id;
  impl->timers.push_back(timer); // Timer is trivially copyable; the copy owns the callback
  return JS_NewInt64(ctx, timer_id);
}

JSValue
TimerClear(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int /*magic*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_UNDEFINED;
  }
  int64_t id = 0;
  if (argc >= 1) {
    if (JS_ToInt64(ctx, &id, argv[0]) != 0) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return JS_UNDEFINED;
    }
  }
  for (auto it = impl->timers.begin(); it != impl->timers.end(); ++it) {
    if (it->id == id) {
      JS_FreeValue(ctx, it->callback);
      impl->timers.erase(it);
      break;
    }
  }
  return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Window-level event functions.
//
// The window is a plain object (not a node), but in this minimal model the
// window and the document share one event target set: window-level listeners
// are stored under the document node.  So window.addEventListener(...) (and
// the bare global addEventListener(...) aliases) forward to the document.
// ---------------------------------------------------------------------------

JSValue WindowAddEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  JSValue doc_wrap = impl->WrapNode(&impl->document);
  JSValue result = NodeAddEventListener(ctx, doc_wrap, argc, argv);
  JS_FreeValue(ctx, doc_wrap);
  return result;
}

JSValue
WindowRemoveEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  JSValue doc_wrap = impl->WrapNode(&impl->document);
  JSValue result = NodeRemoveEventListener(ctx, doc_wrap, argc, argv);
  JS_FreeValue(ctx, doc_wrap);
  return result;
}

JSValue WindowDispatchEvent(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  JSValue doc_wrap = impl->WrapNode(&impl->document);
  JSValue result = NodeDispatchEvent(ctx, doc_wrap, argc, argv);
  JS_FreeValue(ctx, doc_wrap);
  return result;
}

// ---------------------------------------------------------------------------
// Prototype construction.
// ---------------------------------------------------------------------------

void DefineNodePrototype(JSContext* ctx, Impl& impl)
{
  static const std::array<JSCFunctionListEntry, 10> kMethods = {{
      JS_CFUNC_DEF("appendChild", 1, NodeAppendChild),
      JS_CFUNC_DEF("append", 0, NodeAppend),
      JS_CFUNC_DEF("replaceChildren", 0, NodeReplaceChildren),
      JS_CFUNC_DEF("insertBefore", 2, NodeInsertBefore),
      JS_CFUNC_DEF("removeChild", 1, NodeRemoveChild),
      JS_CFUNC_DEF("hasChildNodes", 0, NodeHasChildNodes),
      JS_CFUNC_DEF("cloneNode", 1, NodeCloneNode),
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
  DefineGetter(
      ctx, impl.node_proto, "firstChild", MakeGetter(ctx, "firstChild", NodeGetFirstChild));
  DefineGetter(ctx, impl.node_proto, "lastChild", MakeGetter(ctx, "lastChild", NodeGetLastChild));
  DefineGetter(
      ctx, impl.node_proto, "childNodes", MakeGetter(ctx, "childNodes", NodeGetChildNodes));
}

void DefineElementPrototype(JSContext* ctx, Impl& impl)
{
  static const std::array<JSCFunctionListEntry, 8> kMethods = {{
      JS_CFUNC_DEF("getAttribute", 1, ElementGetAttribute),
      JS_CFUNC_DEF("setAttribute", 2, ElementSetAttribute),
      JS_CFUNC_DEF("removeAttribute", 1, ElementRemoveAttribute),
      JS_CFUNC_DEF("hasAttribute", 1, ElementHasAttribute),
      JS_CFUNC_DEF("querySelector", 1, ElementQuerySelector),
      JS_CFUNC_DEF("querySelectorAll", 1, ElementQuerySelectorAll),
      JS_CFUNC_DEF("getElementsByTagName", 1, ElementGetElementsByTagName),
      JS_CFUNC_DEF("getElementsByClassName", 1, ElementGetElementsByClassName),
  }};
  JS_SetPropertyFunctionList(
      ctx, impl.element_proto, kMethods.data(), static_cast<int>(kMethods.size()));

  DefineGetter(ctx, impl.element_proto, "tagName", MakeGetter(ctx, "tagName", ElementGetTagName));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "id",
                 MakeGetter(ctx, "id", ElementGetId),
                 MakeSetter(ctx, "id", ElementSetId));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "className",
                 MakeGetter(ctx, "className", ElementGetClassName),
                 MakeSetter(ctx, "className", ElementSetClassName));
  DefineGetter(
      ctx, impl.element_proto, "attributes", MakeGetter(ctx, "attributes", ElementGetAttributes));
  DefineGetter(
      ctx, impl.element_proto, "children", MakeGetter(ctx, "children", ElementGetChildren));
  DefineGetter(ctx,
               impl.element_proto,
               "firstElementChild",
               MakeGetter(ctx, "firstElementChild", ElementGetFirstElementChild));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "innerHTML",
                 MakeGetter(ctx, "innerHTML", ElementGetInnerHTML),
                 MakeSetter(ctx, "innerHTML", ElementSetInnerHTML));
  DefineGetter(ctx, impl.element_proto, "style", MakeGetter(ctx, "style", ElementGetStyle));
}

void DefineDocumentPrototype(JSContext* ctx, Impl& impl)
{
  static const std::array<JSCFunctionListEntry, 5> kMethods = {{
      JS_CFUNC_DEF("getElementById", 1, DocGetElementById),
      JS_CFUNC_DEF("createElement", 1, DocCreateElement),
      JS_CFUNC_DEF("createTextNode", 1, DocCreateTextNode),
      JS_CFUNC_DEF("querySelector", 1, DocQuerySelector),
      JS_CFUNC_DEF("querySelectorAll", 1, DocQuerySelectorAll),
  }};
  JS_SetPropertyFunctionList(
      ctx, impl.document_proto, kMethods.data(), static_cast<int>(kMethods.size()));

  DefineGetter(ctx,
               impl.document_proto,
               "documentElement",
               MakeGetter(ctx, "documentElement", DocGetDocumentElement));
  DefineGetter(ctx, impl.document_proto, "body", MakeGetter(ctx, "body", DocGetBody));
  DefineGetter(ctx, impl.document_proto, "head", MakeGetter(ctx, "head", DocGetHead));
  DefineGetter(
      ctx, impl.document_proto, "readyState", MakeGetter(ctx, "readyState", DocGetReadyState));
  DefineAccessor(ctx,
                 impl.document_proto,
                 "title",
                 MakeGetter(ctx, "title", DocGetTitle),
                 MakeSetter(ctx, "title", DocSetTitle));
}

void DefineStylePrototype(JSContext* ctx, Impl& impl)
{
  static const std::array<JSCFunctionListEntry, 3> kMethods = {{
      JS_CFUNC_DEF("setProperty", 2, StyleSetProperty),
      JS_CFUNC_DEF("getPropertyValue", 1, StyleGetPropertyValue),
      JS_CFUNC_DEF("removeProperty", 1, StyleRemoveProperty),
  }};
  JS_SetPropertyFunctionList(
      ctx, impl.style_proto, kMethods.data(), static_cast<int>(kMethods.size()));

  // Direct camelCase accessors for the documented property subset.
  for (std::size_t i = 0; i < kStyleProps.size(); ++i) {
    const std::string_view prop = kStyleProps[i];
    std::string camel;
    bool upper = false;
    for (const char c : prop) {
      if (c == '-') {
        upper = true;
        continue;
      }
      camel.push_back(upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
      upper = false;
    }
    const int magic = static_cast<int>(i);
    DefineAccessor(ctx,
                   impl.style_proto,
                   camel.c_str(),
                   MakeGetterMagic(ctx, camel.c_str(), StyleGetProperty, magic),
                   MakeSetterMagic(ctx, camel.c_str(), StyleSetPropertyDirect, magic));
  }
}

// ---------------------------------------------------------------------------
// Web IDL-style interface constructors.
//
// Browsers expose the DOM interfaces as global constructors (Node, Element,
// ...).  Real pages rely on this in two ways: `x instanceof Element` and
// prototype extension (`Element.prototype.foo = ...`).  We expose each name
// as a constructor whose .prototype is the live prototype used by wrappers,
// so both work.  Constructing an interface directly is an error, matching
// browsers' "Illegal constructor".
// ---------------------------------------------------------------------------

JSValue IllegalConstructor(JSContext* ctx,
                           JSValueConst /*new_target*/,
                           int /*argc*/,
                           JSValueConst* /*argv*/)
{
  return JS_ThrowTypeError(ctx, "Illegal constructor");
}

void DefineInterface(
    JSContext* ctx, JSValue global, const char* name, JSValue proto, bool set_constructor = true)
{
  // JS_CFUNC_constructor: the constructor is only constructable, so calling
  // it without `new` throws "must be called with new" and `new X()` reaches
  // IllegalConstructor (browsers' "Illegal constructor").
  JSValue ctor = JS_NewCFunction2(ctx, IllegalConstructor, name, 0, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(ctx, ctor, "prototype", JS_DupValue(ctx, proto)); // steals dup
  if (set_constructor) {
    JS_SetPropertyStr(ctx, proto, "constructor", JS_DupValue(ctx, ctor)); // steals dup
  }
  JS_SetPropertyStr(ctx, global, name, ctor); // steals ctor
}

} // namespace

// ---------------------------------------------------------------------------
// Impl definition.
// ---------------------------------------------------------------------------

Impl::Impl(dom::Document& doc) : document(doc)
{
  ctx = static_cast<JSContext*>(ScriptEngineContext(engine));
  if (ctx == nullptr) {
    return;
  }

  JSRuntime* rt = JS_GetRuntime(ctx);
  {
    std::lock_guard<std::mutex> lock(g_class_mutex);
    JS_NewClassID(rt, &g_node_class_id);
    if (g_class_registered.find(rt) == g_class_registered.end()) {
      JSClassDef def;
      std::memset(&def, 0, sizeof(def));
      def.class_name = "Node";
      def.finalizer = &NodeFinalizer;
      JS_NewClass(rt, g_node_class_id, &def);
      g_class_registered.insert(rt);
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_ctx_mutex);
    g_ctx_to_impl[ctx] = this;
  }

  // Prototypes (element/text/... inherit Node).
  node_proto = JS_NewObject(ctx);
  element_proto = JS_NewObjectProto(ctx, node_proto);
  text_proto = JS_NewObjectProto(ctx, node_proto);
  comment_proto = JS_NewObjectProto(ctx, node_proto);
  document_proto = JS_NewObjectProto(ctx, node_proto);
  fragment_proto = JS_NewObjectProto(ctx, node_proto);
  style_proto = JS_NewObject(ctx);

  DefineNodePrototype(ctx, *this);
  DefineElementPrototype(ctx, *this);
  DefineDocumentPrototype(ctx, *this);
  DefineStylePrototype(ctx, *this);

  // Global scope: document, window, timers, DOM interface constructors.
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue doc_wrap = WrapNode(&document);
  JSValue doc_for_window = JS_DupValue(ctx, doc_wrap);
  JS_SetPropertyStr(ctx, global, "document", doc_wrap); // steals doc_wrap
  window = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, window, "document", doc_for_window); // steals

  static const std::array<JSCFunctionListEntry, 4> kTimers = {{
      JS_CFUNC_MAGIC_DEF("setTimeout", 2, TimerCreate, 0),
      JS_CFUNC_MAGIC_DEF("setInterval", 2, TimerCreate, 1),
      JS_CFUNC_MAGIC_DEF("clearTimeout", 1, TimerClear, 0),
      JS_CFUNC_MAGIC_DEF("clearInterval", 1, TimerClear, 1),
  }};
  JS_SetPropertyFunctionList(ctx, window, kTimers.data(), static_cast<int>(kTimers.size()));
  for (const char* name : {"setTimeout", "setInterval", "clearTimeout", "clearInterval"}) {
    JSValue fn = JS_GetPropertyStr(ctx, window, name);
    JS_SetPropertyStr(ctx, global, name, fn); // steals fn
  }

  // Window-level event functions (and bare global aliases) forward to the
  // document, which owns the page's listener storage.
  static const std::array<JSCFunctionListEntry, 3> kWindowEvents = {{
      JS_CFUNC_DEF("addEventListener", 2, WindowAddEventListener),
      JS_CFUNC_DEF("removeEventListener", 2, WindowRemoveEventListener),
      JS_CFUNC_DEF("dispatchEvent", 1, WindowDispatchEvent),
  }};
  JS_SetPropertyFunctionList(
      ctx, window, kWindowEvents.data(), static_cast<int>(kWindowEvents.size()));
  for (const char* name : {"addEventListener", "removeEventListener", "dispatchEvent"}) {
    JSValue fn = JS_GetPropertyStr(ctx, window, name);
    JS_SetPropertyStr(ctx, global, name, fn); // steals fn
  }

  // DOM interface constructors backed by the live prototypes (see
  // DefineInterface above).  HTMLElement shares Element's prototype but does
  // not overwrite Element.prototype.constructor.
  DefineInterface(ctx, global, "Node", node_proto);
  DefineInterface(ctx, global, "Document", document_proto);
  DefineInterface(ctx, global, "Text", text_proto);
  DefineInterface(ctx, global, "Comment", comment_proto);
  DefineInterface(ctx, global, "DocumentFragment", fragment_proto);
  DefineInterface(ctx, global, "CSSStyleDeclaration", style_proto);
  DefineInterface(ctx, global, "Element", element_proto);
  DefineInterface(ctx, global, "HTMLElement", element_proto, /*set_constructor=*/false);

  // navigator: engine identity.  The UA matches what the network stack sends;
  // the rest are documented defaults (the browser UI language is not wired
  // yet).  Exposed on both the global scope and the window.
  JSValue navigator = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, navigator, "userAgent", JS_NewString(ctx, "neko-browser/0.1.0"));
  JS_SetPropertyStr(ctx, navigator, "platform", JS_NewString(ctx, NavigatorPlatform()));
  JS_SetPropertyStr(ctx, navigator, "language", JS_NewString(ctx, "en-US"));
  JSValue languages = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, languages, 0, JS_NewString(ctx, "en-US")); // steals
  JS_SetPropertyStr(ctx, navigator, "languages", languages);           // steals
  JS_SetPropertyStr(ctx, navigator, "onLine", JS_TRUE);
  JS_SetPropertyStr(ctx, navigator, "cookieEnabled", JS_TRUE);
  const unsigned cores = std::thread::hardware_concurrency();
  JS_SetPropertyStr(ctx,
                    navigator,
                    "hardwareConcurrency",
                    JS_NewInt32(ctx, static_cast<int>(std::max(1u, cores))));
  JS_SetPropertyStr(ctx, navigator, "vendor", JS_NewString(ctx, ""));
  JS_SetPropertyStr(ctx, window, "navigator", JS_DupValue(ctx, navigator)); // steals dup
  JS_SetPropertyStr(ctx, global, "navigator", navigator);                   // steals

  // screen: the engine's default viewport (matches renderer::Page's default
  // layout width).  Wiring real window dimensions is future work, so scripts
  // see these defaults.
  JSValue screen = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, screen, "width", JS_NewInt32(ctx, 800));
  JS_SetPropertyStr(ctx, screen, "height", JS_NewInt32(ctx, 600));
  JS_SetPropertyStr(ctx, screen, "availWidth", JS_NewInt32(ctx, 800));
  JS_SetPropertyStr(ctx, screen, "availHeight", JS_NewInt32(ctx, 600));
  JS_SetPropertyStr(ctx, screen, "colorDepth", JS_NewInt32(ctx, 24));
  JS_SetPropertyStr(ctx, screen, "pixelDepth", JS_NewInt32(ctx, 24));
  JS_SetPropertyStr(ctx, window, "screen", JS_DupValue(ctx, screen)); // steals dup
  JS_SetPropertyStr(ctx, global, "screen", screen);                   // steals

  // Window viewport geometry (engine defaults, see screen above).
  JS_SetPropertyStr(ctx, window, "innerWidth", JS_NewInt32(ctx, 800));
  JS_SetPropertyStr(ctx, window, "innerHeight", JS_NewInt32(ctx, 600));
  JS_SetPropertyStr(ctx, window, "devicePixelRatio", JS_NewInt32(ctx, 1));

  JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, window)); // steals
  JS_FreeValue(ctx, global);
}

Impl::~Impl()
{
  if (ctx == nullptr) {
    return;
  }
  JSRuntime* rt = JS_GetRuntime(ctx);

  // Detach every live wrapper so no JS object can dereference a freed node.
  for (const auto& entry : wrappers) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(entry.second, g_node_class_id));
    if (w != nullptr) {
      w->node = nullptr;
    }
  }
  // Drop the registry's own references (the runtime is destroyed later, in
  // ~ScriptEngine, after this destructor body).
  for (const auto& entry : wrappers) {
    JS_FreeValue(ctx, entry.second);
  }
  wrappers.clear();

  // Free listener callbacks.
  for (auto& entry : listeners) {
    for (auto& type_entry : entry.second) {
      for (JSValue cb : type_entry.second) {
        JS_FreeValue(ctx, cb);
      }
    }
  }
  listeners.clear();

  // Free timer callbacks.
  for (Timer& timer : timers) {
    JS_FreeValue(ctx, timer.callback);
  }
  timers.clear();

  // Release the global object's references to the objects we installed so the
  // GC below can collect and finalize them (reachable objects would otherwise
  // be torn down by the runtime without running their finalizers, leaking the
  // NodeWrapper payloads).
  JSValue global = JS_GetGlobalObject(ctx);
  for (const char* name : {"document",
                           "window",
                           "setTimeout",
                           "setInterval",
                           "clearTimeout",
                           "clearInterval",
                           "addEventListener",
                           "removeEventListener",
                           "dispatchEvent",
                           "Node",
                           "Document",
                           "Text",
                           "Comment",
                           "DocumentFragment",
                           "CSSStyleDeclaration",
                           "Element",
                           "HTMLElement",
                           "navigator",
                           "screen"}) {
    JSAtom atom = JS_NewAtom(ctx, name);
    JS_DeleteProperty(ctx, global, atom, JS_PROP_THROW);
    JS_FreeAtom(ctx, atom);
  }
  JS_FreeValue(ctx, global);
  // The window object also holds a reference to document.
  if (JS_IsObject(window)) {
    JSAtom atom = JS_NewAtom(ctx, "document");
    JS_DeleteProperty(ctx, window, atom, JS_PROP_THROW);
    JS_FreeAtom(ctx, atom);
  }

  // Free prototypes and window (own references).
  JS_FreeValue(ctx, node_proto);
  JS_FreeValue(ctx, element_proto);
  JS_FreeValue(ctx, text_proto);
  JS_FreeValue(ctx, comment_proto);
  JS_FreeValue(ctx, document_proto);
  JS_FreeValue(ctx, fragment_proto);
  JS_FreeValue(ctx, style_proto);
  JS_FreeValue(ctx, window);

  // Run the GC so the (now unreachable) wrappers are finalized while the
  // runtime is still alive; this reclaims the NodeWrapper payloads and the
  // QuickJS arena memory instead of leaking them at runtime teardown.
  JS_RunGC(rt);

  {
    std::lock_guard<std::mutex> lock(g_ctx_mutex);
    g_ctx_to_impl.erase(ctx);
  }
  // Forget the runtime in the class-registration set: a future runtime
  // allocated at the same address must re-register the class, otherwise
  // JS_NewObjectClass would use an unregistered class id.
  {
    std::lock_guard<std::mutex> lock(g_class_mutex);
    g_class_registered.erase(rt);
  }

  // Free owned C++ nodes.  Wrappers were detached and finalized above, so
  // nothing can reach them; ~ScriptEngine (member, destroyed after this body)
  // tears down the runtime.
  created.clear();
  retained.clear();
}

JSValue Impl::WrapNode(dom::Node* node)
{
  if (node == nullptr) {
    return JS_NULL;
  }
  const auto it = wrappers.find(node);
  if (it != wrappers.end()) {
    return JS_DupValue(ctx, it->second);
  }
  auto* w = new NodeWrapper{this, node};
  JSValue obj = JS_NewObjectClass(ctx, g_node_class_id);
  JS_SetOpaque(obj, w);
  JS_SetPrototype(ctx, obj, PrototypeFor(node));
  JSValue dup = JS_DupValue(ctx, obj);
  wrappers[node] = dup;
  return obj; // one owned reference for the caller
}

JSValue Impl::PrototypeFor(const dom::Node* node) const
{
  switch (node->node_type()) {
  case dom::NodeType::kDocument:
    return document_proto;
  case dom::NodeType::kElement:
    return element_proto;
  case dom::NodeType::kText:
    return text_proto;
  case dom::NodeType::kComment:
    return comment_proto;
  case dom::NodeType::kDocumentFragment:
    return fragment_proto;
  }
  return node_proto;
}

JSValue Impl::MakeNodeArray(const std::vector<dom::Node*>& nodes)
{
  JSValue arr = JS_NewArray(ctx);
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    JSValue w = WrapNode(nodes[i]);
    JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), w); // steals w
  }
  return arr;
}

JSValue Impl::MakeElementArray(const std::vector<dom::Element*>& elements)
{
  JSValue arr = JS_NewArray(ctx);
  for (std::size_t i = 0; i < elements.size(); ++i) {
    JSValue w = WrapNode(elements[i]);
    JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), w); // steals w
  }
  return arr;
}

void Impl::TakeOwnership(dom::Node* node, std::unique_ptr<dom::Node> owned)
{
  if (owned == nullptr) {
    return;
  }
  // The node should not already be tracked; if it is (shouldn't happen), the
  // new owner replaces the old one.
  created.erase(node);
  retained.push_back(std::move(owned));
}

std::unique_ptr<dom::Node> Impl::ReleaseOwned(dom::Node* node)
{
  const auto created_it = created.find(node);
  if (created_it != created.end()) {
    std::unique_ptr<dom::Node> owned = std::move(created_it->second);
    created.erase(created_it);
    return owned;
  }
  for (auto it = retained.begin(); it != retained.end(); ++it) {
    if (it->get() == node) {
      std::unique_ptr<dom::Node> owned = std::move(*it);
      retained.erase(it);
      return owned;
    }
  }
  return nullptr;
}

int Impl::RunPendingTimers()
{
  const auto now = std::chrono::steady_clock::now();
  // Snapshot the due timers before running any callback: a callback may add
  // or clear timers, which would invalidate iterators.
  std::vector<Timer> due;
  for (auto it = timers.begin(); it != timers.end();) {
    if (it->due <= now) {
      Timer snapshot;
      snapshot.id = it->id;
      snapshot.repeating = it->repeating;
      snapshot.interval = it->interval;
      snapshot.due = it->due;
      snapshot.callback = JS_DupValue(ctx, it->callback); // extra ref for the snapshot
      due.push_back(snapshot); // Timer is trivially copyable; the copy owns the snapshot ref
      if (it->repeating) {
        it->due += it->interval; // accumulate to avoid drift
        ++it;
      } else {
        // Timer has no destructor: release the callback reference before the
        // slot is destroyed, otherwise the callback leaks.
        JS_FreeValue(ctx, it->callback);
        it = timers.erase(it);
      }
    } else {
      ++it;
    }
  }
  int ran = 0;
  for (Timer& timer : due) {
    JSValue result = JS_Call(ctx, timer.callback, JS_UNDEFINED, 0, nullptr);
    if (JS_IsException(result)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
    } else {
      JS_FreeValue(ctx, result);
    }
    JS_FreeValue(ctx, timer.callback);
    // Promise continuations created by the callback make progress before the
    // next timer runs.
    engine.RunPendingJobs();
    ++ran;
  }
  return ran;
}

std::optional<std::chrono::steady_clock::time_point> Impl::NextTimerDeadline() const
{
  std::optional<std::chrono::steady_clock::time_point> next;
  for (const Timer& timer : timers) {
    if (!next.has_value() || timer.due < next.value()) {
      next = timer.due;
    }
  }
  return next;
}

// Shared dispatch: runs the listeners registered on |node| for |type| with a
// fresh event object (no bubbling).
void Impl::DispatchToNode(dom::Node* node, std::string_view type)
{
  const auto el_it = listeners.find(node);
  if (el_it == listeners.end()) {
    return;
  }
  const std::string key(type);
  const auto type_it = el_it->second.find(key);
  if (type_it == el_it->second.end()) {
    return;
  }
  std::vector<JSValue> callbacks;
  callbacks.reserve(type_it->second.size());
  for (JSValue cb : type_it->second) {
    callbacks.push_back(JS_DupValue(ctx, cb));
  }
  JSValue event = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, event, "type", JS_NewStringLen(ctx, type.data(), type.size()));
  JSValue target = WrapNode(node);
  JS_SetPropertyStr(ctx, event, "target", JS_DupValue(ctx, target));
  JS_SetPropertyStr(ctx, event, "currentTarget", JS_DupValue(ctx, target));
  JS_FreeValue(ctx, target);
  JSValue this_wrap = WrapNode(node);
  for (JSValue cb : callbacks) {
    JSValue result = JS_Call(ctx, cb, this_wrap, 1, &event);
    if (JS_IsException(result)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
    } else {
      JS_FreeValue(ctx, result);
    }
    JS_FreeValue(ctx, cb);
  }
  // Promise continuations created by the listeners make progress.
  engine.RunPendingJobs();
  JS_FreeValue(ctx, this_wrap);
  JS_FreeValue(ctx, event);
}

void Impl::DispatchEvent(dom::Element& element, std::string_view type)
{
  DispatchToNode(&element, type);
}

void Impl::DispatchDocumentEvent(std::string_view type)
{
  DispatchToNode(&document, type);
}

// ---------------------------------------------------------------------------
// DomBinder (public API).
// ---------------------------------------------------------------------------

DomBinder::DomBinder(dom::Document& document) : impl_(std::make_unique<Impl>(document)) {}

DomBinder::~DomBinder() = default;

base::Result<ScriptValue> DomBinder::Evaluate(std::string_view source, std::string_view filename)
{
  return impl_->engine.Evaluate(source, filename);
}

void DomBinder::SetConsoleSink(ScriptEngine::ConsoleSink sink)
{
  impl_->engine.SetConsoleSink(std::move(sink));
}

int DomBinder::RunPendingTimers()
{
  return impl_->RunPendingTimers();
}

std::optional<std::chrono::steady_clock::time_point> DomBinder::NextTimerDeadline() const
{
  return impl_->NextTimerDeadline();
}

void DomBinder::DispatchEvent(dom::Element& element, std::string_view type)
{
  impl_->DispatchEvent(element, type);
}

void DomBinder::DispatchDocumentEvent(std::string_view type)
{
  impl_->DispatchDocumentEvent(type);
}

ScriptEngine& DomBinder::engine()
{
  return impl_->engine;
}

dom::Document& DomBinder::document() const
{
  return impl_->document;
}

} // namespace neko::javascript
