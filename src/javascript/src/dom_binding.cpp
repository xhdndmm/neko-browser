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
#include "neko/base/version.h"
#include "neko/css/parser.h"
#include "neko/dom/query.h"
#include "neko/html/parser.h"
#include "neko/javascript/script_engine_internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
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

// Opaque state attached to every Event object (new Event(...) or the fresh
// event created by a dispatch).  target/currentTarget are owned JSValues
// (Dup'd node wrappers) stored here rather than as JS properties: the
// prototype's target/currentTarget getters must not read the same-named
// property (that would recurse), and storing them on the wrapper keeps the
// dispatch state in one place.
struct EventWrapper
{
  Impl* impl = nullptr;
  std::string type;
  bool bubbles = false;
  bool cancelable = false;
  bool default_prevented = false;
  bool propagation_stopped = false;
  bool immediate_stopped = false;
  int event_phase = 0;                   // 0 none, 1 capture, 2 target, 3 bubble
  JSValue target = JS_UNDEFINED;         // owned node wrapper (or undefined)
  JSValue current_target = JS_UNDEFINED; // owned node wrapper (or undefined)
};

JSClassID g_event_class_id = 0;
std::mutex g_event_class_mutex;
std::unordered_set<JSRuntime*> g_event_class_registered;

void EventFinalizer(JSRuntime* rt, JSValue obj)
{
  auto* w = static_cast<EventWrapper*>(JS_GetOpaque(obj, g_event_class_id));
  if (w != nullptr) {
    JS_FreeValueRT(rt, w->target);
    JS_FreeValueRT(rt, w->current_target);
    delete w;
  }
}

// The EventWrapper holds owned JSValues (target/current_target) in its opaque
// payload.  The GC cannot see those unless the class's gc_mark callback marks
// them; without it a live event keeps its target reachable only through a
// refcount that the GC treats as an external reference, so at runtime teardown
// the target (e.g. the document wrapper) is never collected and JS_FreeRuntime
// fails its "gc_obj_list is empty" assertion.
void EventGcMark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
  auto* w = static_cast<EventWrapper*>(JS_GetOpaque(val, g_event_class_id));
  if (w == nullptr) {
    return;
  }
  JS_MarkValue(rt, w->target, mark_func);
  JS_MarkValue(rt, w->current_target, mark_func);
}

// Forward declarations (defined below with the other node/string helpers).
std::string ArgString(JSContext* ctx, JSValueConst value, bool* ok);
dom::Element* AsElement(dom::Node* node);

// Dataset objects (element.dataset): a class whose exotic get/set property
// handlers map camelCase keys to the element's data-* attributes, so both
// reads (el.dataset.foo) and writes (el.dataset.fooBar = 'x') round-trip
// through the attribute list without needing per-property accessors.
JSClassID g_dataset_class_id = 0;
std::mutex g_dataset_class_mutex;
std::unordered_set<JSRuntime*> g_dataset_class_registered;
JSClassExoticMethods g_dataset_exotic = {};

void DatasetFinalizer(JSRuntime* /*rt*/, JSValue obj)
{
  auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(obj, g_dataset_class_id));
  delete w;
}

// "fooBar" -> "data-foo-bar".
std::string DatasetKeyToAttr(const std::string& key)
{
  std::string attr = "data-";
  for (const char c : key) {
    if (c >= 'A' && c <= 'Z') {
      attr.push_back('-');
      attr.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    } else {
      attr.push_back(c);
    }
  }
  return attr;
}

// "data-foo-bar" -> "fooBar".
std::string DatasetAttrToKey(const std::string& attr)
{
  std::string key;
  bool upper = false;
  for (std::size_t i = 5; i < attr.size(); ++i) { // skip "data-"
    const char c = attr[i];
    if (c == '-') {
      upper = true;
      continue;
    }
    key.push_back(upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
    upper = false;
  }
  return key;
}

JSValue DatasetGetProperty(JSContext* ctx, JSValueConst obj, JSAtom atom, JSValueConst /*receiver*/)
{
  auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(obj, g_dataset_class_id));
  dom::Element* el = w != nullptr ? AsElement(w->node) : nullptr;
  if (el == nullptr) {
    return JS_UNDEFINED;
  }
  const char* name = JS_AtomToCString(ctx, atom);
  if (name == nullptr) {
    return JS_UNDEFINED;
  }
  const std::string attr = DatasetKeyToAttr(name);
  JS_FreeCString(ctx, name);
  const std::optional<std::string_view> value = el->GetAttribute(attr);
  return value.has_value() ? JS_NewStringLen(ctx, value->data(), value->size()) : JS_UNDEFINED;
}

int DatasetSetProperty(JSContext* ctx,
                       JSValueConst obj,
                       JSAtom atom,
                       JSValueConst value,
                       JSValueConst /*receiver*/,
                       int /*flags*/)
{
  auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(obj, g_dataset_class_id));
  dom::Element* el = w != nullptr ? AsElement(w->node) : nullptr;
  if (el == nullptr) {
    return 0;
  }
  const char* name = JS_AtomToCString(ctx, atom);
  if (name == nullptr) {
    return 0;
  }
  const std::string attr = DatasetKeyToAttr(name);
  JS_FreeCString(ctx, name);
  bool ok = false;
  const std::string value_str = ArgString(ctx, value, &ok);
  if (!ok) {
    return 0; // exception pending
  }
  if (value_str.empty()) {
    el->RemoveAttribute(attr);
  } else {
    el->SetAttribute(attr, value_str);
  }
  return 1;
}

// Returns the element's data-* attributes as an object keyed by camelCase
// (used by get_own_property_names so Object.keys / JSON see them).
int DatasetGetOwnPropertyNames(JSContext* ctx,
                               JSPropertyEnum** ptab,
                               uint32_t* plen,
                               JSValueConst obj)
{
  auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(obj, g_dataset_class_id));
  dom::Element* el = w != nullptr ? AsElement(w->node) : nullptr;
  if (el == nullptr) {
    *ptab = nullptr;
    *plen = 0;
    return 0;
  }
  std::vector<std::string> keys;
  for (const dom::Attribute& attr : el->attributes()) {
    if (attr.name.rfind("data-", 0) == 0) {
      keys.push_back(DatasetAttrToKey(attr.name));
    }
  }
  JSPropertyEnum* table =
      static_cast<JSPropertyEnum*>(js_malloc(ctx, keys.size() * sizeof(JSPropertyEnum)));
  if (keys.empty()) {
    *ptab = table;
    *plen = 0;
    return 0;
  }
  if (table == nullptr) {
    return -1;
  }
  for (std::size_t i = 0; i < keys.size(); ++i) {
    table[i].atom = JS_NewAtomLen(ctx, keys[i].data(), keys[i].size());
    table[i].is_enumerable = 1;
  }
  *ptab = table;
  *plen = static_cast<uint32_t>(keys.size());
  return 0;
}

// Forward declarations (defined below with the other node/string helpers).
EventWrapper* UnwrapEvent(JSValueConst value)
{
  return static_cast<EventWrapper*>(JS_GetOpaque(value, g_event_class_id));
}

// Reads the 'type' of an event-like value: the opaque wrapper's type for a
// real Event, else the JS 'type' property (for plain {type: ...} objects,
// which dispatchEvent still accepts as a shortcut).
bool EventTypeOf(JSContext* ctx, JSValueConst value, std::string* out)
{
  if (EventWrapper* w = UnwrapEvent(value); w != nullptr) {
    *out = w->type;
    return true;
  }
  JSValue type_val = JS_GetPropertyStr(ctx, value, "type");
  bool ok = false;
  *out = ArgString(ctx, type_val, &ok);
  JS_FreeValue(ctx, type_val);
  return ok;
}

// True when |value| is a real Event object.
bool IsEvent(JSValueConst value)
{
  return UnwrapEvent(value) != nullptr;
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
  JSValue event_proto = JS_UNDEFINED;
  JSValue custom_event_proto = JS_UNDEFINED;
  JSValue class_list_proto = JS_UNDEFINED;
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

  // requestAnimationFrame callbacks.  Queued during a script run; the pump
  // (RunPendingTimers) moves them to |raf_pending| and invokes each with a
  // timestamp.  Callbacks are Dup'd.
  struct RafEntry
  {
    int64_t id = 0;
    JSValue callback = JS_UNDEFINED;
  };
  std::vector<RafEntry> raf_queue;
  std::vector<RafEntry> raf_pending;
  int64_t next_raf_id = 1;

  // performance.now() origin (steady clock at binder construction).
  std::chrono::steady_clock::time_point performance_origin = std::chrono::steady_clock::now();
  // performance.timing.navigationStart (wall-clock epoch ms at construction).
  double navigation_start_epoch_ms = 0.0;

  // Event listeners: node -> (type -> listeners).  Keyed by node (elements
  // and the document; window-level listeners are stored under the document,
  // since the window and the document are the same event target here).
  // Callbacks are Dup'd; nodes stay alive through |wrappers|/|retained| for
  // the binder's life.  Capture/once options are honored by dispatch.
  struct Listener
  {
    JSValue callback = JS_UNDEFINED;
    bool capture = false;
    bool once = false;
  };
  using ListenerMap = std::unordered_map<std::string, std::vector<Listener>>;
  std::unordered_map<const dom::Node*, ListenerMap> listeners;

  // Optional browser Web APIs (localStorage/fetch) wired by the browser layer.
  PageApis apis;

  explicit Impl(dom::Document& doc, const PageApis& page_apis);
  ~Impl();

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  JSValue WrapNode(dom::Node* node);
  JSValue PrototypeFor(const dom::Node* node) const;
  JSValue MakeNodeArray(const std::vector<dom::Node*>& nodes);
  JSValue MakeElementArray(const std::vector<dom::Element*>& elements);

  // Creates an Event object (class wrapper + event_proto prototype).
  JSValue MakeEvent(std::string type, bool bubbles, bool cancelable);
  // Runs one event through capture -> target -> bubble propagation over the
  // ancestor path of |target|.  |event| must be an Event object (the opaque
  // wrapper's state is mutated).  Listeners run synchronously; the event's
  // target/currentTarget/eventPhase are updated along the way.  Returns false
  // when the event was canceled (dispatchEvent must return false then).
  bool DispatchPropagated(dom::Node* target, JSValue event);

  void TakeOwnership(dom::Node* node, std::unique_ptr<dom::Node> owned);
  std::unique_ptr<dom::Node> ReleaseOwned(dom::Node* node);

  int RunPendingTimers();
  int RunPendingRaf();
  std::optional<std::chrono::steady_clock::time_point> NextTimerDeadline() const;
  void DispatchToNode(dom::Node* node, std::string_view type);
  void DispatchEvent(dom::Element& element, std::string_view type);
  void DispatchDocumentEvent(std::string_view type);
};

namespace {

// ---------------------------------------------------------------------------
// Node methods and accessors.
// ---------------------------------------------------------------------------

// Throws a DOMException with the given WebIDL exception name (DOM spec §4.4,
// WebIDL §3.5).  DOM tree operations report NotFoundError, HierarchyRequestError,
// InvalidNodeTypeError, etc. rather than a plain TypeError.
JSValue ThrowDomException(JSContext* ctx, std::string_view name, std::string_view message)
{
  JSValue err = JS_NewError(ctx);
  JS_SetPropertyStr(ctx, err, "name", JS_NewStringLen(ctx, name.data(), name.size()));
  JS_SetPropertyStr(ctx, err, "message", JS_NewStringLen(ctx, message.data(), message.size()));
  // Legacy numeric code (DOMException §6): constants of the exception names
  // used by the DOM APIs implemented here.
  auto legacy_code = [](std::string_view n) -> int {
    if (n == "IndexSizeError")
      return 1;
    if (n == "HierarchyRequestError")
      return 3;
    if (n == "WrongDocumentError")
      return 4;
    if (n == "InvalidCharacterError")
      return 5;
    if (n == "NoModificationAllowedError")
      return 7;
    if (n == "NotFoundError")
      return 8;
    if (n == "NotSupportedError")
      return 9;
    if (n == "InvalidStateError")
      return 11;
    if (n == "SyntaxError")
      return 12;
    if (n == "InvalidModificationError")
      return 13;
    if (n == "NamespaceError")
      return 14;
    if (n == "InvalidNodeTypeError")
      return 24;
    if (n == "InvalidAccessError")
      return 15;
    return 0;
  };
  JS_SetPropertyStr(ctx, err, "code", JS_NewInt32(ctx, legacy_code(name)));
  return JS_Throw(ctx, err);
}

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
    return ThrowDomException(
        ctx, "NotFoundError", "insertBefore: reference is not a child of this node");
  }
  if (child == parent || child->node_type() == dom::NodeType::kDocument) {
    return ThrowDomException(ctx, "HierarchyRequestError", "insertBefore: invalid node");
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
    return ThrowDomException(
        ctx, "NotFoundError", "removeChild: argument is not a child of this node");
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
    return JS_UNDEFINED;
  }
  if (node->node_type() == dom::NodeType::kComment) {
    static_cast<dom::Comment*>(node)->SetData(text);
    return JS_UNDEFINED;
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

JSValue ElementGetLastElementChild(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  dom::Element* found = nullptr;
  for (dom::Node* child : element->ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      found = el;
    }
  }
  return impl->WrapNode(found);
}

// The adjacent element sibling of |element| in the given direction.
dom::Element* ElementSiblingOf(dom::Element* element, int offset)
{
  if (element == nullptr || element->parent() == nullptr) {
    return nullptr;
  }
  dom::Node* parent = element->parent();
  dom::Element* prev = nullptr;
  bool seen = false;
  for (dom::Node* child : parent->ChildNodes()) {
    dom::Element* el = AsElement(child);
    if (el == nullptr) {
      continue;
    }
    if (el == element) {
      seen = true;
      if (offset < 0) {
        return prev;
      }
    } else if (seen && offset > 0) {
      return el;
    } else if (!seen) {
      prev = el;
    }
  }
  return nullptr;
}

JSValue ElementGetNextElementSibling(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return impl->WrapNode(ElementSiblingOf(element, +1));
}

JSValue ElementGetPreviousElementSibling(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return impl->WrapNode(ElementSiblingOf(element, -1));
}

JSValue ElementGetDataset(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // A dataset object per access; its exotic property handlers read/write the
  // element's data-* attributes through the class's NodeWrapper opaque.
  auto* w = new NodeWrapper{impl, element};
  JSValue dataset = JS_NewObjectClass(ctx, g_dataset_class_id);
  JS_SetOpaque(dataset, w);
  return dataset;
}

JSValue ElementMatches(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "matches: not an element");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "matches requires a selector");
  }
  bool ok = false;
  const std::string selector = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  return JS_NewBool(ctx, dom::MatchesCompoundSelector(*element, selector));
}

JSValue ElementClosest(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "closest: not an element");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "closest requires a selector");
  }
  bool ok = false;
  const std::string selector = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  for (dom::Element* e = element; e != nullptr; e = AsElement(e->parent())) {
    if (dom::MatchesCompoundSelector(*e, selector)) {
      return impl->WrapNode(e);
    }
  }
  return JS_NULL;
}

JSValue ElementRemove(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr || node->parent() == nullptr) {
    return JS_UNDEFINED;
  }
  std::unique_ptr<dom::Node> removed = node->parent()->RemoveChild(node);
  impl->TakeOwnership(node, std::move(removed));
  return JS_UNDEFINED;
}

// DOMRect.toJSON(): a plain object with the rect fields (numbers).
JSValue RectToJson(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  JSValue out = JS_NewObject(ctx);
  for (const char* name : {"x", "y", "left", "top", "right", "bottom", "width", "height"}) {
    JSValue v = JS_GetPropertyStr(ctx, this_val, name);
    JS_SetPropertyStr(ctx, out, name, v); // steals v
  }
  return out;
}

JSValue ElementGetBoundingClientRect(JSContext* ctx,
                                     JSValueConst this_val,
                                     int /*argc*/,
                                     JSValueConst* /*argv*/)
{
  if (AsElement(UnwrapNode(this_val)) == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // The binder has no layout tree, so geometry is a documented approximation:
  // a zero rect at the origin.  Scripts that branch on visibility still work
  // (0 is falsy for width/height), but pixel positions are not meaningful.
  JSValue rect = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, rect, "x", JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, rect, "y", JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, rect, "left", JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, rect, "top", JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, rect, "right", JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, rect, "bottom", JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, rect, "width", JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, rect, "height", JS_NewInt32(ctx, 0));
  JSValue to_json = JS_NewCFunction(ctx, RectToJson, "toJSON", 0);
  JS_SetPropertyStr(ctx, rect, "toJSON", to_json); // steals to_json
  return rect;
}

JSValue ElementGetHidden(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewBool(ctx, element->HasAttribute("hidden"));
}

JSValue ElementSetHidden(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const int v = JS_ToBool(ctx, value);
  if (v < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }
  if (v != 0) {
    element->SetAttribute("hidden", "");
  } else {
    element->RemoveAttribute("hidden");
  }
  return JS_UNDEFINED;
}

JSValue ElementGetTitle(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::optional<std::string_view> title = element->GetAttribute("title");
  return title.has_value() ? JS_NewStringLen(ctx, title->data(), title->size())
                           : JS_NewStringLen(ctx, "", 0);
}

JSValue ElementSetTitle(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string title = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (title.empty()) {
    element->RemoveAttribute("title");
  } else {
    element->SetAttribute("title", title);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetLang(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::optional<std::string_view> lang = element->GetAttribute("lang");
  return lang.has_value() ? JS_NewStringLen(ctx, lang->data(), lang->size())
                          : JS_NewStringLen(ctx, "", 0);
}

JSValue ElementSetLang(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string lang = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (lang.empty()) {
    element->RemoveAttribute("lang");
  } else {
    element->SetAttribute("lang", lang);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetOuterHTML(JSContext* ctx, JSValueConst this_val)
{
  dom::Node* node = UnwrapNode(this_val);
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  const std::string out = node->ToString();
  return JS_NewStringLen(ctx, out.data(), out.size());
}

// ---------------------------------------------------------------------------
// Form controls / links / images.
//
// value/checked/type/placeholder/disabled/name apply to form controls
// (input/textarea/select/option); href/target/rel to <a>; src/alt/width/
// height/natural*/complete to <img> (and src to <script>).  They live on the
// Element prototype and dispatch on the element's tag name.
// ---------------------------------------------------------------------------

// The raw attribute value, or empty string.
std::string RawAttr(const dom::Element& element, std::string_view name)
{
  const std::optional<std::string_view> value = element.GetAttribute(name);
  return value.has_value() ? std::string(value.value()) : std::string();
}

// The absolute URL for an href/src attribute (resolved against the page base
// via the PageApis callback when wired; otherwise the raw attribute).
std::string ResolvedUrl(const Impl& impl, const std::string& raw)
{
  if (impl.apis.resolve_url && !raw.empty()) {
    const std::string resolved = impl.apis.resolve_url(raw);
    if (!resolved.empty()) {
      return resolved;
    }
  }
  return raw;
}

bool IsFormControl(const dom::Element& element)
{
  const std::string_view tag = element.tag_name();
  return tag == "input" || tag == "textarea" || tag == "select" || tag == "option" ||
         tag == "button";
}

bool IsCheckableInput(const dom::Element& element)
{
  if (element.tag_name() != "input") {
    return false;
  }
  const std::string type = ToLower(RawAttr(element, "type"));
  return type == "checkbox" || type == "radio";
}

JSValue ElementGetValue(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string_view tag = element->tag_name();
  if (tag == "textarea") {
    // value == the text content (textContent; <br> not meaningful here).
    const std::string text = element->TextContent();
    return JS_NewStringLen(ctx, text.data(), text.size());
  }
  if (tag == "option") {
    // option.value: the value attribute, else the text content.
    const std::string attr = RawAttr(*element, "value");
    if (!attr.empty()) {
      return JS_NewStringLen(ctx, attr.data(), attr.size());
    }
    const std::string text = element->TextContent();
    return JS_NewStringLen(ctx, text.data(), text.size());
  }
  const std::string value = RawAttr(*element, "value");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetValue(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (element->tag_name() == "textarea") {
    // Setting value on a textarea replaces its text child.  RemoveChild
    // returns ownership; keep the removed subtree alive in the binder's
    // retained set so JS references to the old children do not dangle.
    Impl* impl = ImplFor(ctx, this_val);
    while (element->first_child() != nullptr) {
      dom::Node* child = element->first_child();
      std::unique_ptr<dom::Node> removed = element->RemoveChild(child);
      if (impl != nullptr) {
        impl->TakeOwnership(child, std::move(removed));
      }
    }
    if (!text.empty()) {
      element->AppendChild(std::make_unique<dom::Text>(text));
    }
  } else {
    element->SetAttribute("value", text);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetChecked(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr || !IsCheckableInput(*element)) {
    return JS_NewBool(ctx, false);
  }
  return JS_NewBool(ctx, element->HasAttribute("checked"));
}

JSValue ElementSetChecked(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr || !IsCheckableInput(*element)) {
    return JS_UNDEFINED;
  }
  const int v = JS_ToBool(ctx, value);
  if (v < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }
  if (v != 0) {
    element->SetAttribute("checked", "checked");
  } else {
    element->RemoveAttribute("checked");
  }
  return JS_UNDEFINED;
}

JSValue ElementGetType(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (element->tag_name() != "input") {
    return JS_NewStringLen(ctx, "", 0);
  }
  const std::string type = RawAttr(*element, "type");
  return JS_NewStringLen(ctx, type.data(), type.size());
}

JSValue ElementSetType(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string type = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (element->tag_name() == "input") {
    element->SetAttribute("type", type);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetPlaceholder(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string value = RawAttr(*element, "placeholder");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetPlaceholder(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("placeholder", text);
  return JS_UNDEFINED;
}

JSValue ElementGetDisabled(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewBool(ctx, element->HasAttribute("disabled"));
}

JSValue ElementSetDisabled(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const int v = JS_ToBool(ctx, value);
  if (v < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }
  if (v != 0) {
    element->SetAttribute("disabled", "disabled");
  } else {
    element->RemoveAttribute("disabled");
  }
  return JS_UNDEFINED;
}

JSValue ElementGetName(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string value = RawAttr(*element, "name");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetName(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("name", text);
  return JS_UNDEFINED;
}

JSValue ElementGetHref(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string raw = RawAttr(*element, "href");
  const std::string resolved = ResolvedUrl(*impl, raw);
  return JS_NewStringLen(ctx, resolved.data(), resolved.size());
}

JSValue ElementSetHref(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string href = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("href", href);
  return JS_UNDEFINED;
}

JSValue ElementGetTarget(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string value = RawAttr(*element, "target");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetTarget(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("target", text);
  return JS_UNDEFINED;
}

JSValue ElementGetRel(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string value = RawAttr(*element, "rel");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetRel(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("rel", text);
  return JS_UNDEFINED;
}

JSValue ElementGetSrc(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string raw = RawAttr(*element, "src");
  const std::string resolved = ResolvedUrl(*impl, raw);
  return JS_NewStringLen(ctx, resolved.data(), resolved.size());
}

JSValue ElementSetSrc(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string src = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("src", src);
  return JS_UNDEFINED;
}

JSValue ElementGetAlt(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string value = RawAttr(*element, "alt");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetAlt(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("alt", text);
  return JS_UNDEFINED;
}

// Parses a non-negative integer attribute (for img width/height).
int64_t PositiveIntAttr(const dom::Element& element, std::string_view name)
{
  const std::string value = RawAttr(element, name);
  if (value.empty()) {
    return 0;
  }
  int64_t out = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') {
      return 0;
    }
    out = out * 10 + (c - '0');
  }
  return out;
}

JSValue ElementGetWidth(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewInt64(ctx, PositiveIntAttr(*element, "width"));
}

JSValue ElementSetWidth(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  int32_t v = 0;
  if (JS_ToInt32(ctx, &v, value) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }
  if (v < 0) {
    v = 0;
  }
  element->SetAttribute("width", std::to_string(v));
  return JS_UNDEFINED;
}

JSValue ElementGetHeight(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewInt64(ctx, PositiveIntAttr(*element, "height"));
}

JSValue ElementSetHeight(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  int32_t v = 0;
  if (JS_ToInt32(ctx, &v, value) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }
  if (v < 0) {
    v = 0;
  }
  element->SetAttribute("height", std::to_string(v));
  return JS_UNDEFINED;
}

JSValue ElementGetNaturalWidth(JSContext* ctx, JSValueConst this_val)
{
  if (AsElement(UnwrapNode(this_val)) == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // The binder does not track decoded image dimensions; 0 (a documented
  // approximation — scripts that check naturalWidth before drawing a layout
  // still branch correctly).
  return JS_NewInt32(ctx, 0);
}

JSValue ElementGetNaturalHeight(JSContext* ctx, JSValueConst this_val)
{
  if (AsElement(UnwrapNode(this_val)) == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewInt32(ctx, 0);
}

JSValue ElementGetComplete(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // complete: true once a src is present (loading is synchronous here).
  return JS_NewBool(ctx, element->HasAttribute("src"));
}

JSValue ElementGetCurrentSrc(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string raw = RawAttr(*element, "src");
  const std::string resolved = ResolvedUrl(*impl, raw);
  return JS_NewStringLen(ctx, resolved.data(), resolved.size());
}

JSValue ElementGetText(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (element->tag_name() != "option") {
    return JS_NewStringLen(ctx, "", 0);
  }
  const std::string text = element->TextContent();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

// ---------------------------------------------------------------------------
// classList (DOMTokenList subset).
//
// A classList object is backed by the element's class attribute.  add/remove/
// toggle/contains/replace operate on the whitespace-separated token set and
// persist to the attribute; item/length/toString expose the current tokens.
// ---------------------------------------------------------------------------

std::vector<std::string> ClassTokens(const dom::Element& element)
{
  std::vector<std::string> out;
  const std::optional<std::string_view> cls = element.GetAttribute("class");
  if (!cls.has_value()) {
    return out;
  }
  std::string current;
  auto flush = [&]() {
    if (!current.empty()) {
      out.push_back(current);
      current.clear();
    }
  };
  for (const char c : cls.value()) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      flush();
    } else {
      current.push_back(c);
    }
  }
  flush();
  return out;
}

void SetClassTokens(dom::Element& element, const std::vector<std::string>& tokens)
{
  std::string out;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (i > 0) {
      out += ' ';
    }
    out += tokens[i];
  }
  if (out.empty()) {
    element.RemoveAttribute("class");
  } else {
    element.SetAttribute("class", out);
  }
}

JSValue ClassListAdd(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "classList: not an element");
  }
  std::vector<std::string> tokens = ClassTokens(*element);
  for (int i = 0; i < argc; ++i) {
    bool ok = false;
    const std::string token = ArgString(ctx, argv[i], &ok);
    if (!ok) {
      return JS_EXCEPTION;
    }
    if (token.empty()) {
      continue;
    }
    bool present = false;
    for (const std::string& t : tokens) {
      if (t == token) {
        present = true;
        break;
      }
    }
    if (!present) {
      tokens.push_back(token);
    }
  }
  SetClassTokens(*element, tokens);
  return JS_UNDEFINED;
}

JSValue ClassListRemove(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "classList: not an element");
  }
  std::vector<std::string> tokens = ClassTokens(*element);
  for (int i = 0; i < argc; ++i) {
    bool ok = false;
    const std::string token = ArgString(ctx, argv[i], &ok);
    if (!ok) {
      return JS_EXCEPTION;
    }
    tokens.erase(std::remove(tokens.begin(), tokens.end(), token), tokens.end());
  }
  SetClassTokens(*element, tokens);
  return JS_UNDEFINED;
}

JSValue ClassListContains(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "classList: not an element");
  }
  if (argc < 1) {
    return JS_NewBool(ctx, false);
  }
  bool ok = false;
  const std::string token = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::vector<std::string> tokens = ClassTokens(*element);
  for (const std::string& t : tokens) {
    if (t == token) {
      return JS_NewBool(ctx, true);
    }
  }
  return JS_NewBool(ctx, false);
}

JSValue ClassListToggle(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "classList: not an element");
  }
  if (argc < 1) {
    return JS_NewBool(ctx, false);
  }
  bool ok = false;
  const std::string token = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  std::vector<std::string> tokens = ClassTokens(*element);
  const bool present = std::find(tokens.begin(), tokens.end(), token) != tokens.end();
  // Optional |force| argument: true -> add, false -> remove.
  if (argc >= 2) {
    const int force = JS_ToBool(ctx, argv[1]);
    if (force < 0) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return JS_EXCEPTION;
    }
    const bool want = force != 0;
    const bool added = want && !present;
    if (want && !present) {
      tokens.push_back(token);
    } else if (!want && present) {
      tokens.erase(std::remove(tokens.begin(), tokens.end(), token), tokens.end());
    }
    SetClassTokens(*element, tokens);
    return JS_NewBool(ctx, added);
  }
  if (present) {
    tokens.erase(std::remove(tokens.begin(), tokens.end(), token), tokens.end());
    SetClassTokens(*element, tokens);
    return JS_NewBool(ctx, false);
  }
  tokens.push_back(token);
  SetClassTokens(*element, tokens);
  return JS_NewBool(ctx, true);
}

JSValue ClassListReplace(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "classList: not an element");
  }
  if (argc < 2) {
    return JS_NewBool(ctx, false);
  }
  bool ok = false;
  const std::string old_token = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::string new_token = ArgString(ctx, argv[1], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  std::vector<std::string> tokens = ClassTokens(*element);
  for (std::string& t : tokens) {
    if (t == old_token) {
      t = new_token;
      SetClassTokens(*element, tokens);
      return JS_NewBool(ctx, true);
    }
  }
  return JS_NewBool(ctx, false);
}

JSValue ClassListItem(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "classList: not an element");
  }
  const std::vector<std::string> tokens = ClassTokens(*element);
  int64_t index = -1;
  if (argc >= 1) {
    if (JS_ToInt64(ctx, &index, argv[0]) != 0) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return JS_EXCEPTION;
    }
  }
  if (index < 0 || index >= static_cast<int64_t>(tokens.size())) {
    return JS_NULL;
  }
  return JS_NewStringLen(ctx,
                         tokens[static_cast<std::size_t>(index)].data(),
                         tokens[static_cast<std::size_t>(index)].size());
}

JSValue ClassListGetLength(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "classList: not an element");
  }
  return JS_NewInt32(ctx, static_cast<int32_t>(ClassTokens(*element).size()));
}

JSValue
ClassListToString(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "classList: not an element");
  }
  const std::vector<std::string> tokens = ClassTokens(*element);
  std::string out;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (i > 0) {
      out += ' ';
    }
    out += tokens[i];
  }
  return JS_NewStringLen(ctx, out.data(), out.size());
}

JSValue ElementGetClassList(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // Reuse the node wrapper class (NodeWrapper opaque carries the element);
  // the classList prototype provides the DOMTokenList methods.
  auto* w = new NodeWrapper{impl, element};
  JSValue list = JS_NewObjectClass(ctx, g_node_class_id);
  JS_SetOpaque(list, w);
  JS_SetPrototype(ctx, list, impl->class_list_proto);
  return list;
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

JSValue DocCreateDocumentFragment(JSContext* ctx,
                                  JSValueConst this_val,
                                  int /*argc*/,
                                  JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  auto fragment = std::make_unique<dom::DocumentFragment>();
  dom::DocumentFragment* raw = fragment.get();
  impl->created[raw] = std::move(fragment);
  return impl->WrapNode(raw);
}

JSValue DocCreateComment(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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
  auto comment = std::make_unique<dom::Comment>(text);
  dom::Comment* raw = comment.get();
  impl->created[raw] = std::move(comment);
  return impl->WrapNode(raw);
}

JSValue DocCreateElementNS(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  // The namespace is accepted and ignored (HTML semantics; no SVG/MathML
  // element types are distinguished).  createElementNS(ns, "svg") yields a
  // plain element, matching the HTML parser's behavior for unknown tags.
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "createElementNS requires (namespace, tagName)");
  }
  return DocCreateElement(ctx, this_val, 1, &argv[1]);
}

// All elements with the given tag name, in document order.
std::vector<dom::Element*> CollectByTag(const dom::Node& root, std::string_view tag)
{
  std::vector<dom::Element*> out;
  for (dom::Node* child : root.ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      if (el->tag_name() == tag) {
        out.push_back(el);
      }
      std::vector<dom::Element*> nested = CollectByTag(*el, tag);
      out.insert(out.end(), nested.begin(), nested.end());
    }
  }
  return out;
}

// All elements carrying |cls| (exact token match) in document order.
std::vector<dom::Element*> CollectByClass(const dom::Node& root, std::string_view cls)
{
  std::vector<dom::Element*> out;
  for (dom::Node* child : root.ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      for (const std::string_view token : el->ClassList()) {
        if (token == cls) {
          out.push_back(el);
          break;
        }
      }
      std::vector<dom::Element*> nested = CollectByClass(*el, cls);
      out.insert(out.end(), nested.begin(), nested.end());
    }
  }
  return out;
}

JSValue DocGetElementsByTagName(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  if (argc < 1) {
    return impl->MakeElementArray({});
  }
  bool ok = false;
  const std::string tag = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    return JS_EXCEPTION;
  }
  // "*" matches every element.
  if (tag == "*") {
    std::vector<dom::Element*> all;
    std::function<void(const dom::Node&)> walk = [&](const dom::Node& n) {
      for (dom::Node* c : n.ChildNodes()) {
        if (dom::Element* el = AsElement(c)) {
          all.push_back(el);
          walk(*el);
        }
      }
    };
    walk(*node);
    return impl->MakeElementArray(all);
  }
  return impl->MakeElementArray(CollectByTag(*node, tag));
}

JSValue
DocGetElementsByClassName(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  if (argc < 1) {
    return impl->MakeElementArray({});
  }
  bool ok = false;
  const std::string cls = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  return impl->MakeElementArray(CollectByClass(*node, cls));
}

// The current document URL (from the PageApis location callback when wired).
std::string DocumentUrl(const Impl& impl)
{
  if (impl.apis.location_href) {
    const std::string url = impl.apis.location_href();
    if (!url.empty()) {
      return url;
    }
  }
  return std::string();
}

JSValue DocGetURL(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  const std::string url = DocumentUrl(*impl);
  return JS_NewStringLen(ctx, url.data(), url.size());
}

JSValue DocGetBaseURI(JSContext* ctx, JSValueConst this_val)
{
  return DocGetURL(ctx, this_val);
}

JSValue DocGetDocumentURI(JSContext* ctx, JSValueConst this_val)
{
  return DocGetURL(ctx, this_val);
}

JSValue DocGetCharacterSet(JSContext* ctx, JSValueConst this_val)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  return JS_NewString(ctx, "UTF-8");
}

JSValue DocGetContentType(JSContext* ctx, JSValueConst this_val)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  return JS_NewString(ctx, "text/html");
}

JSValue DocGetReferrer(JSContext* ctx, JSValueConst this_val)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  return JS_NewString(ctx, "");
}

JSValue DocGetForms(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  return impl->MakeElementArray(CollectByTag(*node, "form"));
}

JSValue DocGetImages(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  return impl->MakeElementArray(CollectByTag(*node, "img"));
}

JSValue DocGetScripts(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  return impl->MakeElementArray(CollectByTag(*node, "script"));
}

JSValue DocGetLinks(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  // document.links: <a> and <area> elements with an href.
  std::vector<dom::Element*> out;
  std::function<void(const dom::Node&)> walk = [&](const dom::Node& n) {
    for (dom::Node* c : n.ChildNodes()) {
      if (dom::Element* el = AsElement(c)) {
        const std::string_view tag = el->tag_name();
        if ((tag == "a" || tag == "area") && el->HasAttribute("href")) {
          out.push_back(el);
        }
        walk(*el);
      }
    }
  };
  walk(*node);
  return impl->MakeElementArray(out);
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
// Event objects.
//
// `new Event(type, {bubbles, cancelable})` creates a real Event whose
// prototype exposes type/target/currentTarget/bubbles/cancelable/
// defaultPrevented/eventPhase/timeStamp and the standard methods.  Events
// created internally by a dispatch (including plain {type: ...} shortcuts)
// carry the same state.  The mutable dispatch state lives in the opaque
// EventWrapper; target/currentTarget are regular properties on the object.
// ---------------------------------------------------------------------------

// Event constants (DOM standard).
static constexpr int kEventNone = 0;
static constexpr int kEventCapturing = 1;
static constexpr int kEventAtTarget = 2;
static constexpr int kEventBubbling = 3;

JSValue EventConstructor(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  // The Event constructor has no node wrapper on |this|; ImplFor falls back
  // to the ctx -> Impl registry.
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  bool ok = false;
  const std::string type = argc >= 1 ? ArgString(ctx, argv[0], &ok) : std::string();
  if (!ok) {
    return JS_EXCEPTION;
  }
  bool bubbles = false;
  bool cancelable = false;
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue b = JS_GetPropertyStr(ctx, argv[1], "bubbles");
    if (JS_IsBool(b)) {
      const int v = JS_ToBool(ctx, b);
      if (v < 0) {
        JS_FreeValue(ctx, b);
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_EXCEPTION;
      }
      bubbles = v != 0;
    }
    JS_FreeValue(ctx, b);
    JSValue c = JS_GetPropertyStr(ctx, argv[1], "cancelable");
    if (JS_IsBool(c)) {
      const int v = JS_ToBool(ctx, c);
      if (v < 0) {
        JS_FreeValue(ctx, c);
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_EXCEPTION;
      }
      cancelable = v != 0;
    }
    JS_FreeValue(ctx, c);
  }
  return impl->MakeEvent(type, bubbles, cancelable);
}

// new CustomEvent(type, {detail, bubbles, cancelable}): an Event whose
// prototype chain includes Event.prototype and which carries a `detail`
// payload (CustomEventInit.detail, default null).  bing's instrumentation
// dispatches CustomEvents between its scripts, so `detail` must round-trip.
JSValue
CustomEventConstructor(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  bool ok = false;
  const std::string type = argc >= 1 ? ArgString(ctx, argv[0], &ok) : std::string();
  if (!ok) {
    return JS_EXCEPTION;
  }
  bool bubbles = false;
  bool cancelable = false;
  JSValue detail = JS_NULL;
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue b = JS_GetPropertyStr(ctx, argv[1], "bubbles");
    if (JS_IsBool(b)) {
      const int v = JS_ToBool(ctx, b);
      if (v < 0) {
        JS_FreeValue(ctx, b);
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_EXCEPTION;
      }
      bubbles = v != 0;
    }
    JS_FreeValue(ctx, b);
    JSValue c = JS_GetPropertyStr(ctx, argv[1], "cancelable");
    if (JS_IsBool(c)) {
      const int v = JS_ToBool(ctx, c);
      if (v < 0) {
        JS_FreeValue(ctx, c);
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_EXCEPTION;
      }
      cancelable = v != 0;
    }
    JS_FreeValue(ctx, c);
    JSValue d = JS_GetPropertyStr(ctx, argv[1], "detail");
    if (!JS_IsUndefined(d)) {
      detail = d; // detail defaults to null (spec); explicit undefined -> null
    } else {
      JS_FreeValue(ctx, d);
    }
  }
  JSValue event = impl->MakeEvent(type, bubbles, cancelable);
  // CustomEvent instances use the derived prototype (spec: CustomEvent extends
  // Event); MakeEvent wired event_proto, so point it at custom_event_proto.
  JS_SetPrototype(ctx, event, impl->custom_event_proto);
  JS_SetPropertyStr(ctx, event, "detail", detail); // steals detail
  return event;
}

JSValue EventGetType(JSContext* ctx, JSValueConst this_val)
{
  EventWrapper* w = UnwrapEvent(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  return JS_NewStringLen(ctx, w->type.data(), w->type.size());
}

JSValue EventGetTarget(JSContext* ctx, JSValueConst this_val)
{
  EventWrapper* w = UnwrapEvent(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  return !JS_IsUndefined(w->target) ? JS_DupValue(ctx, w->target) : JS_NULL;
}

JSValue EventGetCurrentTarget(JSContext* ctx, JSValueConst this_val)
{
  EventWrapper* w = UnwrapEvent(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  return !JS_IsUndefined(w->current_target) ? JS_DupValue(ctx, w->current_target) : JS_NULL;
}

JSValue EventGetBubbles(JSContext* ctx, JSValueConst this_val)
{
  EventWrapper* w = UnwrapEvent(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  return JS_NewBool(ctx, w->bubbles);
}

JSValue EventGetCancelable(JSContext* ctx, JSValueConst this_val)
{
  EventWrapper* w = UnwrapEvent(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  return JS_NewBool(ctx, w->cancelable);
}

JSValue EventGetDefaultPrevented(JSContext* ctx, JSValueConst this_val)
{
  EventWrapper* w = UnwrapEvent(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  return JS_NewBool(ctx, w->default_prevented);
}

JSValue EventGetEventPhase(JSContext* ctx, JSValueConst this_val)
{
  EventWrapper* w = UnwrapEvent(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  return JS_NewInt32(ctx, w->event_phase);
}

JSValue EventGetTimeStamp(JSContext* ctx, JSValueConst this_val)
{
  if (UnwrapEvent(this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  // A documented simplification: timeStamp is the time of the getter call.
  return JS_NewFloat64(ctx, static_cast<double>(std::time(nullptr)) * 1000.0);
}

JSValue EventGetIsTrusted(JSContext* ctx, JSValueConst this_val)
{
  if (UnwrapEvent(this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  return JS_FALSE;
}

JSValue
EventPreventDefault(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  EventWrapper* w = UnwrapEvent(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  if (w->cancelable) {
    w->default_prevented = true;
  }
  return JS_UNDEFINED;
}

JSValue
EventStopPropagation(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  EventWrapper* w = UnwrapEvent(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  w->propagation_stopped = true;
  return JS_UNDEFINED;
}

JSValue EventStopImmediatePropagation(JSContext* ctx,
                                      JSValueConst this_val,
                                      int /*argc*/,
                                      JSValueConst* /*argv*/)
{
  EventWrapper* w = UnwrapEvent(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  w->propagation_stopped = true;
  w->immediate_stopped = true;
  return JS_UNDEFINED;
}

JSValue
EventComposedPath(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  EventWrapper* w = UnwrapEvent(this_val);
  if (impl == nullptr || w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  dom::Node* node = !JS_IsUndefined(w->target) ? UnwrapNode(w->target) : nullptr;
  std::vector<dom::Node*> path;
  for (dom::Node* p = node; p != nullptr; p = p->parent()) {
    path.push_back(p);
  }
  // composedPath() returns root -> target.
  std::vector<dom::Node*> reversed;
  reversed.reserve(path.size());
  for (auto it = path.rbegin(); it != path.rend(); ++it) {
    reversed.push_back(*it);
  }
  return impl->MakeNodeArray(reversed);
}

void DefineEventPrototype(JSContext* ctx, Impl& impl)
{
  static const std::array<JSCFunctionListEntry, 4> kMethods = {{
      JS_CFUNC_DEF("preventDefault", 0, EventPreventDefault),
      JS_CFUNC_DEF("stopPropagation", 0, EventStopPropagation),
      JS_CFUNC_DEF("stopImmediatePropagation", 0, EventStopImmediatePropagation),
      JS_CFUNC_DEF("composedPath", 0, EventComposedPath),
  }};
  JS_SetPropertyFunctionList(
      ctx, impl.event_proto, kMethods.data(), static_cast<int>(kMethods.size()));

  DefineGetter(ctx, impl.event_proto, "type", MakeGetter(ctx, "type", EventGetType));
  DefineGetter(ctx, impl.event_proto, "target", MakeGetter(ctx, "target", EventGetTarget));
  DefineGetter(ctx,
               impl.event_proto,
               "currentTarget",
               MakeGetter(ctx, "currentTarget", EventGetCurrentTarget));
  DefineGetter(ctx, impl.event_proto, "bubbles", MakeGetter(ctx, "bubbles", EventGetBubbles));
  DefineGetter(
      ctx, impl.event_proto, "cancelable", MakeGetter(ctx, "cancelable", EventGetCancelable));
  DefineGetter(ctx,
               impl.event_proto,
               "defaultPrevented",
               MakeGetter(ctx, "defaultPrevented", EventGetDefaultPrevented));
  DefineGetter(
      ctx, impl.event_proto, "eventPhase", MakeGetter(ctx, "eventPhase", EventGetEventPhase));
  DefineGetter(ctx, impl.event_proto, "timeStamp", MakeGetter(ctx, "timeStamp", EventGetTimeStamp));
  DefineGetter(ctx, impl.event_proto, "isTrusted", MakeGetter(ctx, "isTrusted", EventGetIsTrusted));
}

void DefineClassListPrototype(JSContext* ctx, Impl& impl)
{
  static const std::array<JSCFunctionListEntry, 7> kMethods = {{
      JS_CFUNC_DEF("add", 1, ClassListAdd),
      JS_CFUNC_DEF("remove", 1, ClassListRemove),
      JS_CFUNC_DEF("toggle", 1, ClassListToggle),
      JS_CFUNC_DEF("contains", 1, ClassListContains),
      JS_CFUNC_DEF("replace", 2, ClassListReplace),
      JS_CFUNC_DEF("item", 1, ClassListItem),
      JS_CFUNC_DEF("toString", 0, ClassListToString),
  }};
  JS_SetPropertyFunctionList(
      ctx, impl.class_list_proto, kMethods.data(), static_cast<int>(kMethods.size()));
  DefineGetter(ctx, impl.class_list_proto, "length", MakeGetter(ctx, "length", ClassListGetLength));
}

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

void DefineElementPrototype(JSContext* ctx, Impl& impl)
{
  static const std::array<JSCFunctionListEntry, 12> kMethods = {{
      JS_CFUNC_DEF("getAttribute", 1, ElementGetAttribute),
      JS_CFUNC_DEF("setAttribute", 2, ElementSetAttribute),
      JS_CFUNC_DEF("removeAttribute", 1, ElementRemoveAttribute),
      JS_CFUNC_DEF("hasAttribute", 1, ElementHasAttribute),
      JS_CFUNC_DEF("querySelector", 1, ElementQuerySelector),
      JS_CFUNC_DEF("querySelectorAll", 1, ElementQuerySelectorAll),
      JS_CFUNC_DEF("getElementsByTagName", 1, ElementGetElementsByTagName),
      JS_CFUNC_DEF("getElementsByClassName", 1, ElementGetElementsByClassName),
      JS_CFUNC_DEF("matches", 1, ElementMatches),
      JS_CFUNC_DEF("closest", 1, ElementClosest),
      JS_CFUNC_DEF("remove", 0, ElementRemove),
      JS_CFUNC_DEF("getBoundingClientRect", 0, ElementGetBoundingClientRect),
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
  DefineGetter(ctx,
               impl.element_proto,
               "lastElementChild",
               MakeGetter(ctx, "lastElementChild", ElementGetLastElementChild));
  DefineGetter(ctx,
               impl.element_proto,
               "nextElementSibling",
               MakeGetter(ctx, "nextElementSibling", ElementGetNextElementSibling));
  DefineGetter(ctx,
               impl.element_proto,
               "previousElementSibling",
               MakeGetter(ctx, "previousElementSibling", ElementGetPreviousElementSibling));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "innerHTML",
                 MakeGetter(ctx, "innerHTML", ElementGetInnerHTML),
                 MakeSetter(ctx, "innerHTML", ElementSetInnerHTML));
  DefineGetter(
      ctx, impl.element_proto, "outerHTML", MakeGetter(ctx, "outerHTML", ElementGetOuterHTML));
  // innerText (read-only): an approximation returning the element's
  // textContent.  Real innerText reflects *rendered* text (hidden elements
  // excluded, whitespace normalized); the engine has no layout-backed innerText
  // yet, and bing reads innerText mainly to sniff page text during bootstrap.
  DefineGetter(
      ctx, impl.element_proto, "innerText", MakeGetter(ctx, "innerText", NodeGetTextContent));
  DefineGetter(ctx, impl.element_proto, "style", MakeGetter(ctx, "style", ElementGetStyle));
  DefineGetter(
      ctx, impl.element_proto, "classList", MakeGetter(ctx, "classList", ElementGetClassList));
  DefineGetter(ctx, impl.element_proto, "dataset", MakeGetter(ctx, "dataset", ElementGetDataset));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "hidden",
                 MakeGetter(ctx, "hidden", ElementGetHidden),
                 MakeSetter(ctx, "hidden", ElementSetHidden));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "title",
                 MakeGetter(ctx, "title", ElementGetTitle),
                 MakeSetter(ctx, "title", ElementSetTitle));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "lang",
                 MakeGetter(ctx, "lang", ElementGetLang),
                 MakeSetter(ctx, "lang", ElementSetLang));

  // Form controls (input/textarea/select/option/button).
  DefineAccessor(ctx,
                 impl.element_proto,
                 "value",
                 MakeGetter(ctx, "value", ElementGetValue),
                 MakeSetter(ctx, "value", ElementSetValue));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "checked",
                 MakeGetter(ctx, "checked", ElementGetChecked),
                 MakeSetter(ctx, "checked", ElementSetChecked));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "type",
                 MakeGetter(ctx, "type", ElementGetType),
                 MakeSetter(ctx, "type", ElementSetType));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "placeholder",
                 MakeGetter(ctx, "placeholder", ElementGetPlaceholder),
                 MakeSetter(ctx, "placeholder", ElementSetPlaceholder));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "disabled",
                 MakeGetter(ctx, "disabled", ElementGetDisabled),
                 MakeSetter(ctx, "disabled", ElementSetDisabled));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "name",
                 MakeGetter(ctx, "name", ElementGetName),
                 MakeSetter(ctx, "name", ElementSetName));
  // Links (<a>).
  DefineAccessor(ctx,
                 impl.element_proto,
                 "href",
                 MakeGetter(ctx, "href", ElementGetHref),
                 MakeSetter(ctx, "href", ElementSetHref));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "target",
                 MakeGetter(ctx, "target", ElementGetTarget),
                 MakeSetter(ctx, "target", ElementSetTarget));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "rel",
                 MakeGetter(ctx, "rel", ElementGetRel),
                 MakeSetter(ctx, "rel", ElementSetRel));
  // Images (<img>) and script src.
  DefineAccessor(ctx,
                 impl.element_proto,
                 "src",
                 MakeGetter(ctx, "src", ElementGetSrc),
                 MakeSetter(ctx, "src", ElementSetSrc));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "currentSrc",
                 MakeGetter(ctx, "currentSrc", ElementGetCurrentSrc),
                 MakeSetter(ctx, "currentSrc", ElementSetSrc));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "alt",
                 MakeGetter(ctx, "alt", ElementGetAlt),
                 MakeSetter(ctx, "alt", ElementSetAlt));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "width",
                 MakeGetter(ctx, "width", ElementGetWidth),
                 MakeSetter(ctx, "width", ElementSetWidth));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "height",
                 MakeGetter(ctx, "height", ElementGetHeight),
                 MakeSetter(ctx, "height", ElementSetHeight));
  DefineGetter(ctx,
               impl.element_proto,
               "naturalWidth",
               MakeGetter(ctx, "naturalWidth", ElementGetNaturalWidth));
  DefineGetter(ctx,
               impl.element_proto,
               "naturalHeight",
               MakeGetter(ctx, "naturalHeight", ElementGetNaturalHeight));
  DefineGetter(
      ctx, impl.element_proto, "complete", MakeGetter(ctx, "complete", ElementGetComplete));
}

void DefineDocumentPrototype(JSContext* ctx, Impl& impl)
{
  static const std::array<JSCFunctionListEntry, 10> kMethods = {{
      JS_CFUNC_DEF("getElementById", 1, DocGetElementById),
      JS_CFUNC_DEF("createElement", 1, DocCreateElement),
      JS_CFUNC_DEF("createElementNS", 2, DocCreateElementNS),
      JS_CFUNC_DEF("createTextNode", 1, DocCreateTextNode),
      JS_CFUNC_DEF("createDocumentFragment", 0, DocCreateDocumentFragment),
      JS_CFUNC_DEF("createComment", 1, DocCreateComment),
      JS_CFUNC_DEF("querySelector", 1, DocQuerySelector),
      JS_CFUNC_DEF("querySelectorAll", 1, DocQuerySelectorAll),
      JS_CFUNC_DEF("getElementsByTagName", 1, DocGetElementsByTagName),
      JS_CFUNC_DEF("getElementsByClassName", 1, DocGetElementsByClassName),
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
  DefineGetter(ctx, impl.document_proto, "URL", MakeGetter(ctx, "URL", DocGetURL));
  DefineGetter(ctx, impl.document_proto, "baseURI", MakeGetter(ctx, "baseURI", DocGetBaseURI));
  DefineGetter(
      ctx, impl.document_proto, "documentURI", MakeGetter(ctx, "documentURI", DocGetDocumentURI));
  DefineGetter(ctx,
               impl.document_proto,
               "characterSet",
               MakeGetter(ctx, "characterSet", DocGetCharacterSet));
  DefineGetter(
      ctx, impl.document_proto, "contentType", MakeGetter(ctx, "contentType", DocGetContentType));
  DefineGetter(ctx, impl.document_proto, "referrer", MakeGetter(ctx, "referrer", DocGetReferrer));
  DefineGetter(ctx, impl.document_proto, "forms", MakeGetter(ctx, "forms", DocGetForms));
  DefineGetter(ctx, impl.document_proto, "images", MakeGetter(ctx, "images", DocGetImages));
  DefineGetter(ctx, impl.document_proto, "links", MakeGetter(ctx, "links", DocGetLinks));
  DefineGetter(ctx, impl.document_proto, "scripts", MakeGetter(ctx, "scripts", DocGetScripts));
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

// ---------------------------------------------------------------------------
// Page Web APIs: window.localStorage + window.fetch (Phase 8 M3 subset).
//
// localStorage is a synchronous per-origin key-value store wired from the
// browser layer through PageApis callbacks.  fetch(url) returns a Promise
// resolved with a minimal Response object (status/ok/statusText/url/headers
// with get(), text() and json()); network errors reject the promise.  The
// synchronous network call resolves the promise immediately, and the
// microtask pump makes `await fetch(...)` continuations progress.
// ---------------------------------------------------------------------------

// Returns a promise resolved with |value| (|value|'s reference is consumed).
JSValue ResolvePromise(JSContext* ctx, JSValue value)
{
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue ctor = JS_GetPropertyStr(ctx, global, "Promise");
  JSValue resolve_fn = JS_GetPropertyStr(ctx, ctor, "resolve");
  JSValue argv[] = {value};
  // Promise.resolve must be called with the Promise constructor as |this|.
  JSValue result = JS_Call(ctx, resolve_fn, ctor, 1, argv);
  JS_FreeValue(ctx, argv[0]);
  JS_FreeValue(ctx, resolve_fn);
  JS_FreeValue(ctx, ctor);
  JS_FreeValue(ctx, global);
  return result;
}

// Returns a promise rejected with an Error carrying |message|.
JSValue RejectPromise(JSContext* ctx, const std::string& message)
{
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue ctor = JS_GetPropertyStr(ctx, global, "Promise");
  JSValue reject_fn = JS_GetPropertyStr(ctx, ctor, "reject");
  JSValue err = JS_NewError(ctx);
  JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, message.c_str()));
  JSValue argv[] = {err};
  // Promise.reject must be called with the Promise constructor as |this|.
  JSValue result = JS_Call(ctx, reject_fn, ctor, 1, argv);
  JS_FreeValue(ctx, err);
  JS_FreeValue(ctx, reject_fn);
  JS_FreeValue(ctx, ctor);
  JS_FreeValue(ctx, global);
  return result;
}

// ---- localStorage ----------------------------------------------------------

JSValue LocalStorageGetItem(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.storage_get) {
    return JS_ThrowTypeError(ctx, "localStorage is not available");
  }
  bool ok = false;
  const std::string key = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::optional<std::string> value = impl->apis.storage_get(key);
  return value.has_value() ? JS_NewString(ctx, value->c_str()) : JS_NULL;
}

JSValue LocalStorageSetItem(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.storage_set) {
    return JS_ThrowTypeError(ctx, "localStorage is not available");
  }
  bool ok = false;
  const std::string key = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::string value = ArgString(ctx, argc >= 2 ? argv[1] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  impl->apis.storage_set(key, value);
  return JS_UNDEFINED;
}

JSValue LocalStorageRemoveItem(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.storage_remove) {
    return JS_ThrowTypeError(ctx, "localStorage is not available");
  }
  bool ok = false;
  const std::string key = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  impl->apis.storage_remove(key);
  return JS_UNDEFINED;
}

JSValue
LocalStorageClear(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.storage_clear) {
    return JS_ThrowTypeError(ctx, "localStorage is not available");
  }
  impl->apis.storage_clear();
  return JS_UNDEFINED;
}

JSValue LocalStorageKey(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.storage_keys) {
    return JS_ThrowTypeError(ctx, "localStorage is not available");
  }
  int64_t index = 0;
  if (argc >= 1 && JS_ToInt64(ctx, &index, argv[0]) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_NULL;
  }
  const std::vector<std::string> keys = impl->apis.storage_keys();
  if (index < 0 || static_cast<std::size_t>(index) >= keys.size()) {
    return JS_NULL;
  }
  return JS_NewString(ctx, keys[static_cast<std::size_t>(index)].c_str());
}

JSValue LocalStorageLength(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.storage_keys) {
    return JS_NewInt32(ctx, 0);
  }
  return JS_NewInt32(ctx, static_cast<int32_t>(impl->apis.storage_keys().size()));
}

// ---- location ---------------------------------------------------------------

// Splits an absolute URL string into its Location components using simple
// string parsing so the javascript layer stays decoupled from the url module.
struct LocationParts
{
  std::string protocol; // "https:"
  std::string host;     // "www.example.com:8080"
  std::string hostname; // "www.example.com"
  std::string port;     // "8080" (empty when the URL has no port)
  std::string pathname; // "/a/b" (empty when the URL has no path)
  std::string search;   // "?x=1" (empty when absent)
  std::string hash;     // "#frag" (empty when absent)
  std::string origin;   // "https://www.example.com:8080"
};

LocationParts ParseLocationParts(std::string_view href)
{
  LocationParts out;
  const std::size_t scheme_end = href.find("://");
  if (scheme_end == std::string_view::npos) {
    const std::size_t colon = href.find(':');
    if (colon != std::string_view::npos) {
      out.protocol = std::string(href.substr(0, colon + 1));
    }
    return out;
  }
  out.protocol = std::string(href.substr(0, scheme_end + 1));
  std::size_t i = scheme_end + 3;
  const std::size_t path_start = href.find_first_of("/?#", i);
  const std::size_t host_end = path_start == std::string_view::npos ? href.size() : path_start;
  out.host = std::string(href.substr(i, host_end - i));
  out.origin = out.protocol + "//" + out.host;
  const std::size_t colon = out.host.rfind(':');
  if (colon != std::string::npos) {
    out.hostname = out.host.substr(0, colon);
    out.port = out.host.substr(colon + 1);
  } else {
    out.hostname = out.host;
  }
  if (path_start == std::string_view::npos) {
    return out;
  }
  i = path_start;
  const std::size_t q = href.find('?', i);
  const std::size_t h = href.find('#', i);
  const std::size_t path_end = std::min(q == std::string_view::npos ? href.size() : q,
                                        h == std::string_view::npos ? href.size() : h);
  out.pathname = std::string(href.substr(i, path_end - i));
  if (q != std::string_view::npos) {
    const std::size_t search_end =
        h == std::string_view::npos ? href.size() : std::min(h, href.size());
    out.search = std::string(href.substr(q, search_end - q));
  }
  if (h != std::string_view::npos) {
    out.hash = std::string(href.substr(h));
  }
  return out;
}

// Resolves a (possibly relative) target against the page base, falling back to
// the raw string when resolution is unavailable or fails.
std::string ResolveLocationTarget(Impl* impl, std::string_view target)
{
  if (impl->apis.resolve_url) {
    const std::string resolved = impl->apis.resolve_url(std::string(target));
    if (!resolved.empty()) {
      return resolved;
    }
  }
  return std::string(target);
}

JSValue LocationHrefGetter(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.location_href) {
    return JS_NewStringLen(ctx, "", 0);
  }
  const std::string href = impl->apis.location_href();
  return JS_NewStringLen(ctx, href.data(), href.size());
}

JSValue LocationHrefSetter(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.navigate) {
    return JS_UNDEFINED;
  }
  bool ok = false;
  const std::string target = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  impl->apis.navigate(ResolveLocationTarget(impl, target));
  return JS_UNDEFINED;
}

// Read-only Location parts, dispatched by magic (see kLocationMagic below).
JSValue LocationPropGetter(JSContext* ctx, JSValueConst this_val, int magic)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.location_href) {
    return JS_NewStringLen(ctx, "", 0);
  }
  const LocationParts parts = ParseLocationParts(impl->apis.location_href());
  const std::string* value = nullptr;
  switch (magic) {
  case 0:
    value = &parts.protocol;
    break;
  case 1:
    value = &parts.host;
    break;
  case 2:
    value = &parts.hostname;
    break;
  case 3:
    value = &parts.port;
    break;
  case 4:
    value = &parts.pathname;
    break;
  case 5:
    value = &parts.search;
    break;
  case 6:
    value = &parts.hash;
    break;
  case 7:
    value = &parts.origin;
    break;
  default:
    return JS_NewStringLen(ctx, "", 0);
  }
  return JS_NewStringLen(ctx, value->data(), value->size());
}

JSValue LocationAssign(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.navigate) {
    return JS_UNDEFINED;
  }
  bool ok = false;
  const std::string target = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  impl->apis.navigate(ResolveLocationTarget(impl, target));
  return JS_UNDEFINED;
}

JSValue LocationReplace(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.navigate) {
    return JS_UNDEFINED;
  }
  bool ok = false;
  const std::string target = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  // Replace is a documented approximation: the synchronous engine has no
  // back/forward entry for the navigating page to swap, so it behaves like
  // assign().
  impl->apis.navigate(ResolveLocationTarget(impl, target));
  return JS_UNDEFINED;
}

JSValue LocationReload(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.reload) {
    return JS_UNDEFINED;
  }
  impl->apis.reload();
  return JS_UNDEFINED;
}

JSValue
LocationToString(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  return LocationHrefGetter(ctx, this_val);
}

// ---- fetch -----------------------------------------------------------------

// Response.text(): a promise resolved with the body (func_data[0]).
JSValue FetchResponseText(JSContext* ctx,
                          JSValueConst /*this_val*/,
                          int /*argc*/,
                          JSValueConst* /*argv*/,
                          int /*magic*/,
                          JSValueConst* func_data)
{
  return ResolvePromise(ctx, JS_DupValue(ctx, func_data[0]));
}

// Response.json(): parses the body (func_data[0]) as JSON; rejects on error.
JSValue FetchResponseJson(JSContext* ctx,
                          JSValueConst /*this_val*/,
                          int /*argc*/,
                          JSValueConst* /*argv*/,
                          int /*magic*/,
                          JSValueConst* func_data)
{
  const char* s = JS_ToCString(ctx, func_data[0]);
  if (s == nullptr) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return RejectPromise(ctx, "Failed to parse JSON response");
  }
  JSValue parsed = JS_ParseJSON(ctx, s, std::strlen(s), "<fetch>");
  JS_FreeCString(ctx, s);
  if (JS_IsException(parsed)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return RejectPromise(ctx, "Failed to parse JSON response");
  }
  return ResolvePromise(ctx, parsed);
}

// Headers.get(name): case-insensitive lookup over func_data[0], an object of
// lowercased header names.
JSValue FetchHeadersGet(JSContext* ctx,
                        JSValueConst /*this_val*/,
                        int argc,
                        JSValueConst* argv,
                        int /*magic*/,
                        JSValueConst* func_data)
{
  bool ok = false;
  const std::string name = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  std::string lower;
  for (const char c : name) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  JSValue value = JS_GetPropertyStr(ctx, func_data[0], lower.c_str());
  if (JS_IsUndefined(value)) {
    JS_FreeValue(ctx, value);
    return JS_NULL;
  }
  return value;
}

JSValue JsFetch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  if (!impl->apis.fetch) {
    return JS_ThrowTypeError(ctx, "fetch is not available");
  }
  bool ok = false;
  const std::string raw_url = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  std::string url = impl->apis.resolve_url ? impl->apis.resolve_url(raw_url) : raw_url;
  if (url.empty()) {
    return RejectPromise(ctx, "Failed to parse URL: " + raw_url);
  }
  const base::Result<FetchResponse> response = impl->apis.fetch(url);
  if (!response.has_value()) {
    return RejectPromise(ctx, "Network error: " + response.error().message());
  }
  const FetchResponse& r = response.value();

  JSValue resp = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, r.status));
  JS_SetPropertyStr(ctx, resp, "ok", JS_NewBool(ctx, r.status >= 200 && r.status < 300));
  JS_SetPropertyStr(ctx, resp, "statusText", JS_NewString(ctx, r.status_text.c_str()));
  JS_SetPropertyStr(
      ctx, resp, "url", JS_NewString(ctx, r.final_url.empty() ? url.c_str() : r.final_url.c_str()));

  // headers: { get(name) } backed by an object of lowercased names.
  JSValue headers_map = JS_NewObject(ctx);
  for (const auto& header : r.headers) {
    std::string lower;
    for (const char c : header.first) {
      lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    JS_SetPropertyStr(ctx, headers_map, lower.c_str(), JS_NewString(ctx, header.second.c_str()));
  }
  JSValue headers = JS_NewObject(ctx);
  JSValue headers_get = JS_NewCFunctionData(ctx, FetchHeadersGet, 1, 0, 1, &headers_map);
  JS_SetPropertyStr(ctx, headers, "get", headers_get); // steals headers_get
  JS_SetPropertyStr(ctx, resp, "headers", headers);    // steals headers
  JS_FreeValue(ctx, headers_map);                      // the function dup'd it

  // text()/json(): closures over the body.
  JSValue body = JS_NewString(ctx, r.body.c_str());
  JSValue text_fn = JS_NewCFunctionData(ctx, FetchResponseText, 0, 0, 1, &body);
  JS_SetPropertyStr(ctx, resp, "text", text_fn); // steals text_fn
  JSValue json_fn = JS_NewCFunctionData(ctx, FetchResponseJson, 0, 0, 1, &body);
  JS_SetPropertyStr(ctx, resp, "json", json_fn); // steals json_fn
  JS_FreeValue(ctx, body);                       // the functions dup'd it

  return ResolvePromise(ctx, resp);
}

// ---------------------------------------------------------------------------
// Window extensions: requestAnimationFrame, scrolling, history,
// performance.now(), getComputedStyle.
// ---------------------------------------------------------------------------

JSValue
WindowRequestAnimationFrame(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
    return JS_ThrowTypeError(ctx, "requestAnimationFrame requires a callback");
  }
  Impl::RafEntry entry;
  entry.id = impl->next_raf_id++;
  entry.callback = JS_DupValue(ctx, argv[0]);
  const int64_t id = entry.id;
  impl->raf_queue.push_back(std::move(entry));
  return JS_NewInt64(ctx, id);
}

JSValue
WindowCancelAnimationFrame(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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
  auto erase_by_id = [&](std::vector<Impl::RafEntry>& v) {
    for (auto it = v.begin(); it != v.end(); ++it) {
      if (it->id == id) {
        JS_FreeValue(ctx, it->callback);
        v.erase(it);
        break;
      }
    }
  };
  erase_by_id(impl->raf_queue);
  erase_by_id(impl->raf_pending);
  return JS_UNDEFINED;
}

JSValue WindowScrollTo(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  // The page viewport is managed by the GUI scroll area; script-initiated
  // window scrolling is a no-op (documented).
  return JS_UNDEFINED;
}

JSValue WindowScrollBy(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  return JS_UNDEFINED;
}

JSValue HistoryBack(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  // Script-driven history traversal is not wired to the browser navigation
  // stack; no-op (documented).
  return JS_UNDEFINED;
}

JSValue HistoryForward(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  return JS_UNDEFINED;
}

JSValue HistoryGo(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  return JS_UNDEFINED;
}

JSValue
HistoryPushState(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  // pushState/replaceState are accepted but do not change the URL (the
  // navigation stack is not exposed to scripts); documented.
  return JS_UNDEFINED;
}

JSValue
HistoryReplaceState(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  return JS_UNDEFINED;
}

JSValue HistoryGetLength(JSContext* ctx, JSValueConst this_val)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  // The script-visible session history has exactly one entry (the current
  // document); the browser back/forward stack is separate.
  return JS_NewInt32(ctx, 1);
}

JSValue PerformanceNow(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  const auto elapsed = std::chrono::steady_clock::now() - impl->performance_origin;
  return JS_NewFloat64(ctx, std::chrono::duration<double, std::milli>(elapsed).count());
}

// ---------------------------------------------------------------------------
// window.matchMedia(query).
//
// Returns a MediaQueryList evaluated against the engine's fixed viewport
// (800x600@1x, see screen/innerWidth).  Supports the media-feature queries
// real sites use at bootstrap: (min|max)-(width|height): Npx, orientation,
// prefers-color-scheme, prefers-reduced-motion, and the (any-)pointer/hover
// features; a comma-separated list matches when any alternative does.  Unknown
// features resolve to false (conservative).  The list object is static (no
// change events); add/removeListener are no-ops, documented.
// ---------------------------------------------------------------------------
namespace {

bool MatchMediaQueryImpl(const std::string& query)
{
  // (min|max)-(width|height): <N>px  /  (orientation: portrait|landscape)  /
  // (prefers-color-scheme: light|dark)  /  (prefers-reduced-motion: ...)  /
  // (pointer|hover|any-pointer|any-hover: none|coarse|fine|hover).
  auto num_feature = [](const std::string& q, const char* name) -> std::optional<double> {
    const std::string prefix = name;
    const auto pos = q.find(prefix);
    if (pos == std::string::npos) {
      return std::nullopt;
    }
    const auto colon = q.find(':', pos);
    if (colon == std::string::npos) {
      return std::nullopt;
    }
    const auto paren = q.find(')', colon);
    const auto end = paren == std::string::npos ? q.size() : paren;
    std::string val = q.substr(colon + 1, end - colon - 1);
    val.erase(std::remove_if(
                  val.begin(), val.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
              val.end());
    if (val.size() < 3 || val.compare(val.size() - 2, 2, "px") != 0) {
      return std::nullopt;
    }
    try {
      return std::stod(val.substr(0, val.size() - 2));
    } catch (...) {
      return std::nullopt;
    }
  };
  const bool landscape = 800.0 > 600.0;
  bool negate = false;
  std::string q = query;
  // Trim, then honor a leading "not "/"only ".
  const auto first = q.find_first_not_of(" \t");
  q = first == std::string::npos ? "" : q.substr(first);
  if (q.rfind("not ", 0) == 0) {
    negate = true;
    q = q.substr(4);
  } else if (q.rfind("only ", 0) == 0) {
    q = q.substr(5);
  }
  // A bare media type (all/screen/print) matches (print does not).
  if (q.find('(') == std::string::npos) {
    const bool type_match = q == "all" || q == "screen";
    return negate ? !type_match : type_match;
  }
  bool matched = true;
  for (std::size_t i = 0; i < q.size();) {
    const auto open = q.find('(', i);
    if (open == std::string::npos) {
      break;
    }
    const auto close = q.find(')', open);
    if (close == std::string::npos) {
      break;
    }
    const std::string expr = q.substr(open + 1, close - open - 1);
    if (auto w = num_feature(expr, "min-width")) {
      matched = matched && 800.0 >= *w;
    } else if (auto w = num_feature(expr, "max-width")) {
      matched = matched && 800.0 <= *w;
    } else if (auto h = num_feature(expr, "min-height")) {
      matched = matched && 600.0 >= *h;
    } else if (auto h = num_feature(expr, "max-height")) {
      matched = matched && 600.0 <= *h;
    } else if (expr.rfind("orientation:", 0) == 0) {
      const bool p = expr.find("portrait") != std::string::npos;
      matched = matched && (p ? !landscape : landscape);
    } else if (expr.rfind("prefers-color-scheme:", 0) == 0) {
      matched = matched && expr.find("light") != std::string::npos;
    } else if (expr.rfind("prefers-reduced-motion:", 0) == 0) {
      matched = matched && expr.find("no-preference") != std::string::npos;
    } else if (expr.rfind("any-hover:", 0) == 0) {
      matched = matched && expr.find("hover") != std::string::npos;
    } else if (expr.rfind("hover:", 0) == 0) {
      matched = matched && expr.find("hover") != std::string::npos;
    } else if (expr.rfind("any-pointer:", 0) == 0 || expr.rfind("pointer:", 0) == 0) {
      matched = matched && expr.find("fine") != std::string::npos;
    } else {
      // Unknown feature: conservative no-match for this expression.
      matched = false;
    }
    i = close + 1;
  }
  return negate ? !matched : matched;
}

JSValue
MatchMediaNoOp(JSContext* /*ctx*/, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
{
  return JS_UNDEFINED;
}

} // namespace

// window.matchMedia(query) -> MediaQueryList (static; see above).
JSValue WindowMatchMedia(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  bool ok = false;
  const std::string query = argc >= 1 ? ArgString(ctx, argv[0], &ok) : std::string();
  if (!ok) {
    return JS_EXCEPTION;
  }
  // A comma-separated list matches when any alternative does.
  bool matches = false;
  std::size_t start = 0;
  while (start <= query.size()) {
    const auto comma = query.find(',', start);
    const auto part =
        query.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (MatchMediaQueryImpl(part)) {
      matches = true;
      break;
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }

  JSValue list = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, list, "matches", JS_NewBool(ctx, matches));
  JS_SetPropertyStr(ctx, list, "media", JS_NewStringLen(ctx, query.data(), query.size()));
  JS_SetPropertyStr(ctx, list, "onchange", JS_NULL);
  static const std::array<JSCFunctionListEntry, 4> kNoOps = {{
      JS_CFUNC_DEF("addEventListener", 0, MatchMediaNoOp),
      JS_CFUNC_DEF("removeEventListener", 0, MatchMediaNoOp),
      JS_CFUNC_DEF("addListener", 0, MatchMediaNoOp),
      JS_CFUNC_DEF("removeListener", 0, MatchMediaNoOp),
  }};
  JS_SetPropertyFunctionList(ctx, list, kNoOps.data(), static_cast<int>(kNoOps.size()));
  return list;
}

// ---------------------------------------------------------------------------
// window.getComputedStyle(element).
//
// Returns an object with getPropertyValue(name) plus camelCase accessors for
// every property the style engine reports.  The computed values come from the
// browser layer's PageApis::computed_style callback (serialized px strings);
// without it, an empty object is returned.
// ---------------------------------------------------------------------------

JSValue
ComputedStyleGetPropertyValue(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  JSValue map = JS_GetPropertyStr(ctx, this_val, "__props__");
  if (JS_IsUndefined(map)) {
    return JS_NewString(ctx, "");
  }
  if (argc < 1) {
    JS_FreeValue(ctx, map);
    return JS_NewString(ctx, "");
  }
  bool ok = false;
  const std::string prop = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    JS_FreeValue(ctx, map);
    return JS_EXCEPTION;
  }
  JSValue v = JS_GetPropertyStr(ctx, map, prop.c_str());
  JS_FreeValue(ctx, map);
  if (JS_IsUndefined(v)) {
    return JS_NewString(ctx, "");
  }
  return v;
}

// Direct camelCase accessor: func_data[0] is the kebab-case property name.
JSValue ComputedStyleGetByName(JSContext* ctx,
                               JSValueConst this_val,
                               int /*argc*/,
                               JSValueConst* /*argv*/,
                               int /*magic*/,
                               JSValueConst* func_data)
{
  JSValue map = JS_GetPropertyStr(ctx, this_val, "__props__");
  if (JS_IsUndefined(map)) {
    return JS_NewString(ctx, "");
  }
  const char* name = JS_ToCString(ctx, func_data[0]);
  JSValue v = name != nullptr ? JS_GetPropertyStr(ctx, map, name) : JS_UNDEFINED;
  if (name != nullptr) {
    JS_FreeCString(ctx, name);
  }
  JS_FreeValue(ctx, map);
  if (JS_IsUndefined(v)) {
    return JS_NewString(ctx, "");
  }
  return v;
}

// Converts "background-color" to "backgroundColor".
std::string PropToCamel(std::string_view prop)
{
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
  return camel;
}

JSValue WindowGetComputedStyle(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = argc >= 1 ? AsElement(UnwrapNode(argv[0])) : nullptr;
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "getComputedStyle requires an element");
  }
  std::map<std::string, std::string> props;
  if (impl->apis.computed_style) {
    props = impl->apis.computed_style(*element);
  }
  // The property map (kebab-case keys) is stored as a hidden property so
  // both getPropertyValue() and the camelCase accessors can read it.
  JSValue map = JS_NewObject(ctx);
  for (const auto& [name, value] : props) {
    JS_SetPropertyStr(ctx, map, name.c_str(), JS_NewString(ctx, value.c_str()));
  }
  JSValue style = JS_NewObject(ctx);
  JS_DefinePropertyValueStr(ctx, style, "__props__", JS_DupValue(ctx, map), JS_PROP_C_W_E);
  JSValue get_prop = JS_NewCFunction(ctx, ComputedStyleGetPropertyValue, "getPropertyValue", 1);
  JS_SetPropertyStr(ctx, style, "getPropertyValue", get_prop); // steals
  for (const auto& [name, value] : props) {
    (void)value;
    const std::string camel = PropToCamel(name);
    // The getter closure captures the kebab-case property name.
    JSValue name_data = JS_NewString(ctx, name.c_str());
    JSValue getter = JS_NewCFunctionData(ctx, ComputedStyleGetByName, 0, 0, 1, &name_data);
    // DefineGetter (JS_DefinePropertyGetSet) steals the getter reference.
    DefineGetter(ctx, style, camel.c_str(), getter);
    JS_FreeValue(ctx, name_data);
  }
  JS_FreeValue(ctx, map);
  return style;
}

} // namespace

// ---------------------------------------------------------------------------
// Impl definition.
// ---------------------------------------------------------------------------

Impl::Impl(dom::Document& doc, const PageApis& page_apis) : document(doc), apis(page_apis)
{
  navigation_start_epoch_ms =
      static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count());
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
    std::lock_guard<std::mutex> lock(g_event_class_mutex);
    JS_NewClassID(rt, &g_event_class_id);
    if (g_event_class_registered.find(rt) == g_event_class_registered.end()) {
      JSClassDef def;
      std::memset(&def, 0, sizeof(def));
      def.class_name = "Event";
      def.finalizer = &EventFinalizer;
      def.gc_mark = &EventGcMark;
      JS_NewClass(rt, g_event_class_id, &def);
      g_event_class_registered.insert(rt);
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_dataset_class_mutex);
    JS_NewClassID(rt, &g_dataset_class_id);
    if (g_dataset_class_registered.find(rt) == g_dataset_class_registered.end()) {
      g_dataset_exotic.get_property = &DatasetGetProperty;
      g_dataset_exotic.set_property = &DatasetSetProperty;
      g_dataset_exotic.get_own_property_names = &DatasetGetOwnPropertyNames;
      JSClassDef def;
      std::memset(&def, 0, sizeof(def));
      def.class_name = "DOMStringMap";
      def.finalizer = &DatasetFinalizer;
      def.exotic = &g_dataset_exotic;
      JS_NewClass(rt, g_dataset_class_id, &def);
      g_dataset_class_registered.insert(rt);
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
  event_proto = JS_NewObject(ctx);
  // CustomEvent.prototype inherits Event.prototype (spec: CustomEvent extends
  // Event); instances are created with this prototype by CustomEventConstructor.
  custom_event_proto = JS_NewObjectProto(ctx, event_proto);
  class_list_proto = JS_NewObject(ctx);

  DefineNodePrototype(ctx, *this);
  DefineElementPrototype(ctx, *this);
  DefineDocumentPrototype(ctx, *this);
  DefineStylePrototype(ctx, *this);
  DefineEventPrototype(ctx, *this);
  DefineClassListPrototype(ctx, *this);

  // Global scope: document, window, timers, DOM interface constructors.
  //
  // window IS the global object (browser semantics: window === globalThis).
  // Making them the same object means `window._G = {...}` lands on the global
  // scope and is readable as a bare `_G` in the next <script>.  bing's ~47
  // scripts run in sequence, earlier ones defining globals the later ones
  // read; with window as a separate object the chain broke at the first
  // `window._w = ...` (bare `_w` read back as undefined, then _G,
  // EventsToDuplicate, sj_evt, ... all failed downstream).
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue doc_wrap = WrapNode(&document);
  JS_SetPropertyStr(ctx, global, "document", doc_wrap); // steals doc_wrap
  window = JS_DupValue(ctx, global);

  // Global event handler attributes (HTML spec §8.1.7.2).  In browsers these
  // are global properties (`window.onload === onload`), so scripts may read or
  // assign a bare `onload`/`onerror`/...  without declaring it.  They are
  // exposed here as writable null slots; the engine does not auto-fire them
  // from its event system yet (documented limitation).
  static constexpr std::array<const char*, 24> kGlobalEventHandlers = {
      "onload",      "onunload",   "onerror",     "onresize",    "onscroll",   "onbeforeunload",
      "onpageshow",  "onpagehide", "onfocus",     "onblur",      "onclick",    "ondblclick",
      "onmousedown", "onmouseup",  "onmousemove", "onmouseover", "onmouseout", "onkeydown",
      "onkeyup",     "onkeypress", "onchange",    "oninput",     "onsubmit",   "onhashchange"};
  for (const char* handler : kGlobalEventHandlers) {
    JS_SetPropertyStr(ctx, window, handler, JS_NULL); // window === global
  }

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

  // Window viewport/animations/history/services.
  static const std::array<JSCFunctionListEntry, 9> kWindowExtensions = {{
      JS_CFUNC_DEF("requestAnimationFrame", 1, WindowRequestAnimationFrame),
      JS_CFUNC_DEF("cancelAnimationFrame", 1, WindowCancelAnimationFrame),
      JS_CFUNC_DEF("scrollTo", 0, WindowScrollTo),
      JS_CFUNC_DEF("scroll", 0, WindowScrollTo),
      JS_CFUNC_DEF("scrollBy", 0, WindowScrollBy),
      JS_CFUNC_DEF("getComputedStyle", 1, WindowGetComputedStyle),
      JS_CFUNC_DEF("requestIdleCallback", 1, WindowRequestAnimationFrame),
      JS_CFUNC_DEF("cancelIdleCallback", 1, WindowCancelAnimationFrame),
      JS_CFUNC_DEF("matchMedia", 1, WindowMatchMedia),
  }};
  JS_SetPropertyFunctionList(
      ctx, window, kWindowExtensions.data(), static_cast<int>(kWindowExtensions.size()));
  for (const char* name : {"requestAnimationFrame",
                           "cancelAnimationFrame",
                           "scrollTo",
                           "scrollBy",
                           "getComputedStyle",
                           "requestIdleCallback",
                           "cancelIdleCallback",
                           "matchMedia"}) {
    JSValue fn = JS_GetPropertyStr(ctx, window, name);
    JS_SetPropertyStr(ctx, global, name, fn); // steals fn
  }

  // window.history: a minimal History object (script-visible session length
  // plus no-op traversal/mutation; the browser's navigation stack is separate
  // and not script-exposed yet — documented).
  {
    JSValue history = JS_NewObject(ctx);
    static const std::array<JSCFunctionListEntry, 5> kHistory = {{
        JS_CFUNC_DEF("back", 0, HistoryBack),
        JS_CFUNC_DEF("forward", 0, HistoryForward),
        JS_CFUNC_DEF("go", 0, HistoryGo),
        JS_CFUNC_DEF("pushState", 0, HistoryPushState),
        JS_CFUNC_DEF("replaceState", 0, HistoryReplaceState),
    }};
    JS_SetPropertyFunctionList(ctx, history, kHistory.data(), static_cast<int>(kHistory.size()));
    DefineGetter(ctx, history, "length", MakeGetter(ctx, "length", HistoryGetLength));
    JS_SetPropertyStr(ctx, window, "history", JS_DupValue(ctx, history)); // steals dup
    JS_SetPropertyStr(ctx, global, "history", history);                   // steals
  }

  // window.performance: now()/timeOrigin only (a documented subset; real
  // navigation/resource timing is future work).
  {
    JSValue performance = JS_NewObject(ctx);
    static const std::array<JSCFunctionListEntry, 1> kPerformance = {{
        JS_CFUNC_DEF("now", 0, PerformanceNow),
    }};
    JS_SetPropertyFunctionList(
        ctx, performance, kPerformance.data(), static_cast<int>(kPerformance.size()));
    // timeOrigin and timing.navigationStart are both the page-load start
    // (epoch ms; same value as a real browser reports for a fresh load).
    JSValue origin = JS_NewFloat64(ctx, navigation_start_epoch_ms);
    JS_SetPropertyStr(ctx, performance, "timeOrigin", origin); // steals origin
    // performance.timing.navigationStart: the page load start (epoch ms).
    // bing's bootstrap reads performance.timing.navigationStart; without the
    // timing object the read throws "cannot read property ... of undefined".
    {
      JSValue timing = JS_NewObject(ctx);
      JS_SetPropertyStr(
          ctx, timing, "navigationStart", JS_NewFloat64(ctx, navigation_start_epoch_ms));
      JS_SetPropertyStr(ctx, performance, "timing", timing); // steals
    }
    JS_SetPropertyStr(ctx, window, "performance", performance); // steals
  }

  // DOM interface constructors backed by the live prototypes (see
  // DefineInterface above).  HTMLElement shares Element's prototype but does
  // not overwrite Element.prototype.constructor.
  DefineInterface(ctx, global, "Node", node_proto);
  DefineInterface(ctx, global, "Document", document_proto);
  // CharacterData.data is exposed on Text and Comment.
  DefineAccessor(ctx,
                 text_proto,
                 "data",
                 MakeGetter(ctx, "data", CharacterDataGetData),
                 MakeSetter(ctx, "data", CharacterDataSetData));
  DefineAccessor(ctx,
                 comment_proto,
                 "data",
                 MakeGetter(ctx, "data", CharacterDataGetData),
                 MakeSetter(ctx, "data", CharacterDataSetData));
  DefineInterface(ctx, global, "Text", text_proto);
  DefineInterface(ctx, global, "Comment", comment_proto);
  DefineInterface(ctx, global, "DocumentFragment", fragment_proto);
  DefineInterface(ctx, global, "CSSStyleDeclaration", style_proto);
  DefineInterface(ctx, global, "Element", element_proto);
  DefineInterface(ctx, global, "HTMLElement", element_proto, /*set_constructor=*/false);
  // Event is constructable: new Event(type, {bubbles, cancelable}).
  {
    JSValue event_ctor =
        JS_NewCFunction2(ctx, EventConstructor, "Event", 1, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, event_ctor, "prototype", JS_DupValue(ctx, event_proto));   // steals
    JS_SetPropertyStr(ctx, event_proto, "constructor", JS_DupValue(ctx, event_ctor)); // steals
    JS_SetPropertyStr(ctx, global, "Event", event_ctor); // steals event_ctor
  }
  // CustomEvent is constructable: new CustomEvent(type, {detail, ...}).
  {
    JSValue custom_event_ctor =
        JS_NewCFunction2(ctx, CustomEventConstructor, "CustomEvent", 1, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(
        ctx, custom_event_ctor, "prototype", JS_DupValue(ctx, custom_event_proto)); // steals
    JS_SetPropertyStr(
        ctx, custom_event_proto, "constructor", JS_DupValue(ctx, custom_event_ctor)); // steals
    JS_SetPropertyStr(ctx, global, "CustomEvent", custom_event_ctor);                 // steals
  }

  // navigator: engine identity.  The UA matches what the network stack sends;
  // the rest are documented defaults (the browser UI language is not wired
  // yet).  Exposed on both the global scope and the window.
  JSValue navigator = JS_NewObject(ctx);
  JS_SetPropertyStr(
      ctx, navigator, "userAgent", JS_NewString(ctx, std::string(base::GetUserAgent()).c_str()));
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

  // window.self/parent/top/frames: the engine has no frame tree, so each is a
  // self-reference (top-level browsing context semantics).  Because window IS
  // the global object, `self.performance` resolves (bing's bootstrap reads it
  // before defining _G); a separate window object would leave bare `self`
  // undefined and break the chain at `self.performance`.
  JS_SetPropertyStr(ctx, window, "self", JS_DupValue(ctx, window));
  JS_SetPropertyStr(ctx, window, "top", JS_DupValue(ctx, window));
  JS_SetPropertyStr(ctx, window, "parent", JS_DupValue(ctx, window));
  JS_SetPropertyStr(ctx, window, "frames", JS_DupValue(ctx, window));

  // window.location: href (get/set), read-only URL parts, assign()/replace()/
  // reload()/toString().  Navigation requests are deferred to the browser
  // layer via the PageApis callbacks (see dom_binding.h); scripts that assign
  // location.href (e.g. Baidu's anti-bot redirect page) trigger a navigation
  // that the controller acts on after the script run.
  if (apis.location_href || apis.navigate || apis.reload) {
    JSValue location = JS_NewObject(ctx);
    // Magic values shared with LocationPropGetter.
    static constexpr std::array<const char*, 8> kLocationMagic = {
        "protocol", "host", "hostname", "port", "pathname", "search", "hash", "origin"};
    for (int i = 0; i < static_cast<int>(kLocationMagic.size()); ++i) {
      DefineGetter(
          ctx,
          location,
          kLocationMagic[static_cast<std::size_t>(i)],
          MakeGetterMagic(ctx, kLocationMagic[static_cast<std::size_t>(i)], LocationPropGetter, i));
    }
    DefineAccessor(ctx,
                   location,
                   "href",
                   MakeGetter(ctx, "href", LocationHrefGetter),
                   MakeSetter(ctx, "href", LocationHrefSetter));
    JSValue assign_fn = JS_NewCFunction(ctx, LocationAssign, "assign", 1);
    JS_SetPropertyStr(ctx, location, "assign", assign_fn); // steals assign_fn
    JSValue replace_fn = JS_NewCFunction(ctx, LocationReplace, "replace", 1);
    JS_SetPropertyStr(ctx, location, "replace", replace_fn); // steals replace_fn
    JSValue reload_fn = JS_NewCFunction(ctx, LocationReload, "reload", 0);
    JS_SetPropertyStr(ctx, location, "reload", reload_fn); // steals reload_fn
    JSValue toString_fn = JS_NewCFunction(ctx, LocationToString, "toString", 0);
    JS_SetPropertyStr(ctx, location, "toString", toString_fn);              // steals toString_fn
    JS_SetPropertyStr(ctx, window, "location", JS_DupValue(ctx, location)); // steals dup
    JS_SetPropertyStr(ctx, global, "location", location);                   // steals
  }

  // Page Web APIs (Phase 8 M3 subset): window.localStorage and window.fetch
  // are installed only when the browser layer wired the callbacks.
  if (apis.storage_get || apis.storage_set || apis.storage_remove || apis.storage_clear ||
      apis.storage_keys) {
    JSValue local_storage = JS_NewObject(ctx);
    static const std::array<JSCFunctionListEntry, 5> kStorage = {{
        JS_CFUNC_DEF("getItem", 1, LocalStorageGetItem),
        JS_CFUNC_DEF("setItem", 2, LocalStorageSetItem),
        JS_CFUNC_DEF("removeItem", 1, LocalStorageRemoveItem),
        JS_CFUNC_DEF("clear", 0, LocalStorageClear),
        JS_CFUNC_DEF("key", 1, LocalStorageKey),
    }};
    JS_SetPropertyFunctionList(
        ctx, local_storage, kStorage.data(), static_cast<int>(kStorage.size()));
    DefineGetter(ctx, local_storage, "length", MakeGetter(ctx, "length", LocalStorageLength));
    JS_SetPropertyStr(ctx, window, "localStorage", JS_DupValue(ctx, local_storage)); // steals
    JS_SetPropertyStr(ctx, global, "localStorage", local_storage);                   // steals
  }
  if (apis.fetch) {
    JSValue fetch_fn = JS_NewCFunction(ctx, JsFetch, "fetch", 1);
    JS_SetPropertyStr(ctx, window, "fetch", JS_DupValue(ctx, fetch_fn)); // steals
    JS_SetPropertyStr(ctx, global, "fetch", fetch_fn);                   // steals
  }

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
      for (Impl::Listener& l : type_entry.second) {
        JS_FreeValue(ctx, l.callback);
      }
    }
  }
  listeners.clear();

  // Free timer callbacks.
  for (Timer& timer : timers) {
    JS_FreeValue(ctx, timer.callback);
  }
  timers.clear();

  // Free requestAnimationFrame callbacks.
  for (RafEntry& entry : raf_queue) {
    JS_FreeValue(ctx, entry.callback);
  }
  raf_queue.clear();
  for (RafEntry& entry : raf_pending) {
    JS_FreeValue(ctx, entry.callback);
  }
  raf_pending.clear();

  // Release the global object's references to the objects we installed so the
  // GC below can collect and finalize them (reachable objects would otherwise
  // be torn down by the runtime without running their finalizers, leaking the
  // NodeWrapper payloads).
  JSValue global = JS_GetGlobalObject(ctx);
  // window IS the global object, so every property installed on it below is a
  // top-level global.  Delete them here for deterministic teardown: the API
  // objects are released and the window/self/top/parent/frames self-references
  // are broken explicitly (page-created globals are reclaimed by the GC when
  // the context is freed).  Keeping this list in sync with the constructor is
  // what keeps JS_FreeRuntime's "gc_obj_list is empty" assertion green.
  for (const char* name : {"document",
                           "window",
                           "setTimeout",
                           "setInterval",
                           "clearTimeout",
                           "clearInterval",
                           "addEventListener",
                           "removeEventListener",
                           "dispatchEvent",
                           "requestAnimationFrame",
                           "cancelAnimationFrame",
                           "requestIdleCallback",
                           "cancelIdleCallback",
                           "scrollTo",
                           "scrollBy",
                           "getComputedStyle",
                           "matchMedia",
                           "Node",
                           "Document",
                           "Text",
                           "Comment",
                           "DocumentFragment",
                           "CSSStyleDeclaration",
                           "Element",
                           "HTMLElement",
                           "Event",
                           "CustomEvent",
                           "navigator",
                           "screen",
                           "innerWidth",
                           "innerHeight",
                           "devicePixelRatio",
                           "self",
                           "top",
                           "parent",
                           "frames",
                           "performance",
                           "location",
                           "localStorage",
                           "history",
                           "fetch"}) {
    JSAtom atom = JS_NewAtom(ctx, name);
    JS_DeleteProperty(ctx, global, atom, JS_PROP_THROW);
    JS_FreeAtom(ctx, atom);
  }
  JS_FreeValue(ctx, global);
  // (window === global here, so "document" was already deleted above; the
  // separate window object no longer exists.)

  // Free prototypes and window (own references).
  JS_FreeValue(ctx, node_proto);
  JS_FreeValue(ctx, element_proto);
  JS_FreeValue(ctx, text_proto);
  JS_FreeValue(ctx, comment_proto);
  JS_FreeValue(ctx, document_proto);
  JS_FreeValue(ctx, fragment_proto);
  JS_FreeValue(ctx, style_proto);
  JS_FreeValue(ctx, event_proto);
  JS_FreeValue(ctx, custom_event_proto);
  JS_FreeValue(ctx, class_list_proto);
  JS_FreeValue(ctx, window);

  // Run the GC so the (now unreachable) wrappers are finalized while the
  // runtime is still alive; this reclaims the NodeWrapper/EventWrapper
  // payloads and the QuickJS arena memory instead of leaking them at runtime
  // teardown.
  JS_RunGC(rt);

  {
    std::lock_guard<std::mutex> lock(g_ctx_mutex);
    g_ctx_to_impl.erase(ctx);
  }
  // Forget the runtimes in the class-registration sets: a future runtime
  // allocated at the same address must re-register the classes, otherwise
  // JS_NewObjectClass would use an unregistered class id.
  {
    std::lock_guard<std::mutex> lock(g_class_mutex);
    g_class_registered.erase(rt);
  }
  {
    std::lock_guard<std::mutex> lock(g_event_class_mutex);
    g_event_class_registered.erase(rt);
  }
  {
    std::lock_guard<std::mutex> lock(g_dataset_class_mutex);
    g_dataset_class_registered.erase(rt);
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
  // requestAnimationFrame callbacks queued since the last pump also run here
  // (the GUI pumps this on its frame timer).
  ran += RunPendingRaf();
  return ran;
}

int Impl::RunPendingRaf()
{
  if (raf_queue.empty()) {
    return 0;
  }
  // Move the queued callbacks to a pending list (a callback may queue more
  // frames, which must not run in the same pump).
  for (RafEntry& entry : raf_queue) {
    raf_pending.push_back(std::move(entry));
  }
  raf_queue.clear();
  const auto elapsed = std::chrono::steady_clock::now() - performance_origin;
  const double timestamp = std::chrono::duration<double, std::milli>(elapsed).count();
  int ran = 0;
  for (RafEntry& entry : raf_pending) {
    JSValue argv[] = {JS_NewFloat64(ctx, timestamp)};
    JSValue result = JS_Call(ctx, entry.callback, JS_UNDEFINED, 1, argv);
    JS_FreeValue(ctx, argv[0]);
    if (JS_IsException(result)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
    } else {
      JS_FreeValue(ctx, result);
    }
    JS_FreeValue(ctx, entry.callback);
    engine.RunPendingJobs();
    ++ran;
  }
  raf_pending.clear();
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

JSValue Impl::MakeEvent(std::string type, bool bubbles, bool cancelable)
{
  auto* w = new EventWrapper{this, std::move(type), bubbles, cancelable};
  JSValue obj = JS_NewObjectClass(ctx, g_event_class_id);
  JS_SetOpaque(obj, w);
  JS_SetPrototype(ctx, obj, event_proto);
  return obj;
}

// Runs one event through capture -> target -> bubble propagation over the
// ancestor path of |target|.  The event's target/currentTarget/eventPhase are
// updated along the way; stopPropagation()/stopImmediatePropagation() are
// honored between and within listener lists.  once listeners are removed
// after firing.  Returns false when the event was canceled.
bool Impl::DispatchPropagated(dom::Node* target, JSValue event)
{
  EventWrapper* w = UnwrapEvent(event);
  if (w == nullptr || target == nullptr) {
    return true;
  }
  // Ancestor path: [target, parent, ..., document].
  std::vector<dom::Node*> path;
  for (dom::Node* p = target; p != nullptr; p = p->parent()) {
    path.push_back(p);
  }
  const std::string type = w->type;

  // Reset the dispatch state (a single event may be dispatched repeatedly).
  w->propagation_stopped = false;
  w->immediate_stopped = false;
  w->default_prevented = false;
  w->event_phase = kEventNone;
  if (!JS_IsUndefined(w->target)) {
    JS_FreeValue(ctx, w->target);
    w->target = JS_UNDEFINED;
  }
  if (!JS_IsUndefined(w->current_target)) {
    JS_FreeValue(ctx, w->current_target);
    w->current_target = JS_UNDEFINED;
  }
  w->target = WrapNode(target); // owned reference

  // Fires the listeners registered on |node| for |phase| (capture or bubble).
  // Listeners are snapshotted so additions/removals during dispatch do not
  // mutate the list being iterated; once listeners are removed after firing.
  auto fire = [&](dom::Node* node, bool capture_phase, int phase) {
    if (w->immediate_stopped || w->propagation_stopped) {
      return;
    }
    const auto el_it = listeners.find(node);
    if (el_it == listeners.end()) {
      return;
    }
    const auto type_it = el_it->second.find(type);
    if (type_it == el_it->second.end()) {
      return;
    }
    std::vector<Listener> snapshot;
    snapshot.reserve(type_it->second.size());
    for (const Listener& l : type_it->second) {
      if (l.capture == capture_phase) {
        snapshot.push_back(Listener{JS_DupValue(ctx, l.callback), l.capture, l.once});
      }
    }
    if (snapshot.empty()) {
      return;
    }
    if (!JS_IsUndefined(w->current_target)) {
      JS_FreeValue(ctx, w->current_target);
    }
    w->current_target = WrapNode(node); // owned reference
    w->event_phase = phase;
    JSValue this_wrap = WrapNode(node);
    for (const Listener& l : snapshot) {
      if (w->immediate_stopped) {
        break;
      }
      JSValue result = JS_Call(ctx, l.callback, this_wrap, 1, &event);
      if (JS_IsException(result)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
      } else {
        JS_FreeValue(ctx, result);
      }
      if (l.once) {
        // Remove the once listener from the live list after it fires.
        auto& live = type_it->second;
        for (auto it = live.begin(); it != live.end(); ++it) {
          if (it->once && JS_IsStrictEqual(ctx, it->callback, l.callback)) {
            JS_FreeValue(ctx, it->callback);
            live.erase(it);
            break;
          }
        }
      }
    }
    JS_FreeValue(ctx, this_wrap);
    for (const Listener& l : snapshot) {
      JS_FreeValue(ctx, l.callback);
    }
  };

  // Capture phase: root -> target (the target itself is handled next).
  for (auto it = path.rbegin(); it != path.rend(); ++it) {
    if (*it == target) {
      break;
    }
    fire(*it, /*capture=*/true, kEventCapturing);
    if (w->immediate_stopped || w->propagation_stopped) {
      break;
    }
  }
  // Target phase: capture listeners then bubble listeners on the target.
  if (!w->propagation_stopped) {
    fire(target, /*capture=*/true, kEventAtTarget);
    if (!w->propagation_stopped) {
      fire(target, /*capture=*/false, kEventAtTarget);
    }
  }
  // Bubble phase: target's ancestors, only when the event bubbles.
  if (!w->propagation_stopped && w->bubbles) {
    for (std::size_t i = 1; i < path.size(); ++i) {
      fire(path[i], /*capture=*/false, kEventBubbling);
      if (w->immediate_stopped || w->propagation_stopped) {
        break;
      }
    }
  }
  w->event_phase = kEventNone;
  return !w->default_prevented;
}

// Shared dispatch: runs the listeners registered on |node| (and its
// ancestors, when the event bubbles) for |type|.
void Impl::DispatchToNode(dom::Node* node, std::string_view type)
{
  if (node == nullptr) {
    return;
  }
  JSValue event = MakeEvent(std::string(type), /*bubbles=*/true, /*cancelable=*/false);
  DispatchPropagated(node, event);
  JS_FreeValue(ctx, event);
  // Promise continuations created by the listeners make progress.
  engine.RunPendingJobs();
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

DomBinder::DomBinder(dom::Document& document) : impl_(std::make_unique<Impl>(document, PageApis{}))
{}

DomBinder::DomBinder(dom::Document& document, const PageApis& apis)
    : impl_(std::make_unique<Impl>(document, apis))
{}

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
