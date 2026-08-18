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
#include "neko/url/url.h"

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
  EventWrapper(Impl* i, std::string t, bool b, bool c)
      : impl(i), type(std::move(t)), bubbles(b), cancelable(c)
  {}
  Impl* impl = nullptr;
  std::string type;
  bool bubbles = false;
  bool cancelable = false;
  bool default_prevented = false;
  bool propagation_stopped = false;
  bool immediate_stopped = false;
  int event_phase = 0; // 0 none, 1 capture, 2 target, 3 bubble
  // KeyboardEvent fields (empty for non-keyboard events).
  std::string key;
  std::string code;
  int key_code = 0; // legacy UI Events keyCode
  // Pointer/MouseEvent fields.
  double client_x = 0;
  double client_y = 0;
  int button = 0;
  // WheelEvent vertical scroll delta (px).
  double delta_y = 0;
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
    auto clone =
        std::make_unique<dom::Element>(std::string(el.tag_name()), std::string(el.namespace_uri()));
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
constexpr std::array<std::string_view, 18> kStyleProps = {
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
    "zoom",
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
  JSValue node_list_proto = JS_UNDEFINED;
  JSValue html_iframe_element_proto = JS_UNDEFINED;
  JSValue svg_element_proto = JS_UNDEFINED;
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

  // Element IDL event handlers (element.oninput = fn). These cannot live on
  // the wrapper itself because reading the property would re-enter its
  // prototype accessor.
  std::unordered_map<const dom::Node*, std::unordered_map<std::string, JSValue>> event_handlers;

  struct MutationRecord
  {
    std::string type;
    dom::Node* target = nullptr;
    std::vector<dom::Node*> added_nodes;
    std::vector<dom::Node*> removed_nodes;
    std::string attribute_name;
  };
  struct MutationObserver
  {
    JSValue callback = JS_UNDEFINED;
    JSValue self = JS_UNDEFINED;
    dom::Node* target = nullptr;
    bool child_list = false;
    bool attributes = false;
    bool character_data = false;
    bool subtree = false;
    std::vector<MutationRecord> records;
  };
  std::vector<MutationObserver> mutation_observers;
  bool delivering_mutation_observers = false;

  void RecordChildListMutation(dom::Node* target,
                               std::vector<dom::Node*> added,
                               std::vector<dom::Node*> removed);
  void RecordAttributeMutation(dom::Node* target, std::string attribute_name);
  void RecordCharacterDataMutation(dom::Node* target);
  void DeliverMutationObservers();

  // Set whenever a JS DOM mutation runs (textContent/attribute/style/node
  // tree edits).  The browser layer reads it after dispatching user
  // interaction events to decide whether to re-run the style cascade/layout,
  // so on-screen updates from event handlers are reflected promptly.
  bool dom_dirty_ = false;
  void MarkDomDirty()
  {
    dom_dirty_ = true;
  }
  bool TakeDomDirty()
  {
    const bool dirty = dom_dirty_;
    dom_dirty_ = false;
    return dirty;
  }

  // The <script> element whose body is currently executing (WHATWG HTML
  // §4.12.1 document.currentScript); null outside of classic script
  // execution.  Set/cleared by the browser layer around each script body.
  dom::Element* current_script = nullptr;
  void SetCurrentScript(dom::Element* element)
  {
    current_script = element;
  }

  // DOMImplementation object (document.implementation): a small subset of
  // the DOM Core contract used by script bootstraps and legacy libraries.
  JSValue document_implementation = JS_UNDEFINED;

  // Optional browser Web APIs (localStorage/fetch) wired by the browser layer.
  PageApis apis;

  // -------------------------------------------------------------------------
  // window.indexedDB state.  Handles back the JS object model (databases,
  // transactions, object stores) and keep their C++ state alive for the
  // binder's lifetime; JS objects reference them by index.
  // -------------------------------------------------------------------------
  struct IdbStoreInfo
  {
    std::string name;
    std::string key_path; // "" = out-of-line keys
    bool auto_increment = false;
  };
  struct IdbHandle
  {
    enum class Kind
    {
      kDatabase,
      kTransaction,
      kObjectStore
    };
    Kind kind = Kind::kDatabase;
    std::string db_name;
    std::string store_name;
    std::string mode; // transactions: readonly/readwrite/versionchange
    bool upgrade = false;
    bool aborted = false;
    bool completed = false;
    int64_t version = 0;
    std::vector<IdbStoreInfo> stores; // databases
    int db_handle = -1;               // transactions/stores → owning database
    int tx_handle = -1;               // stores → owning transaction
    int pending = 0;                  // transactions: outstanding requests
    JSValue object = JS_UNDEFINED;    // the JS object this handle backs (dup'd)
  };
  struct IdbRequest
  {
    bool is_open = false;
    bool is_delete_db = false;
    std::string db_name;
    int64_t requested_version = 0;
    int tx_handle = -1;
    std::string error_name;
    std::string error_message;
    std::optional<std::string> result_json;
    bool has_result = false;
  };
  std::vector<std::shared_ptr<IdbHandle>> idb_handles;
  std::vector<std::shared_ptr<IdbRequest>> idb_requests;

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
  // Creates a KeyboardEvent-carrying Event with key/code strings.
  JSValue MakeKeyboardEvent(
      std::string type, bool bubbles, bool cancelable, std::string key, std::string code);
  // Creates a pointer (MouseEvent) carrying Event with client coordinates.
  JSValue MakeMouseEvent(std::string type,
                         bool bubbles,
                         bool cancelable,
                         double client_x,
                         double client_y,
                         int button);
  // Creates a wheel Event carrying a vertical scroll delta.
  JSValue MakeWheelEvent(std::string type, bool bubbles, bool cancelable, double delta_y);
  // Dispatches a cancelable pointer event to |node|; returns whether NOT
  // canceled.
  bool DispatchMouseToNode(
      dom::Node* node, std::string_view type, double client_x, double client_y, int button);
  // Dispatches a cancelable wheel event to |node|; returns whether NOT canceled.
  bool DispatchWheelToNode(dom::Node* node, std::string_view type, double delta_y);
  // Dispatches a non-bubbling focus/blur event to |node|.
  void DispatchFocusToNode(dom::Node* node, std::string_view type);
  // Dispatches a bubbling "input" event to |node|.
  void DispatchInputToNode(dom::Node* node);
  // Fires the element's global event handler (element.onclick/oninput/...),
  // whether set via JS (IDL) or a content attribute (on*="code"), during
  // dispatch for every element on the propagation path.
  void FireEventHandler(dom::Node* node, EventWrapper* w, JSValue event, int phase);
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
  bool DispatchCancelableToNode(dom::Node* node, std::string_view type);
  bool DispatchKeyboardToNode(dom::Node* node,
                              std::string_view type,
                              std::string_view key,
                              std::string_view code);
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

int MutationObserverIndex(JSContext* ctx, JSValueConst value)
{
  JSValue index = JS_GetPropertyStr(ctx, value, "_nekoMutationObserver");
  int32_t out = -1;
  if (!JS_IsUndefined(index)) {
    (void)JS_ToInt32(ctx, &out, index);
  }
  JS_FreeValue(ctx, index);
  return out;
}

JSValue MutationObserverObserve(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  const int index = MutationObserverIndex(ctx, this_val);
  if (impl == nullptr || index < 0 || index >= static_cast<int>(impl->mutation_observers.size())) {
    return JS_ThrowTypeError(ctx, "not a MutationObserver");
  }
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "observe requires target and options");
  }
  dom::Node* target = UnwrapNode(argv[0]);
  if (target == nullptr) {
    return JS_ThrowTypeError(ctx, "observe target is not a Node");
  }
  auto read_flag = [&](const char* name) {
    JSValue value = JS_GetPropertyStr(ctx, argv[1], name);
    const int result = JS_ToBool(ctx, value);
    JS_FreeValue(ctx, value);
    return result > 0;
  };
  Impl::MutationObserver& observer = impl->mutation_observers[static_cast<std::size_t>(index)];
  observer.target = target;
  observer.child_list = read_flag("childList");
  observer.attributes = read_flag("attributes");
  observer.character_data = read_flag("characterData");
  observer.subtree = read_flag("subtree");
  if (!observer.child_list && !observer.attributes && !observer.character_data) {
    return JS_ThrowTypeError(ctx, "observe options must enable a mutation type");
  }
  return JS_UNDEFINED;
}

JSValue MutationObserverDisconnect(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  const int index = MutationObserverIndex(ctx, this_val);
  if (impl == nullptr || index < 0 || index >= static_cast<int>(impl->mutation_observers.size())) {
    return JS_ThrowTypeError(ctx, "not a MutationObserver");
  }
  Impl::MutationObserver& observer = impl->mutation_observers[static_cast<std::size_t>(index)];
  observer.target = nullptr;
  observer.records.clear();
  return JS_UNDEFINED;
}

JSValue MutationObserverTakeRecords(JSContext* ctx,
                                    JSValueConst this_val,
                                    int /*argc*/,
                                    JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  const int index = MutationObserverIndex(ctx, this_val);
  if (impl == nullptr || index < 0 || index >= static_cast<int>(impl->mutation_observers.size())) {
    return JS_ThrowTypeError(ctx, "not a MutationObserver");
  }
  Impl::MutationObserver& observer = impl->mutation_observers[static_cast<std::size_t>(index)];
  JSValue records = JS_NewArray(ctx);
  for (std::size_t i = 0; i < observer.records.size(); ++i) {
    const Impl::MutationRecord& record = observer.records[i];
    JSValue item = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, item, "type", JS_NewString(ctx, record.type.c_str()));
    JS_SetPropertyStr(ctx, item, "target", impl->WrapNode(record.target));
    JS_SetPropertyStr(ctx,
                      item,
                      "attributeName",
                      record.attribute_name.empty() ? JS_NULL
                                                    : JS_NewString(ctx, record.attribute_name.c_str()));
    JSValue added = impl->MakeNodeArray(record.added_nodes);
    JSValue removed = impl->MakeNodeArray(record.removed_nodes);
    JS_SetPropertyStr(ctx, item, "addedNodes", added);
    JS_SetPropertyStr(ctx, item, "removedNodes", removed);
    JS_SetPropertyUint32(ctx, records, static_cast<uint32_t>(i), item);
  }
  observer.records.clear();
  return records;
}

JSValue MutationObserverConstructor(JSContext* ctx,
                                    JSValueConst /*new_target*/,
                                    int argc,
                                    JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  if (impl == nullptr || argc < 1 || !JS_IsFunction(ctx, argv[0])) {
    return JS_ThrowTypeError(ctx, "MutationObserver callback must be a function");
  }
  JSValue observer = JS_NewObject(ctx);
  const int index = static_cast<int>(impl->mutation_observers.size());
  JS_SetPropertyStr(ctx, observer, "_nekoMutationObserver", JS_NewInt32(ctx, index));
  JS_SetPropertyStr(ctx, observer, "observe", JS_NewCFunction(ctx, MutationObserverObserve, "observe", 2));
  JS_SetPropertyStr(ctx, observer, "disconnect", JS_NewCFunction(ctx, MutationObserverDisconnect, "disconnect", 0));
  JS_SetPropertyStr(ctx, observer, "takeRecords", JS_NewCFunction(ctx, MutationObserverTakeRecords, "takeRecords", 0));
  Impl::MutationObserver state;
  state.callback = JS_DupValue(ctx, argv[0]);
  state.self = JS_DupValue(ctx, observer);
  impl->mutation_observers.push_back(std::move(state));
  return observer;
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
  if (parent->WouldExceedMaximumTreeDepth(*child)) {
    return ThrowDomException(ctx, "HierarchyRequestError", "appendChild: maximum tree depth exceeded");
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
    return ThrowDomException(ctx, "HierarchyRequestError", "insertBefore: maximum tree depth exceeded");
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
  ImplFor(ctx, this_val)->MarkDomDirty();
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
  ImplFor(ctx, this_val)->MarkDomDirty();
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
                          JS_DupValue(ctx, obj));
    // NamedNodeMap supports both indexed and named access, e.g.
    // element.attributes.style.value.
    JS_SetPropertyStr(ctx, arr, attrs[i].name.c_str(), obj);
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

// Returns the laid-out geometry of the element backing |this_val|, or nullopt
// when it is not an element, no layout exists, or the browser layer did not
// wire geometry.
std::optional<ElementGeometry> ElementGeometryOf(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return std::nullopt;
  }
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.element_geometry) {
    return std::nullopt;
  }
  return impl->apis.element_geometry(*element);
}

JSValue ElementGetBoundingClientRect(JSContext* ctx,
                                     JSValueConst this_val,
                                     int /*argc*/,
                                     JSValueConst* /*argv*/)
{
  if (AsElement(UnwrapNode(this_val)) == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // The laid-out border box in document coordinates (client coordinates when
  // the page is scrolled to the top; scroll offsets are not yet modelled).
  const std::optional<ElementGeometry> g = ElementGeometryOf(ctx, this_val);
  const double x = g ? g->x : 0;
  const double y = g ? g->y : 0;
  const double width = g ? g->width : 0;
  const double height = g ? g->height : 0;
  JSValue rect = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, rect, "x", JS_NewFloat64(ctx, x));
  JS_SetPropertyStr(ctx, rect, "y", JS_NewFloat64(ctx, y));
  JS_SetPropertyStr(ctx, rect, "left", JS_NewFloat64(ctx, x));
  JS_SetPropertyStr(ctx, rect, "top", JS_NewFloat64(ctx, y));
  JS_SetPropertyStr(ctx, rect, "right", JS_NewFloat64(ctx, x + width));
  JS_SetPropertyStr(ctx, rect, "bottom", JS_NewFloat64(ctx, y + height));
  JS_SetPropertyStr(ctx, rect, "width", JS_NewFloat64(ctx, width));
  JS_SetPropertyStr(ctx, rect, "height", JS_NewFloat64(ctx, height));
  JS_SetPropertyStr(ctx, rect, "toJSON", JS_NewCFunction(ctx, RectToJson, "toJSON", 0)); // steals
  return rect;
}

#define DEFINE_GEOMETRY_GETTER(Name, Member)                                                       \
  JSValue Name(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)        \
  {                                                                                                \
    const std::optional<ElementGeometry> g = ElementGeometryOf(ctx, this_val);                     \
    return JS_NewFloat64(ctx, g ? static_cast<double>(g->Member) : 0);                             \
  }

DEFINE_GEOMETRY_GETTER(ElementGetOffsetWidth, width)
DEFINE_GEOMETRY_GETTER(ElementGetOffsetHeight, height)
DEFINE_GEOMETRY_GETTER(ElementGetClientWidth, client_width)
DEFINE_GEOMETRY_GETTER(ElementGetClientHeight, client_height)
DEFINE_GEOMETRY_GETTER(ElementGetClientTop, border_top)
DEFINE_GEOMETRY_GETTER(ElementGetClientLeft, border_left)
DEFINE_GEOMETRY_GETTER(ElementGetOffsetTop, y)
DEFINE_GEOMETRY_GETTER(ElementGetOffsetLeft, x)

#undef DEFINE_GEOMETRY_GETTER

JSValue
ElementGetOffsetParent(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // Simplified: the <body> is the offset parent of in-flow content; positioned
  // ancestors and table cells are not yet modelled.  offsetTop/offsetLeft are
  // therefore reported in document coordinates.
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_NULL;
  }
  dom::Element* body = nullptr;
  if (impl->document.document_element() != nullptr) {
    body = dom::QuerySelector(*impl->document.document_element(), "body");
  }
  if (body == nullptr || body == element) {
    return JS_NULL;
  }
  return impl->WrapNode(body);
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
  ImplFor(ctx, this_val)->MarkDomDirty();
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

[[maybe_unused]] bool IsFormControl(const dom::Element& element)
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

enum class AnchorUrlPart
{
  kProtocol,
  kHost,
  kHostname,
  kPort,
  kPathname,
  kSearch,
  kHash,
};

JSValue ElementGetAnchorUrlPart(JSContext* ctx, JSValueConst this_val, int magic)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const base::Result<url::Url> parsed =
      url::Url::Parse(ResolvedUrl(*impl, RawAttr(*element, "href")));
  if (!parsed.has_value()) {
    return JS_NewStringLen(ctx, "", 0);
  }

  const url::Url& parsed_url = parsed.value();
  std::string value;
  switch (static_cast<AnchorUrlPart>(magic)) {
  case AnchorUrlPart::kProtocol:
    value = parsed_url.scheme().empty() ? "" : parsed_url.scheme() + ":";
    break;
  case AnchorUrlPart::kHost:
    value = parsed_url.host();
    if (parsed_url.port().has_value()) {
      value += ":" + std::to_string(parsed_url.port().value());
    }
    break;
  case AnchorUrlPart::kHostname:
    value = parsed_url.host();
    break;
  case AnchorUrlPart::kPort:
    value = parsed_url.port().has_value() ? std::to_string(parsed_url.port().value()) : "";
    break;
  case AnchorUrlPart::kPathname:
    value = parsed_url.path().empty() ? "/" : parsed_url.path();
    break;
  case AnchorUrlPart::kSearch:
    value = parsed_url.has_query() ? "?" + parsed_url.query() : "";
    break;
  case AnchorUrlPart::kHash:
    value = parsed_url.has_fragment() ? "#" + parsed_url.fragment() : "";
    break;
  }
  return JS_NewStringLen(ctx, value.data(), value.size());
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

// ---- HTMLMediaElement (video) ----------------------------------------------

bool IsVideoElement(const dom::Element* element)
{
  return element != nullptr && element->tag_name() == "video";
}

JSValue
ElementPlayVideo(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (!IsVideoElement(element)) {
    return JS_ThrowTypeError(ctx, "play() is only defined on <video>");
  }
  if (!impl->apis.video_play) {
    return JS_ThrowTypeError(ctx, "video playback is not available");
  }
  impl->apis.video_play(*element);
  return JS_UNDEFINED;
}

JSValue
ElementPauseVideo(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (!IsVideoElement(element)) {
    return JS_ThrowTypeError(ctx, "pause() is only defined on <video>");
  }
  if (impl->apis.video_pause) {
    impl->apis.video_pause(*element);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetVideoDuration(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr || !IsVideoElement(element) ||
      !impl->apis.video_duration) {
    return JS_NewFloat64(ctx, std::nan(""));
  }
  const std::optional<double> duration = impl->apis.video_duration(*element);
  return JS_NewFloat64(ctx, duration.value_or(std::nan("")));
}

JSValue ElementGetVideoCurrentTime(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr || !IsVideoElement(element) ||
      !impl->apis.video_current_time) {
    return JS_NewFloat64(ctx, 0);
  }
  return JS_NewFloat64(ctx, impl->apis.video_current_time(*element).value_or(0));
}

JSValue ElementSetVideoCurrentTime(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr || !IsVideoElement(element) || !impl->apis.video_seek) {
    return JS_UNDEFINED;
  }
  double seconds = 0;
  if (JS_ToFloat64(ctx, &seconds, value) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_UNDEFINED;
  }
  impl->apis.video_seek(*element, seconds);
  return JS_UNDEFINED;
}

JSValue ElementGetVideoPaused(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr || !IsVideoElement(element) ||
      !impl->apis.video_paused) {
    return JS_NewBool(ctx, 1);
  }
  return JS_NewBool(ctx, impl->apis.video_paused(*element) ? 1 : 0);
}

[[maybe_unused]] JSValue ElementGetText(JSContext* ctx, JSValueConst this_val)
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
  impl->MarkDomDirty();
  return JS_UNDEFINED;
}

// Parses |html| as a fragment and moves the parsed <body> children into a
// fresh document, returning them in document order (scripts are not run).
std::vector<std::unique_ptr<dom::Node>> TakeFragmentChildren(std::string_view html)
{
  std::unique_ptr<dom::Document> parsed = html::Parser(html).Parse();
  std::vector<std::unique_ptr<dom::Node>> out;
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
  if (body != nullptr) {
    while (body->first_child() != nullptr) {
      out.push_back(body->RemoveChild(body->first_child()));
    }
  }
  return out;
}

// Element.insertAdjacentHTML(position, html): parses the fragment and inserts
// it relative to the element — beforebegin/afterend outside (sibling) and
// afterbegin/beforeend inside.
JSValue
ElementInsertAdjacentHTML(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "insertAdjacentHTML requires (position, text)");
  }
  bool ok = false;
  const std::string position = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::string html = ArgString(ctx, argv[1], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  std::vector<std::unique_ptr<dom::Node>> nodes = TakeFragmentChildren(html);
  const auto insert_before =
      [&](dom::Node* parent, std::unique_ptr<dom::Node>&& node, dom::Node* reference) {
        parent->InsertBefore(std::move(node), reference);
      };
  if (position == "beforeend") {
    for (auto& node : nodes) {
      element->AppendChild(std::move(node));
    }
  } else if (position == "afterbegin") {
    dom::Node* first = element->first_child();
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
      element->InsertBefore(std::move(*it), first);
    }
  } else if (position == "beforebegin" || position == "afterend") {
    dom::Node* parent = element->parent();
    if (parent == nullptr) {
      return JS_UNDEFINED; // detached element: nothing to do
    }
    dom::Node* reference = position == "beforebegin" ? element : SiblingOf(element, +1);
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
      insert_before(parent, std::move(*it), reference);
    }
  }
  impl->MarkDomDirty();
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
  Impl* impl = ImplFor(ctx, this_val);
  impl->RecordAttributeMutation(element, name);
  impl->MarkDomDirty();
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
  Impl* impl = ImplFor(ctx, this_val);
  impl->RecordAttributeMutation(element, name);
  impl->MarkDomDirty();
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

JSValue DocumentImplementationHasFeature(JSContext* ctx,
                                        JSValueConst /*this_val*/,
                                        int argc,
                                        JSValueConst* argv)
{
  if (argc < 1) {
    return JS_FALSE;
  }
  bool ok = false;
  const std::string feature = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (argc >= 2) {
    // |version| is accepted but not yet used to differentiate partial support.
    bool version_ok = false;
    (void)ArgString(ctx, argv[1], &version_ok);
  }
  return feature == "html" || feature == "dom" || feature == "core" || feature == "xml" ||
                 feature == "css" || feature == "css2" || feature == "html5"
             ? JS_TRUE
             : JS_FALSE;
}

JSValue DocumentImplementationCreateHTMLDocument(JSContext* ctx,
                                                JSValueConst this_val,
                                                int argc,
                                                JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  bool ok = false;
  const std::string title = argc >= 1 ? ArgString(ctx, argv[0], &ok) : std::string();
  if (argc >= 1 && !ok) {
    return JS_EXCEPTION;
  }

  auto document = std::make_unique<dom::Document>();
  auto html = std::make_unique<dom::Element>("html");
  auto head = std::make_unique<dom::Element>("head");
  auto body = std::make_unique<dom::Element>("body");
  if (!title.empty()) {
    auto title_el = std::make_unique<dom::Element>("title");
    title_el->AppendChild(std::make_unique<dom::Text>(title));
    head->AppendChild(std::move(title_el));
  }
  html->AppendChild(std::move(head));
  html->AppendChild(std::move(body));
  document->AppendChild(std::move(html));

  dom::Document* raw = document.get();
  impl->created[raw] = std::move(document);
  return impl->WrapNode(raw);
}

JSValue DocGetImplementation(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr || node->node_type() != dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  if (JS_IsUndefined(impl->document_implementation)) {
    JSValue doc_impl = JS_NewObject(ctx);
    JSValue has_feature = JS_NewCFunction(ctx, DocumentImplementationHasFeature, "hasFeature", 2);
    JS_SetPropertyStr(ctx, doc_impl, "hasFeature", has_feature); // steals
    JSValue create_html =
        JS_NewCFunction(ctx, DocumentImplementationCreateHTMLDocument, "createHTMLDocument", 1);
    JS_SetPropertyStr(ctx, doc_impl, "createHTMLDocument", create_html); // steals
    impl->document_implementation = JS_DupValue(ctx, doc_impl);
    JS_FreeValue(ctx, doc_impl);
  }
  return JS_DupValue(ctx, impl->document_implementation);
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
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "createElementNS requires (namespace, tagName)");
  }
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  bool ok = false;
  const std::string namespace_uri = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::string tag = ArgString(ctx, argv[1], &ok);
  if (!ok || tag.empty()) {
    return JS_ThrowTypeError(ctx, "createElementNS requires a tag name");
  }
  auto element = std::make_unique<dom::Element>(tag, namespace_uri);
  dom::Element* raw = element.get();
  impl->created[raw] = std::move(element);
  return impl->WrapNode(raw);
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

JSValue DocGetCookie(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.cookie_get) {
    return JS_NewStringLen(ctx, "", 0);
  }
  const std::string cookies = impl->apis.cookie_get();
  return JS_NewStringLen(ctx, cookies.data(), cookies.size());
}

JSValue DocSetCookie(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.cookie_set) {
    return JS_UNDEFINED;
  }
  bool ok = false;
  const std::string assignment = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  impl->apis.cookie_set(assignment);
  return JS_UNDEFINED;
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

JSValue DocGetCurrentScript(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  // The element whose classic script body is currently executing (WHATWG
  // HTML §4.12.1), or null outside of script execution.  Bundlers (e.g.
  // umi/utoo) read this at entry time to locate their chunk manifest.
  return impl->WrapNode(impl->current_script);
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
  ImplFor(ctx, this_val)->MarkDomDirty();
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

JSValue EventGetKey(JSContext* ctx, JSValueConst this_val)
{
  EventWrapper* w = UnwrapEvent(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  return JS_NewStringLen(ctx, w->key.data(), w->key.size());
}

JSValue EventGetCode(JSContext* ctx, JSValueConst this_val)
{
  EventWrapper* w = UnwrapEvent(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  return JS_NewStringLen(ctx, w->code.data(), w->code.size());
}

JSValue EventGetKeyCode(JSContext* ctx, JSValueConst this_val)
{
  EventWrapper* w = UnwrapEvent(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  return JS_NewInt32(ctx, w->key_code);
}

JSValue EventGetClientX(JSContext* ctx, JSValueConst this_val)
{
  EventWrapper* w = UnwrapEvent(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  return JS_NewFloat64(ctx, w->client_x);
}

JSValue EventGetClientY(JSContext* ctx, JSValueConst this_val)
{
  EventWrapper* w = UnwrapEvent(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  return JS_NewFloat64(ctx, w->client_y);
}

JSValue EventGetButton(JSContext* ctx, JSValueConst this_val)
{
  EventWrapper* w = UnwrapEvent(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  return JS_NewInt32(ctx, w->button);
}

JSValue EventGetDeltaY(JSContext* ctx, JSValueConst this_val)
{
  EventWrapper* w = UnwrapEvent(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an Event");
  }
  return JS_NewFloat64(ctx, w->delta_y);
}

// Maps an event type to the element's global event handler attribute name
// (element.onclick = fn / on*="code"), or nullptr when no handler exists for
// the type.  HTML spec §8.1.7.2.
const char* OnHandlerForType(std::string_view type)
{
  if (type == "click")
    return "onclick";
  if (type == "dblclick")
    return "ondblclick";
  if (type == "mousedown")
    return "onmousedown";
  if (type == "mouseup")
    return "onmouseup";
  if (type == "mousemove")
    return "onmousemove";
  if (type == "mouseover")
    return "onmouseover";
  if (type == "mouseout")
    return "onmouseout";
  if (type == "focus")
    return "onfocus";
  if (type == "blur")
    return "onblur";
  if (type == "keydown")
    return "onkeydown";
  if (type == "keyup")
    return "onkeyup";
  if (type == "input")
    return "oninput";
  if (type == "change")
    return "onchange";
  if (type == "submit")
    return "onsubmit";
  if (type == "wheel")
    return "onwheel";
  if (type == "load")
    return "onload";
  if (type == "error")
    return "onerror";
  if (type == "scroll")
    return "onscroll";
  return nullptr;
}

// Element-level global event handler IDL attributes (onclick/oninput/...).
// IDL handlers are stored by Impl. A content attribute (on*="code") is
// compiled when it fires.
static constexpr std::array<const char*, 19> kElementEventHandlers = {"onclick",
                                                                      "ondblclick",
                                                                      "onmousedown",
                                                                      "onmouseup",
                                                                      "onmousemove",
                                                                      "onmouseover",
                                                                      "onmouseout",
                                                                      "onfocus",
                                                                      "onblur",
                                                                      "onkeydown",
                                                                      "onkeyup",
                                                                      "oninput",
                                                                      "onchange",
                                                                      "onsubmit",
                                                                      "onwheel",
                                                                      "onload",
                                                                      "onerror",
                                                                      "onscroll",
                                                                      "onresize"};

// The getter/setter are JS_CFUNC_getter_magic / JS_CFUNC_setter_magic: the
// magic (handler index) arrives as the trailing int argument (no argc/argv).
JSValue ElementGetEventHandler(JSContext* ctx, JSValueConst this_val, int magic)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr || magic < 0 ||
      magic >= static_cast<int>(kElementEventHandlers.size())) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const auto node_it = impl->event_handlers.find(node);
  if (node_it == impl->event_handlers.end()) {
    return JS_NULL;
  }
  const auto handler_it =
      node_it->second.find(kElementEventHandlers[static_cast<std::size_t>(magic)]);
  if (handler_it == node_it->second.end()) {
    return JS_NULL;
  }
  return JS_DupValue(ctx, handler_it->second);
}

JSValue ElementSetEventHandler(JSContext* ctx, JSValueConst this_val, JSValueConst value, int magic)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr || magic < 0 ||
      magic >= static_cast<int>(kElementEventHandlers.size())) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string name = kElementEventHandlers[static_cast<std::size_t>(magic)];
  auto& handlers = impl->event_handlers[node];
  const auto existing = handlers.find(name);
  if (existing != handlers.end()) {
    JS_FreeValue(ctx, existing->second);
  }
  handlers[name] = JS_DupValue(ctx, value);
  return JS_UNDEFINED;
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
  DefineGetter(ctx, impl.event_proto, "key", MakeGetter(ctx, "key", EventGetKey));
  DefineGetter(ctx, impl.event_proto, "code", MakeGetter(ctx, "code", EventGetCode));
  DefineGetter(ctx, impl.event_proto, "keyCode", MakeGetter(ctx, "keyCode", EventGetKeyCode));
  DefineGetter(ctx, impl.event_proto, "clientX", MakeGetter(ctx, "clientX", EventGetClientX));
  DefineGetter(ctx, impl.event_proto, "clientY", MakeGetter(ctx, "clientY", EventGetClientY));
  DefineGetter(ctx, impl.event_proto, "button", MakeGetter(ctx, "button", EventGetButton));
  DefineGetter(ctx, impl.event_proto, "deltaY", MakeGetter(ctx, "deltaY", EventGetDeltaY));
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
  static const std::array<JSCFunctionListEntry, 15> kMethods = {{
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
      JS_CFUNC_DEF("insertAdjacentHTML", 2, ElementInsertAdjacentHTML),
      JS_CFUNC_DEF("getBoundingClientRect", 0, ElementGetBoundingClientRect),
      JS_CFUNC_DEF("play", 0, ElementPlayVideo),
      JS_CFUNC_DEF("pause", 0, ElementPauseVideo),
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
  // Element layout geometry (getBoundingClientRect + offset/client series).
  DefineGetter(ctx,
               impl.element_proto,
               "offsetWidth",
               MakeGetter(ctx, "offsetWidth", ElementGetOffsetWidth));
  DefineGetter(ctx,
               impl.element_proto,
               "offsetHeight",
               MakeGetter(ctx, "offsetHeight", ElementGetOffsetHeight));
  DefineGetter(
      ctx, impl.element_proto, "offsetTop", MakeGetter(ctx, "offsetTop", ElementGetOffsetTop));
  DefineGetter(
      ctx, impl.element_proto, "offsetLeft", MakeGetter(ctx, "offsetLeft", ElementGetOffsetLeft));
  DefineGetter(ctx,
               impl.element_proto,
               "offsetParent",
               MakeGetter(ctx, "offsetParent", ElementGetOffsetParent));
  DefineGetter(ctx,
               impl.element_proto,
               "clientWidth",
               MakeGetter(ctx, "clientWidth", ElementGetClientWidth));
  DefineGetter(ctx,
               impl.element_proto,
               "clientHeight",
               MakeGetter(ctx, "clientHeight", ElementGetClientHeight));
  DefineGetter(
      ctx, impl.element_proto, "clientTop", MakeGetter(ctx, "clientTop", ElementGetClientTop));
  DefineGetter(
      ctx, impl.element_proto, "clientLeft", MakeGetter(ctx, "clientLeft", ElementGetClientLeft));
  // Element-level global event handler attributes (element.onclick = fn).
  for (int i = 0; i < static_cast<int>(kElementEventHandlers.size()); ++i) {
    DefineAccessor(
        ctx,
        impl.element_proto,
        kElementEventHandlers[static_cast<std::size_t>(i)],
        MakeGetterMagic(
            ctx, kElementEventHandlers[static_cast<std::size_t>(i)], ElementGetEventHandler, i),
        MakeSetterMagic(
            ctx, kElementEventHandlers[static_cast<std::size_t>(i)], ElementSetEventHandler, i));
  }
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
  for (const auto& [name, part] :
       std::array<std::pair<const char*, AnchorUrlPart>, 7>{{{"protocol", AnchorUrlPart::kProtocol},
                                                               {"host", AnchorUrlPart::kHost},
                                                               {"hostname", AnchorUrlPart::kHostname},
                                                               {"port", AnchorUrlPart::kPort},
                                                               {"pathname", AnchorUrlPart::kPathname},
                                                               {"search", AnchorUrlPart::kSearch},
                                                               {"hash", AnchorUrlPart::kHash}}}) {
    DefineAccessor(ctx,
                   impl.element_proto,
                   name,
                   MakeGetterMagic(ctx, name, ElementGetAnchorUrlPart, static_cast<int>(part)),
                   JS_UNDEFINED);
  }
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
  // Media (<video>): duration/currentTime/paused + play()/pause() (methods
  // above).  currentTime is seekable; non-video elements report the media
  // defaults (NaN duration, paused = true).
  DefineGetter(
      ctx, impl.element_proto, "duration", MakeGetter(ctx, "duration", ElementGetVideoDuration));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "currentTime",
                 MakeGetter(ctx, "currentTime", ElementGetVideoCurrentTime),
                 MakeSetter(ctx, "currentTime", ElementSetVideoCurrentTime));
  DefineGetter(ctx, impl.element_proto, "paused", MakeGetter(ctx, "paused", ElementGetVideoPaused));
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
               "implementation",
               MakeGetter(ctx, "implementation", DocGetImplementation));
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
  DefineAccessor(ctx,
                 impl.document_proto,
                 "cookie",
                 MakeGetter(ctx, "cookie", DocGetCookie),
                 MakeSetter(ctx, "cookie", DocSetCookie));
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
  DefineGetter(ctx,
               impl.document_proto,
               "currentScript",
               MakeGetter(ctx, "currentScript", DocGetCurrentScript));
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
  std::size_t path_start = 0;
  const std::size_t scheme_end = href.find("://");
  if (scheme_end == std::string_view::npos) {
    // Bare path (no scheme, e.g. a local form submission target): the whole
    // string is the path plus an optional query/fragment.
    const std::size_t colon = href.find(':');
    if (colon != std::string_view::npos) {
      out.protocol = std::string(href.substr(0, colon + 1));
    }
  } else {
    out.protocol = std::string(href.substr(0, scheme_end + 1));
    std::size_t i = scheme_end + 3;
    const std::size_t ps = href.find_first_of("/?#", i);
    const std::size_t host_end = ps == std::string_view::npos ? href.size() : ps;
    out.host = std::string(href.substr(i, host_end - i));
    out.origin = out.protocol + "//" + out.host;
    const std::size_t colon = out.host.rfind(':');
    if (colon != std::string::npos) {
      out.hostname = out.host.substr(0, colon);
      out.port = out.host.substr(colon + 1);
    } else {
      out.hostname = out.host;
    }
    path_start = ps == std::string_view::npos ? href.size() : ps;
  }

  // Path / search / hash extraction shared by both cases.
  const std::size_t q = href.find('?', path_start);
  const std::size_t h = href.find('#', path_start);
  const std::size_t path_end = std::min(q == std::string_view::npos ? href.size() : q,
                                        h == std::string_view::npos ? href.size() : h);
  if (path_start < href.size()) {
    out.pathname = std::string(href.substr(path_start, path_end - path_start));
  }
  if (q != std::string_view::npos) {
    const std::size_t search_end = h == std::string_view::npos ? href.size() : h;
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

JSValue BlobText(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  JSValue data = JS_GetPropertyStr(ctx, this_val, "_nekoData");
  JSValue promise = ResolvePromise(ctx, data);
  JS_FreeValue(ctx, data);
  return promise;
}

JSValue BlobArrayBuffer(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  JSValue data = JS_GetPropertyStr(ctx, this_val, "_nekoData");
  const char* text = JS_ToCString(ctx, data);
  if (text == nullptr) {
    JS_FreeValue(ctx, data);
    return JS_EXCEPTION;
  }
  const std::size_t size = std::strlen(text);
  JSValue buffer = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(text), size);
  JS_FreeCString(ctx, text);
  JS_FreeValue(ctx, data);
  return ResolvePromise(ctx, buffer);
}

JSValue BlobConstructor(JSContext* ctx, JSValueConst /*new_target*/, int argc, JSValueConst* argv)
{
  JSValue blob = JS_NewObject(ctx);
  std::string data;
  if (argc >= 1 && JS_IsArray(argv[0])) {
    JSValue length = JS_GetPropertyStr(ctx, argv[0], "length");
    int32_t count = 0;
    (void)JS_ToInt32(ctx, &count, length);
    JS_FreeValue(ctx, length);
    for (int32_t i = 0; i < count; ++i) {
      JSValue part = JS_GetPropertyUint32(ctx, argv[0], static_cast<uint32_t>(i));
      const char* text = JS_ToCString(ctx, part);
      if (text != nullptr) {
        data += text;
        JS_FreeCString(ctx, text);
      } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
      }
      JS_FreeValue(ctx, part);
    }
  } else if (argc >= 1) {
    const char* text = JS_ToCString(ctx, argv[0]);
    if (text != nullptr) {
      data = text;
      JS_FreeCString(ctx, text);
    } else {
      JS_FreeValue(ctx, JS_GetException(ctx));
    }
  }
  JS_SetPropertyStr(ctx, blob, "_nekoData", JS_NewStringLen(ctx, data.data(), data.size()));
  JS_SetPropertyStr(ctx, blob, "size", JS_NewInt64(ctx, static_cast<int64_t>(data.size())));
  std::string type;
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue type_value = JS_GetPropertyStr(ctx, argv[1], "type");
    const char* text = JS_ToCString(ctx, type_value);
    if (text != nullptr) {
      type = text;
      JS_FreeCString(ctx, text);
    }
    JS_FreeValue(ctx, type_value);
  }
  JS_SetPropertyStr(ctx, blob, "type", JS_NewString(ctx, type.c_str()));
  JS_SetPropertyStr(ctx, blob, "text", JS_NewCFunction(ctx, BlobText, "text", 0));
  JS_SetPropertyStr(ctx, blob, "arrayBuffer", JS_NewCFunction(ctx, BlobArrayBuffer, "arrayBuffer", 0));
  return blob;
}

JSValue UrlCreateObjectUrl(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  if (argc < 1 || !JS_IsObject(argv[0])) {
    return JS_ThrowTypeError(ctx, "createObjectURL requires an object");
  }
  static std::atomic<uint64_t> next_id{1};
  const std::string value = "blob:neko/" + std::to_string(next_id.fetch_add(1));
  return JS_NewString(ctx, value.c_str());
}

JSValue UrlRevokeObjectUrl(JSContext* /*ctx*/, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
{
  return JS_UNDEFINED;
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
// window.indexedDB — a practical subset of the IndexedDB API.  Databases
// with versionchange upgrades, object stores with key paths and
// auto-increment keys, and transactions carrying add/put/get/delete/clear/
// count/getAll.  Results are delivered through microtasks (QuickJS jobs)
// with onsuccess/onerror handler properties; the storage layer itself is
// updated synchronously when each request is issued.
//
// Documented deviations/limitations:
//   * no cursors, indexes, or key ranges;
//   * keys are numbers or strings only;
//   * values use the JSON clone subset (no Date/BinaryData; cycles fail the
//     request with DataCloneError);
//   * transactions auto-commit when their last request completes;
//   * errors are Error objects with a DOMException-like .name.
// ---------------------------------------------------------------------------

// Splits a storage-layer error of the form "IDB:<Name>:<message>".
void SplitIdbError(const base::Error& error, std::string& name, std::string& message)
{
  name = "UnknownError";
  message = error.message();
  if (message.rfind("IDB:", 0) == 0) {
    const std::size_t colon = message.find(':', 4);
    if (colon != std::string::npos) {
      name = message.substr(4, colon - 4);
      message = message.substr(colon + 1);
    }
  }
}

// An Error object carrying a DOMException-like .name.
JSValue MakeIdbErrorValue(JSContext* ctx, const std::string& name, const std::string& message)
{
  JSValue err = JS_NewError(ctx);
  JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, message.c_str()));
  JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, name.c_str()));
  return err;
}

JSValue MakeIdbErrorFromBase(JSContext* ctx, const base::Error& error)
{
  std::string name, message;
  SplitIdbError(error, name, message);
  return MakeIdbErrorValue(ctx, name, message);
}

JSValue ThrowIdbError(JSContext* ctx, const std::string& name, const std::string& message)
{
  return JS_Throw(ctx, MakeIdbErrorValue(ctx, name, message));
}

// Invokes |target|'s |prop| handler (a function) with |event|, if set.
// Handler exceptions are consumed (mirroring the DOM event dispatch paths).
void CallIdbHandler(JSContext* ctx, JSValueConst target, const char* prop, JSValueConst event)
{
  JSValue handler = JS_GetPropertyStr(ctx, target, prop);
  if (!JS_IsFunction(ctx, handler)) {
    JS_FreeValue(ctx, handler);
    return;
  }
  JSValueConst argv[] = {event};
  JSValue ret = JS_Call(ctx, handler, target, 1, argv);
  if (JS_IsException(ret)) {
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, JS_GetException(ctx));
  } else {
    JS_FreeValue(ctx, ret);
  }
  JS_FreeValue(ctx, handler);
}

// Returns the IdbHandle addressed by |func_data[0]| (an int32 index).
std::shared_ptr<Impl::IdbHandle> IdbHandleFromData(JSContext* ctx, JSValueConst* func_data)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  if (impl == nullptr) {
    return nullptr;
  }
  int32_t idx = 0;
  if (JS_ToInt32(ctx, &idx, func_data[0]) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return nullptr;
  }
  if (idx < 0 || static_cast<std::size_t>(idx) >= impl->idb_handles.size()) {
    return nullptr;
  }
  return impl->idb_handles[static_cast<std::size_t>(idx)];
}

// JSON-serializes |value| for the structured-clone subset.  Returns nullopt
// when the value cannot be cloned (cycles etc.); the exception is consumed.
std::optional<std::string> IdbCloneToJson(JSContext* ctx, JSValueConst value)
{
  JSValue json = JS_JSONStringify(ctx, value, JS_UNDEFINED, JS_UNDEFINED);
  if (JS_IsException(json)) {
    JS_FreeValue(ctx, json);
    JS_FreeValue(ctx, JS_GetException(ctx));
    return std::nullopt;
  }
  const char* text = JS_ToCString(ctx, json);
  std::string out = text != nullptr ? text : "";
  JS_FreeCString(ctx, text);
  JS_FreeValue(ctx, json);
  return out;
}

// Serializes an explicit key argument (a JSON number or string).  Returns
// nullopt for unsupported key types.
std::optional<std::string> IdbKeyToJson(JSContext* ctx, JSValueConst key)
{
  if (!JS_IsNumber(key) && !JS_IsString(key)) {
    return std::nullopt;
  }
  return IdbCloneToJson(ctx, key);
}

// Parses JSON text back to a JS value; undefined when parsing fails.
JSValue IdbJsonToJs(JSContext* ctx, const std::string& json)
{
  JSValue parsed = JS_ParseJSON(ctx, json.c_str(), json.size(), "<indexeddb>");
  if (JS_IsException(parsed)) {
    JS_FreeValue(ctx, parsed);
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_UNDEFINED;
  }
  return parsed;
}

// Common completion of a request: sets result/error/readyState and fires the
// matching handler.  |result| and |error_value| are consumed.
void SettleIdbRequest(JSContext* ctx,
                      JSValueConst req_obj,
                      const char* handler_prop,
                      const char* event_type,
                      bool success,
                      JSValueConst result,
                      JSValueConst error_value)
{
  if (success) {
    JS_SetPropertyStr(ctx, req_obj, "result", result); // steals the dup
    JS_SetPropertyStr(ctx, req_obj, "error", JS_UNDEFINED);
  } else {
    JS_SetPropertyStr(ctx, req_obj, "error", error_value); // steals the dup
  }
  JS_SetPropertyStr(ctx, req_obj, "readyState", JS_NewString(ctx, "done"));
  JSValue event = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, event_type)); // steals
  JS_SetPropertyStr(ctx, event, "target", JS_DupValue(ctx, req_obj));
  CallIdbHandler(ctx, req_obj, handler_prop, event);
  JS_FreeValue(ctx, event);
}

// Completes a data request (add/put/get/delete/clear/count/getAll) and
// commits its transaction when it was the last outstanding request.
void SettleIdbDataRequest(Impl* impl,
                          JSContext* ctx,
                          JSValueConst req_obj,
                          const std::shared_ptr<Impl::IdbRequest>& request)
{
  if (!request->error_name.empty()) {
    JSValue err = MakeIdbErrorValue(ctx, request->error_name, request->error_message);
    SettleIdbRequest(ctx, req_obj, "onerror", "error", false, JS_UNDEFINED, err);
  } else {
    JSValue result = JS_UNDEFINED;
    if (request->has_result && request->result_json.has_value()) {
      result = IdbJsonToJs(ctx, *request->result_json);
    }
    SettleIdbRequest(ctx, req_obj, "onsuccess", "success", true, result, JS_UNDEFINED);
  }
  if (request->tx_handle >= 0 &&
      static_cast<std::size_t>(request->tx_handle) < impl->idb_handles.size()) {
    const std::shared_ptr<Impl::IdbHandle>& tx =
        impl->idb_handles[static_cast<std::size_t>(request->tx_handle)];
    if (tx->kind == Impl::IdbHandle::Kind::kTransaction && tx->pending > 0) {
      --tx->pending;
      if (tx->pending == 0 && !tx->completed && !tx->aborted) {
        tx->completed = true;
        if (!JS_IsUndefined(tx->object)) {
          JSValue event = JS_NewObject(ctx);
          JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, "complete"));
          JS_SetPropertyStr(ctx, event, "target", JS_DupValue(ctx, tx->object));
          CallIdbHandler(ctx, tx->object, "oncomplete", event);
          JS_FreeValue(ctx, event);
        }
      }
    }
  }
}

// Creates the JS object model for a database handle (name/version/
// objectStoreNames + methods).
JSValue MakeIdbDatabaseObject(Impl* impl, JSContext* ctx, int handle_idx);

JSValue MakeIdbTransactionObject(Impl* impl, JSContext* ctx, int handle_idx);

JSValue MakeIdbStoreObject(Impl* impl, JSContext* ctx, int handle_idx);

int IdbHandleIndexFromData(JSContext* ctx, JSValueConst* func_data);

// Loads the object-store metadata of |db_name| into |handle->stores|.
void IdbLoadStoreMetas(Impl* impl,
                       const std::string& db_name,
                       const std::shared_ptr<Impl::IdbHandle>& handle)
{
  handle->stores.clear();
  const base::Result<std::vector<IdbStoreMeta>> metas = impl->apis.idb_store_names(db_name);
  if (!metas) {
    return;
  }
  for (const IdbStoreMeta& meta : metas.value()) {
    Impl::IdbStoreInfo info;
    info.name = meta.name;
    info.key_path = meta.key_path;
    info.auto_increment = meta.auto_increment;
    handle->stores.push_back(std::move(info));
  }
}

// Completes an open()/deleteDatabase() request: handles version-change
// upgrades (onupgradeneeded) and plain opens.
void SettleIdbOpenRequest(Impl* impl,
                          JSContext* ctx,
                          JSValueConst req_obj,
                          const std::shared_ptr<Impl::IdbRequest>& request)
{
  if (!request->error_name.empty()) {
    JSValue err = MakeIdbErrorValue(ctx, request->error_name, request->error_message);
    SettleIdbRequest(ctx, req_obj, "onerror", "error", false, JS_UNDEFINED, err);
    return;
  }

  auto fail = [&](const base::Error& error) {
    std::string name, message;
    SplitIdbError(error, name, message);
    JSValue err = MakeIdbErrorValue(ctx, name, message);
    SettleIdbRequest(ctx, req_obj, "onerror", "error", false, JS_UNDEFINED, err);
  };

  if (request->is_delete_db) {
    const base::Result<void> removed = impl->apis.idb_delete_db(request->db_name);
    if (!removed) {
      fail(removed.error());
      return;
    }
    SettleIdbRequest(ctx, req_obj, "onsuccess", "success", true, JS_UNDEFINED, JS_UNDEFINED);
    return;
  }

  const std::string& name = request->db_name;
  const base::Result<int64_t> current = impl->apis.idb_current_version(name);
  if (!current) {
    fail(current.error());
    return;
  }
  int64_t new_version = 0;
  bool upgrade = false;
  if (request->requested_version > 0) {
    if (request->requested_version < current.value()) {
      fail(base::Error::InvalidArgument(
          "IDB:VersionError:requested version is lower than the database version"));
      return;
    }
    if (request->requested_version > current.value()) {
      upgrade = true;
      new_version = request->requested_version;
    }
  } else if (current.value() == 0) {
    // Spec: opening a missing database creates version 1 and upgrades.
    upgrade = true;
    new_version = 1;
  }

  const int handle_idx = static_cast<int>(impl->idb_handles.size());
  auto handle = std::make_shared<Impl::IdbHandle>();
  handle->kind = Impl::IdbHandle::Kind::kDatabase;
  handle->db_name = name;
  handle->version = current.value();
  impl->idb_handles.push_back(handle);
  IdbLoadStoreMetas(impl, name, handle);

  if (upgrade) {
    if (current.value() == 0) {
      const base::Result<int64_t> created = impl->apis.idb_create_db(name);
      if (!created) {
        fail(created.error());
        return;
      }
    }
    handle->upgrade = true;
    JSValue db_obj = MakeIdbDatabaseObject(impl, ctx, handle_idx);
    // The version-change transaction.
    const int tx_idx = static_cast<int>(impl->idb_handles.size());
    auto tx = std::make_shared<Impl::IdbHandle>();
    tx->kind = Impl::IdbHandle::Kind::kTransaction;
    tx->db_name = name;
    tx->mode = "versionchange";
    tx->db_handle = handle_idx;
    impl->idb_handles.push_back(tx);
    JSValue tx_obj = MakeIdbTransactionObject(impl, ctx, tx_idx);

    JS_SetPropertyStr(ctx, req_obj, "result", db_obj); // steals db_obj
    JS_SetPropertyStr(ctx, req_obj, "transaction", JS_DupValue(ctx, tx_obj));
    JSValue event = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, "upgradeneeded"));
    JS_SetPropertyStr(ctx, event, "target", JS_DupValue(ctx, req_obj));
    JS_SetPropertyStr(ctx, event, "oldVersion", JS_NewInt64(ctx, current.value()));
    JS_SetPropertyStr(ctx, event, "newVersion", JS_NewInt64(ctx, new_version));
    JS_SetPropertyStr(ctx, event, "transaction", JS_DupValue(ctx, tx_obj));
    CallIdbHandler(ctx, req_obj, "onupgradeneeded", event);
    JS_FreeValue(ctx, event);
    JS_FreeValue(ctx, tx_obj); // the tx handle still dups it

    const base::Result<void> versioned = impl->apis.idb_set_version(name, new_version);
    if (!versioned) {
      fail(versioned.error());
      return;
    }
    handle->version = new_version;
    handle->upgrade = false;
    IdbLoadStoreMetas(impl, name, handle);

    // Refresh the db object's version + store names, then complete.
    JS_SetPropertyStr(ctx, db_obj, "version", JS_NewInt64(ctx, new_version));
    JSValue names = JS_NewArray(ctx);
    for (std::size_t i = 0; i < handle->stores.size(); ++i) {
      JS_SetPropertyUint32(
          ctx, names, static_cast<uint32_t>(i), JS_NewString(ctx, handle->stores[i].name.c_str()));
    }
    JS_SetPropertyStr(ctx, db_obj, "objectStoreNames", names); // steals names
    // The version-change transaction completes, then the request succeeds.
    JSValue complete_event = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, complete_event, "type", JS_NewString(ctx, "complete"));
    JS_SetPropertyStr(ctx, complete_event, "target", JS_DupValue(ctx, tx->object));
    CallIdbHandler(ctx, tx->object, "oncomplete", complete_event);
    JS_FreeValue(ctx, complete_event);
    SettleIdbRequest(
        ctx, req_obj, "onsuccess", "success", true, JS_DupValue(ctx, db_obj), JS_UNDEFINED);
  } else {
    JSValue db_obj = MakeIdbDatabaseObject(impl, ctx, handle_idx);
    JS_SetPropertyStr(ctx, req_obj, "transaction", JS_UNDEFINED);
    SettleIdbRequest(ctx, req_obj, "onsuccess", "success", true, db_obj, JS_UNDEFINED);
  }
}

// The QuickJS job entry for request completions.  argv[0] = request object,
// argv[1] = request index (int32).
JSValue IdbJobEntry(JSContext* ctx, int argc, JSValueConst* argv)
{
  if (argc < 2) {
    return JS_UNDEFINED;
  }
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  if (impl == nullptr) {
    return JS_UNDEFINED;
  }
  int32_t idx = 0;
  if (JS_ToInt32(ctx, &idx, argv[1]) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_UNDEFINED;
  }
  if (idx < 0 || static_cast<std::size_t>(idx) >= impl->idb_requests.size()) {
    return JS_UNDEFINED;
  }
  const std::shared_ptr<Impl::IdbRequest> request =
      impl->idb_requests[static_cast<std::size_t>(idx)];
  if (request->is_open || request->is_delete_db) {
    SettleIdbOpenRequest(impl, ctx, argv[0], request);
  } else {
    SettleIdbDataRequest(impl, ctx, argv[0], request);
  }
  return JS_UNDEFINED;
}

// Allocates a request state + JS request object and enqueues its completion
// job.  The returned JSValue is owned by the caller (the API returns it to
// the script); the job queue holds its own dup.
JSValue MakeIdbRequest(Impl* impl,
                       JSContext* ctx,
                       std::shared_ptr<Impl::IdbRequest> request,
                       bool upgradeneeded_slot)
{
  JSValue req_obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, req_obj, "result", JS_UNDEFINED);
  JS_SetPropertyStr(ctx, req_obj, "error", JS_UNDEFINED);
  JS_SetPropertyStr(ctx, req_obj, "readyState", JS_NewString(ctx, "pending"));
  JS_SetPropertyStr(ctx, req_obj, "onsuccess", JS_UNDEFINED);
  JS_SetPropertyStr(ctx, req_obj, "onerror", JS_UNDEFINED);
  if (upgradeneeded_slot) {
    JS_SetPropertyStr(ctx, req_obj, "onupgradeneeded", JS_UNDEFINED);
    JS_SetPropertyStr(ctx, req_obj, "transaction", JS_UNDEFINED);
  }
  const int32_t idx = static_cast<int32_t>(impl->idb_requests.size());
  impl->idb_requests.push_back(std::move(request));
  JSValue args[2];
  args[0] = JS_DupValue(ctx, req_obj);
  args[1] = JS_NewInt32(ctx, idx);
  JS_EnqueueJob(ctx, IdbJobEntry, 2, args);
  JS_FreeValue(ctx, args[0]);
  JS_FreeValue(ctx, args[1]);
  return req_obj;
}

// ---- method dispatchers ----------------------------------------------------

JSValue IdbOpen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.idb_current_version) {
    return JS_ThrowTypeError(ctx, "indexedDB is not available");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "indexedDB.open requires a database name");
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  int64_t version = 0;
  if (argc >= 2 && !JS_IsUndefined(argv[1])) {
    if (JS_ToInt64(ctx, &version, argv[1]) != 0) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return JS_ThrowTypeError(ctx, "indexedDB.open version must be an integer");
    }
    if (version < 0) {
      return JS_ThrowTypeError(ctx, "indexedDB.open version must not be negative");
    }
  }
  auto request = std::make_shared<Impl::IdbRequest>();
  request->is_open = true;
  request->db_name = name;
  request->requested_version = version;
  return MakeIdbRequest(impl, ctx, std::move(request), /*upgradeneeded_slot=*/true);
}

JSValue IdbDeleteDatabase(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.idb_delete_db) {
    return JS_ThrowTypeError(ctx, "indexedDB is not available");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "indexedDB.deleteDatabase requires a database name");
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  auto request = std::make_shared<Impl::IdbRequest>();
  request->is_delete_db = true;
  request->db_name = name;
  return MakeIdbRequest(impl, ctx, std::move(request), /*upgradeneeded_slot=*/false);
}

JSValue IdbDatabaseCreateObjectStore(JSContext* ctx,
                                     JSValueConst /*this_val*/,
                                     int argc,
                                     JSValueConst* argv,
                                     int /*magic*/,
                                     JSValueConst* func_data)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  const std::shared_ptr<Impl::IdbHandle> handle = IdbHandleFromData(ctx, func_data);
  if (impl == nullptr || handle == nullptr) {
    return JS_ThrowTypeError(ctx, "stale indexedDB object");
  }
  if (!handle->upgrade) {
    return ThrowIdbError(
        ctx, "InvalidStateError", "createObjectStore is only allowed during a version change");
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  for (const Impl::IdbStoreInfo& info : handle->stores) {
    if (info.name == name) {
      return ThrowIdbError(ctx, "ConstraintError", "an object store with this name already exists");
    }
  }
  std::string key_path;
  bool auto_increment = false;
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue kp = JS_GetPropertyStr(ctx, argv[1], "keyPath");
    if (JS_IsString(kp)) {
      key_path = ArgString(ctx, kp, &ok);
      if (!ok) {
        JS_FreeValue(ctx, kp);
        return JS_EXCEPTION;
      }
    }
    JS_FreeValue(ctx, kp);
    JSValue ai = JS_GetPropertyStr(ctx, argv[1], "autoIncrement");
    if (JS_IsBool(ai)) {
      auto_increment = JS_ToBool(ctx, ai) != 0;
    }
    JS_FreeValue(ctx, ai);
  }
  const base::Result<void> created =
      impl->apis.idb_create_store(handle->db_name, name, key_path, auto_increment);
  if (!created) {
    return JS_Throw(ctx, MakeIdbErrorFromBase(ctx, created.error()));
  }
  Impl::IdbStoreInfo info;
  info.name = name;
  info.key_path = key_path;
  info.auto_increment = auto_increment;
  handle->stores.push_back(std::move(info));
  // Return a store object carrying the metadata (looked up through the
  // owning database handle).
  const int db_idx = IdbHandleIndexFromData(ctx, func_data);
  const int store_idx = static_cast<int>(impl->idb_handles.size());
  auto store_handle = std::make_shared<Impl::IdbHandle>();
  store_handle->kind = Impl::IdbHandle::Kind::kObjectStore;
  store_handle->db_name = handle->db_name;
  store_handle->store_name = name;
  store_handle->db_handle = db_idx;
  impl->idb_handles.push_back(store_handle);
  return MakeIdbStoreObject(impl, ctx, store_idx);
}

JSValue IdbDatabaseDeleteObjectStore(JSContext* ctx,
                                     JSValueConst /*this_val*/,
                                     int argc,
                                     JSValueConst* argv,
                                     int /*magic*/,
                                     JSValueConst* func_data)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  const std::shared_ptr<Impl::IdbHandle> handle = IdbHandleFromData(ctx, func_data);
  if (impl == nullptr || handle == nullptr) {
    return JS_ThrowTypeError(ctx, "stale indexedDB object");
  }
  if (!handle->upgrade) {
    return ThrowIdbError(
        ctx, "InvalidStateError", "deleteObjectStore is only allowed during a version change");
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  bool found = false;
  for (auto it = handle->stores.begin(); it != handle->stores.end(); ++it) {
    if (it->name == name) {
      handle->stores.erase(it);
      found = true;
      break;
    }
  }
  if (!found) {
    return ThrowIdbError(ctx, "NotFoundError", "no object store with this name");
  }
  const base::Result<void> removed = impl->apis.idb_delete_store(handle->db_name, name);
  if (!removed) {
    return JS_Throw(ctx, MakeIdbErrorFromBase(ctx, removed.error()));
  }
  return JS_UNDEFINED;
}

// Shared logic for object-store data operations.
JSValue IdbStoreOp(
    JSContext* ctx, int argc, JSValueConst* argv, JSValueConst* func_data, const std::string& op)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  const std::shared_ptr<Impl::IdbHandle> store = IdbHandleFromData(ctx, func_data);
  if (impl == nullptr || store == nullptr) {
    return JS_ThrowTypeError(ctx, "stale indexedDB object");
  }
  if (store->tx_handle < 0 ||
      static_cast<std::size_t>(store->tx_handle) >= impl->idb_handles.size()) {
    return ThrowIdbError(ctx, "InvalidStateError", "transaction is no longer active");
  }
  const std::shared_ptr<Impl::IdbHandle>& tx =
      impl->idb_handles[static_cast<std::size_t>(store->tx_handle)];
  if (tx->aborted) {
    return ThrowIdbError(ctx, "TransactionInactiveError", "transaction was aborted");
  }
  const bool is_write = op != "get" && op != "getAll" && op != "count";
  if (is_write && tx->mode == "readonly") {
    return ThrowIdbError(ctx, "ReadOnlyError", "transaction is read-only");
  }
  if ((op == "add" || op == "put" || op == "get" || op == "delete") &&
      (argc < 1 || JS_IsUndefined(argv[0]))) {
    return JS_ThrowTypeError(ctx, "missing argument");
  }

  auto request = std::make_shared<Impl::IdbRequest>();
  request->tx_handle = store->tx_handle;

  auto run = [&]() {
    // Serialize the value (add/put).
    std::string value_json;
    if (op == "add" || op == "put") {
      const std::optional<std::string> cloned = IdbCloneToJson(ctx, argv[0]);
      if (!cloned.has_value()) {
        request->error_name = "DataCloneError";
        request->error_message = "value could not be cloned";
        return;
      }
      value_json = *cloned;
    }
    // Serialize the key (add/put/get/delete).
    std::optional<std::string> key_json;
    if (op == "add" || op == "put") {
      if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        key_json = IdbKeyToJson(ctx, argv[1]);
        if (!key_json.has_value()) {
          request->error_name = "DataError";
          request->error_message = "key must be a number or a string";
          return;
        }
      }
    } else if (op == "get" || op == "delete") {
      key_json = IdbKeyToJson(ctx, argv[0]);
      if (!key_json.has_value()) {
        request->error_name = "DataError";
        request->error_message = "key must be a number or a string";
        return;
      }
    }
    // Execute against the storage layer (synchronous; results settle async).
    if (op == "add") {
      const base::Result<std::string> r =
          impl->apis.idb_add(store->db_name, store->store_name, key_json, value_json);
      if (!r) {
        SplitIdbError(r.error(), request->error_name, request->error_message);
        return;
      }
      request->result_json = r.value();
      request->has_result = true;
    } else if (op == "put") {
      const base::Result<std::string> r =
          impl->apis.idb_put(store->db_name, store->store_name, key_json, value_json);
      if (!r) {
        SplitIdbError(r.error(), request->error_name, request->error_message);
        return;
      }
      request->result_json = r.value();
      request->has_result = true;
    } else if (op == "get") {
      const base::Result<std::optional<std::string>> r =
          impl->apis.idb_get(store->db_name, store->store_name, *key_json);
      if (!r) {
        SplitIdbError(r.error(), request->error_name, request->error_message);
        return;
      }
      if (r.value().has_value()) {
        request->result_json = *r.value();
        request->has_result = true;
      }
    } else if (op == "delete") {
      const base::Result<void> r =
          impl->apis.idb_delete(store->db_name, store->store_name, *key_json);
      if (!r) {
        SplitIdbError(r.error(), request->error_name, request->error_message);
        return;
      }
    } else if (op == "clear") {
      const base::Result<void> r = impl->apis.idb_clear(store->db_name, store->store_name);
      if (!r) {
        SplitIdbError(r.error(), request->error_name, request->error_message);
        return;
      }
    } else if (op == "count") {
      const base::Result<int64_t> r = impl->apis.idb_count(store->db_name, store->store_name);
      if (!r) {
        SplitIdbError(r.error(), request->error_name, request->error_message);
        return;
      }
      request->result_json = std::to_string(r.value());
      request->has_result = true;
    } else if (op == "getAll") {
      const base::Result<std::vector<std::string>> r =
          impl->apis.idb_get_all(store->db_name, store->store_name);
      if (!r) {
        SplitIdbError(r.error(), request->error_name, request->error_message);
        return;
      }
      std::string array_json = "[";
      for (std::size_t i = 0; i < r.value().size(); ++i) {
        if (i != 0) {
          array_json.push_back(',');
        }
        array_json += r.value()[i];
      }
      array_json.push_back(']');
      request->result_json = std::move(array_json);
      request->has_result = true;
    }
  };
  run();
  ++tx->pending;
  return MakeIdbRequest(impl, ctx, std::move(request), /*upgradeneeded_slot=*/false);
}

#define NEKO_IDB_STORE_OP(fn_name, op_name, length)                                                \
  JSValue fn_name(JSContext* ctx,                                                                  \
                  JSValueConst /*this_val*/,                                                       \
                  int argc,                                                                        \
                  JSValueConst* argv,                                                              \
                  int /*magic*/,                                                                   \
                  JSValueConst* func_data)                                                         \
  {                                                                                                \
    (void)length;                                                                                  \
    return IdbStoreOp(ctx, argc, argv, func_data, op_name);                                        \
  }

NEKO_IDB_STORE_OP(IdbStoreAdd, "add", 2)
NEKO_IDB_STORE_OP(IdbStorePut, "put", 2)
NEKO_IDB_STORE_OP(IdbStoreGet, "get", 1)
NEKO_IDB_STORE_OP(IdbStoreDelete, "delete", 1)
NEKO_IDB_STORE_OP(IdbStoreClear, "clear", 0)
NEKO_IDB_STORE_OP(IdbStoreCount, "count", 0)
NEKO_IDB_STORE_OP(IdbStoreGetAll, "getAll", 0)

#undef NEKO_IDB_STORE_OP

JSValue IdbDatabaseTransaction(JSContext* ctx,
                               JSValueConst /*this_val*/,
                               int argc,
                               JSValueConst* argv,
                               int /*magic*/,
                               JSValueConst* func_data)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  const std::shared_ptr<Impl::IdbHandle> db = IdbHandleFromData(ctx, func_data);
  if (impl == nullptr || db == nullptr) {
    return JS_ThrowTypeError(ctx, "stale indexedDB object");
  }
  bool ok = false;
  std::string mode = "readonly";
  if (argc >= 2 && !JS_IsUndefined(argv[1])) {
    mode = ArgString(ctx, argv[1], &ok);
    if (!ok) {
      return JS_EXCEPTION;
    }
    if (mode != "readonly" && mode != "readwrite") {
      return JS_ThrowTypeError(ctx, "transaction mode must be 'readonly' or 'readwrite'");
    }
  }
  std::vector<std::string> names;
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "transaction requires store names");
  }
  if (JS_IsArray(argv[0])) {
    JSValue len_value = JS_GetPropertyStr(ctx, argv[0], "length");
    int64_t len = 0;
    JS_ToInt64(ctx, &len, len_value);
    JS_FreeValue(ctx, len_value);
    for (int64_t i = 0; i < len; ++i) {
      JSValue item = JS_GetPropertyUint32(ctx, argv[0], static_cast<uint32_t>(i));
      std::string name = ArgString(ctx, item, &ok);
      JS_FreeValue(ctx, item);
      if (!ok) {
        return JS_EXCEPTION;
      }
      names.push_back(name);
    }
  } else {
    std::string name = ArgString(ctx, argv[0], &ok);
    if (!ok) {
      return JS_EXCEPTION;
    }
    names.push_back(name);
  }
  for (const std::string& name : names) {
    bool found = false;
    for (const Impl::IdbStoreInfo& info : db->stores) {
      if (info.name == name) {
        found = true;
        break;
      }
    }
    if (!found) {
      return ThrowIdbError(ctx, "NotFoundError", "no object store with this name");
    }
  }
  const int db_idx = IdbHandleIndexFromData(ctx, func_data);
  const int tx_idx = static_cast<int>(impl->idb_handles.size());
  auto tx = std::make_shared<Impl::IdbHandle>();
  tx->kind = Impl::IdbHandle::Kind::kTransaction;
  tx->db_name = db->db_name;
  tx->mode = mode;
  tx->db_handle = db_idx;
  impl->idb_handles.push_back(tx);
  return MakeIdbTransactionObject(impl, ctx, tx_idx);
}

JSValue IdbTransactionObjectStore(JSContext* ctx,
                                  JSValueConst /*this_val*/,
                                  int argc,
                                  JSValueConst* argv,
                                  int /*magic*/,
                                  JSValueConst* func_data)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  const std::shared_ptr<Impl::IdbHandle> tx = IdbHandleFromData(ctx, func_data);
  if (impl == nullptr || tx == nullptr) {
    return JS_ThrowTypeError(ctx, "stale indexedDB object");
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (tx->db_handle < 0 || static_cast<std::size_t>(tx->db_handle) >= impl->idb_handles.size()) {
    return ThrowIdbError(ctx, "InvalidStateError", "transaction is no longer active");
  }
  const std::shared_ptr<Impl::IdbHandle>& db =
      impl->idb_handles[static_cast<std::size_t>(tx->db_handle)];
  const Impl::IdbStoreInfo* info = nullptr;
  for (const Impl::IdbStoreInfo& candidate : db->stores) {
    if (candidate.name == name) {
      info = &candidate;
      break;
    }
  }
  if (info == nullptr) {
    return ThrowIdbError(ctx, "NotFoundError", "no object store with this name");
  }
  const int store_idx = static_cast<int>(impl->idb_handles.size());
  auto store = std::make_shared<Impl::IdbHandle>();
  store->kind = Impl::IdbHandle::Kind::kObjectStore;
  store->db_name = db->db_name;
  store->store_name = name;
  // db_handle: owning database (metadata lookups); tx_handle: the
  // transaction that requests against this store count against.
  store->db_handle = tx->db_handle;
  store->tx_handle = IdbHandleIndexFromData(ctx, func_data);
  impl->idb_handles.push_back(store);
  return MakeIdbStoreObject(impl, ctx, store_idx);
}

JSValue IdbTransactionAbort(JSContext* ctx,
                            JSValueConst /*this_val*/,
                            int /*argc*/,
                            JSValueConst* /*argv*/,
                            int /*magic*/,
                            JSValueConst* func_data)
{
  const std::shared_ptr<Impl::IdbHandle> tx = IdbHandleFromData(ctx, func_data);
  if (tx != nullptr) {
    tx->aborted = true;
    if (tx->kind == Impl::IdbHandle::Kind::kTransaction && !JS_IsUndefined(tx->object)) {
      JSValue event = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, "abort"));
      JS_SetPropertyStr(ctx, event, "target", JS_DupValue(ctx, tx->object));
      CallIdbHandler(ctx, tx->object, "onabort", event);
      JS_FreeValue(ctx, event);
    }
  }
  return JS_UNDEFINED;
}

// The transaction auto-commits when its requests settle; commit() is a
// documented no-op.
JSValue IdbTransactionCommit(JSContext* /*ctx*/,
                             JSValueConst /*this_val*/,
                             int /*argc*/,
                             JSValueConst* /*argv*/,
                             int /*magic*/,
                             JSValueConst* /*func_data*/)
{
  return JS_UNDEFINED;
}

JSValue IdbDatabaseClose(JSContext* /*ctx*/,
                         JSValueConst /*this_val*/,
                         int /*argc*/,
                         JSValueConst* /*argv*/,
                         int /*magic*/,
                         JSValueConst* /*func_data*/)
{
  return JS_UNDEFINED; // connections live for the page; close() is a no-op
}

int IdbHandleIndexFromData(JSContext* ctx, JSValueConst* func_data)
{
  int32_t idx = -1;
  if (JS_ToInt32(ctx, &idx, func_data[0]) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return -1;
  }
  return idx;
}

JSValue
BindIdbMethod(JSContext* ctx, JSValue obj, const char* name, JSCFunctionData* fn, int handle_idx)
{
  JSValue data[] = {JS_NewInt32(ctx, handle_idx)};
  JSValue bound = JS_NewCFunctionData(ctx, fn, 0, 0, 1, data);
  JS_FreeValue(ctx, data[0]);               // the function dup'd it
  JS_SetPropertyStr(ctx, obj, name, bound); // steals bound
  return obj;
}

JSValue MakeIdbDatabaseObject(Impl* impl, JSContext* ctx, int handle_idx)
{
  const std::shared_ptr<Impl::IdbHandle>& handle =
      impl->idb_handles[static_cast<std::size_t>(handle_idx)];
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, handle->db_name.c_str()));
  JS_SetPropertyStr(ctx, obj, "version", JS_NewInt64(ctx, handle->version));
  JSValue names = JS_NewArray(ctx);
  for (std::size_t i = 0; i < handle->stores.size(); ++i) {
    JS_SetPropertyUint32(
        ctx, names, static_cast<uint32_t>(i), JS_NewString(ctx, handle->stores[i].name.c_str()));
  }
  JS_SetPropertyStr(ctx, obj, "objectStoreNames", names); // steals names
  BindIdbMethod(ctx, obj, "createObjectStore", IdbDatabaseCreateObjectStore, handle_idx);
  BindIdbMethod(ctx, obj, "deleteObjectStore", IdbDatabaseDeleteObjectStore, handle_idx);
  BindIdbMethod(ctx, obj, "transaction", IdbDatabaseTransaction, handle_idx);
  BindIdbMethod(ctx, obj, "close", IdbDatabaseClose, handle_idx);
  if (!JS_IsUndefined(handle->object)) {
    JS_FreeValue(ctx, handle->object);
  }
  handle->object = JS_DupValue(ctx, obj);
  return obj;
}

JSValue MakeIdbTransactionObject(Impl* impl, JSContext* ctx, int handle_idx)
{
  const std::shared_ptr<Impl::IdbHandle>& handle =
      impl->idb_handles[static_cast<std::size_t>(handle_idx)];
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "mode", JS_NewString(ctx, handle->mode.c_str()));
  JS_SetPropertyStr(ctx, obj, "oncomplete", JS_UNDEFINED);
  JS_SetPropertyStr(ctx, obj, "onabort", JS_UNDEFINED);
  JS_SetPropertyStr(ctx, obj, "onerror", JS_UNDEFINED);
  BindIdbMethod(ctx, obj, "objectStore", IdbTransactionObjectStore, handle_idx);
  BindIdbMethod(ctx, obj, "abort", IdbTransactionAbort, handle_idx);
  BindIdbMethod(ctx, obj, "commit", IdbTransactionCommit, handle_idx);
  if (!JS_IsUndefined(handle->object)) {
    JS_FreeValue(ctx, handle->object);
  }
  handle->object = JS_DupValue(ctx, obj);
  return obj;
}

JSValue MakeIdbStoreObject(Impl* impl, JSContext* ctx, int handle_idx)
{
  const std::shared_ptr<Impl::IdbHandle>& handle =
      impl->idb_handles[static_cast<std::size_t>(handle_idx)];
  const Impl::IdbHandle* db = nullptr;
  if (handle->db_handle >= 0 &&
      static_cast<std::size_t>(handle->db_handle) < impl->idb_handles.size()) {
    db = impl->idb_handles[static_cast<std::size_t>(handle->db_handle)].get();
  }
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, handle->store_name.c_str()));
  std::string key_path;
  bool auto_increment = false;
  if (db != nullptr) {
    for (const Impl::IdbStoreInfo& info : db->stores) {
      if (info.name == handle->store_name) {
        key_path = info.key_path;
        auto_increment = info.auto_increment;
        break;
      }
    }
  }
  if (key_path.empty()) {
    JS_SetPropertyStr(ctx, obj, "keyPath", JS_NULL);
  } else {
    JS_SetPropertyStr(ctx, obj, "keyPath", JS_NewString(ctx, key_path.c_str()));
  }
  JS_SetPropertyStr(ctx, obj, "autoIncrement", JS_NewBool(ctx, auto_increment ? 1 : 0));
  BindIdbMethod(ctx, obj, "add", IdbStoreAdd, handle_idx);
  BindIdbMethod(ctx, obj, "put", IdbStorePut, handle_idx);
  BindIdbMethod(ctx, obj, "get", IdbStoreGet, handle_idx);
  BindIdbMethod(ctx, obj, "delete", IdbStoreDelete, handle_idx);
  BindIdbMethod(ctx, obj, "clear", IdbStoreClear, handle_idx);
  BindIdbMethod(ctx, obj, "count", IdbStoreCount, handle_idx);
  BindIdbMethod(ctx, obj, "getAll", IdbStoreGetAll, handle_idx);
  if (!JS_IsUndefined(handle->object)) {
    JS_FreeValue(ctx, handle->object);
  }
  handle->object = JS_DupValue(ctx, obj);
  return obj;
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
    if (auto w_min = num_feature(expr, "min-width")) {
      matched = matched && 800.0 >= *w_min;
    } else if (auto w_max = num_feature(expr, "max-width")) {
      matched = matched && 800.0 <= *w_max;
    } else if (auto h_min = num_feature(expr, "min-height")) {
      matched = matched && 600.0 >= *h_min;
    } else if (auto h_max = num_feature(expr, "max-height")) {
      matched = matched && 600.0 <= *h_max;
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
  JSValue global_for_array = JS_GetGlobalObject(ctx);
  JSValue array_ctor = JS_GetPropertyStr(ctx, global_for_array, "Array");
  JSValue array_proto = JS_GetPropertyStr(ctx, array_ctor, "prototype");
  node_list_proto = JS_NewObjectProto(ctx, array_proto);
  JS_FreeValue(ctx, array_proto);
  JS_FreeValue(ctx, array_ctor);
  JS_FreeValue(ctx, global_for_array);
  JS_SetPropertyStr(ctx, node_list_proto, "item", JS_NewCFunction(ctx, NodeListItem, "item", 1));
  DefineGetter(ctx, node_list_proto, "length", MakeGetter(ctx, "length", NodeListLength));
  html_iframe_element_proto = JS_NewObjectProto(ctx, element_proto);
  svg_element_proto = JS_NewObjectProto(ctx, element_proto);

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
  JS_SetPropertyStr(ctx, doc_wrap, "visibilityState", JS_NewString(ctx, "visible"));
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

  // Legacy jQuery compatibility aliases (common on ad/tracking/bootstraps):
  // these are intentionally minimal and only prevent startup ReferenceErrors.
  // A real selector engine is still out of scope.
  JSValue jquery = JS_NewCFunction(ctx,
                                   [](JSContext* inner_ctx,
                                      JSValueConst /*this_val*/,
                                      int /*argc*/,
                                      JSValueConst* /*argv*/) -> JSValue {
    return JS_NewArray(inner_ctx);
  },
                                   "jQuery",
                                   1);
  JSValue jquery_ready = JS_NewCFunction(ctx,
                                         [](JSContext* inner_ctx,
                                            JSValueConst /*this_val*/,
                                            int argc,
                                            JSValueConst* argv) -> JSValue {
    if (argc > 0 && JS_IsFunction(inner_ctx, argv[0])) {
      JSValue callback = JS_DupValue(inner_ctx, argv[0]);
      JSValue set_timeout = JS_GetPropertyStr(inner_ctx, JS_GetGlobalObject(inner_ctx), "setTimeout");
      JSValue args[2] = {callback, JS_NewInt32(inner_ctx, 0)};
      JSValue result = JS_Call(inner_ctx, set_timeout, JS_UNDEFINED, 2, args);
      JS_FreeValue(inner_ctx, callback);
      JS_FreeValue(inner_ctx, set_timeout);
      JS_FreeValue(inner_ctx, result);
    }
    return JS_UNDEFINED;
  }, "ready", 1);
  JS_SetPropertyStr(ctx, jquery, "ready", jquery_ready); // steals
  JSValue jquery_window = JS_DupValue(ctx, jquery);
  JSValue jquery_global = JS_DupValue(ctx, jquery);
  JSValue dollar_window = JS_DupValue(ctx, jquery);
  JSValue dollar_global = JS_DupValue(ctx, jquery);
  JS_SetPropertyStr(ctx, window, "jQuery", jquery_window); // steals
  JS_SetPropertyStr(ctx, global, "jQuery", jquery_global); // steals
  JS_SetPropertyStr(ctx, window, "$", dollar_window);      // steals
  JS_SetPropertyStr(ctx, global, "$", dollar_global);      // steals
  JS_FreeValue(ctx, jquery);

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
  DefineInterface(ctx, global, "HTMLIFrameElement", html_iframe_element_proto);
  DefineInterface(ctx, global, "SVGElement", svg_element_proto);
  DefineInterface(ctx, global, "NodeList", node_list_proto);
  JS_SetPropertyStr(ctx,
                    global,
                    "MutationObserver",
                    JS_NewCFunction2(ctx,
                                     MutationObserverConstructor,
                                     "MutationObserver",
                                     1,
                                     JS_CFUNC_constructor,
                                     0));
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

  // Legacy bootstrap shims used by Bing and other real-world pages.  These are
  // intentionally minimal and only supply the objects/scripts expect during
  // startup; they are not a full compatibility layer for the full browser.
  JS_SetPropertyStr(ctx, window, "_w", JS_DupValue(ctx, window));
  JS_SetPropertyStr(ctx, window, "_d", JS_DupValue(ctx, doc_wrap));
  JS_SetPropertyStr(ctx, global, "_w", JS_DupValue(ctx, window));
  JS_SetPropertyStr(ctx, global, "_d", JS_DupValue(ctx, doc_wrap));

  auto make_noop_function = [&](const char* name) {
    return JS_NewCFunction(ctx,
                           [](JSContext* /*inner_ctx*/, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) -> JSValue {
                             return JS_UNDEFINED;
                           },
                           name,
                           0);
  };

  JSValue perf_observer_proto = JS_NewObject(ctx);
  static const std::array<JSCFunctionListEntry, 3> kPerformanceObserver = {{
      JS_CFUNC_DEF("observe", 0, nullptr),
      JS_CFUNC_DEF("disconnect", 0, nullptr),
      JS_CFUNC_DEF("takeRecords", 0, nullptr),
  }};
  JS_SetPropertyFunctionList(
      ctx, perf_observer_proto, kPerformanceObserver.data(), static_cast<int>(kPerformanceObserver.size()));
  JSValue performance_observer_ctor =
      JS_NewCFunction2(ctx,
                       [](JSContext* inner_ctx,
                          JSValueConst /*this_val*/,
                          int /*argc*/,
                          JSValueConst* /*argv*/) -> JSValue {
                         JSValue observer = JS_NewObject(inner_ctx);
                         return observer;
                       },
                       "PerformanceObserver",
                       1,
                       JS_CFUNC_constructor,
                       0);
  JS_SetPropertyStr(ctx, performance_observer_ctor, "prototype", perf_observer_proto); // steals
  JS_SetPropertyStr(ctx, window, "PerformanceObserver", JS_DupValue(ctx, performance_observer_ctor));
  JS_SetPropertyStr(ctx, global, "PerformanceObserver", performance_observer_ctor);

  JSValue service_worker = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, service_worker, "controller", JS_NULL);
  JS_SetPropertyStr(ctx, navigator, "serviceWorker", JS_DupValue(ctx, service_worker));
  JS_SetPropertyStr(ctx, window, "serviceWorker", JS_DupValue(ctx, service_worker));
  JS_SetPropertyStr(ctx, global, "serviceWorker", service_worker);

  JSValue visual_viewport = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, visual_viewport, "width", JS_NewFloat64(ctx, 800.0));
  JS_SetPropertyStr(ctx, visual_viewport, "height", JS_NewFloat64(ctx, 600.0));
  JS_SetPropertyStr(ctx, visual_viewport, "scale", JS_NewFloat64(ctx, 1.0));
  JS_SetPropertyStr(ctx, window, "visualViewport", JS_DupValue(ctx, visual_viewport));
  JS_SetPropertyStr(ctx, global, "visualViewport", visual_viewport);

  JSValue feedback = JS_NewObject(ctx);
  JSValue feedback_bootstrap = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, feedback, "Bootstrap", feedback_bootstrap); // steals
  JS_SetPropertyStr(ctx, window, "Feedback", JS_DupValue(ctx, feedback));
  JS_SetPropertyStr(ctx, global, "Feedback", feedback);

  JSValue bm = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, bm, "trigger", make_noop_function("trigger"));
  JS_SetPropertyStr(ctx, window, "BM", JS_DupValue(ctx, bm));
  JS_SetPropertyStr(ctx, global, "BM", bm);

  JSValue log = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, log, "Log", make_noop_function("Log"));
  JS_SetPropertyStr(ctx, window, "Log", JS_DupValue(ctx, log));
  JS_SetPropertyStr(ctx, global, "Log", log);

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
    JS_SetPropertyStr(ctx, global, "location", JS_DupValue(ctx, location)); // steals dup
    JS_SetPropertyStr(ctx, document_proto, "location", JS_DupValue(ctx, location));
    JS_FreeValue(ctx, location);
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
  JSValue blob_ctor = JS_NewCFunction2(ctx, BlobConstructor, "Blob", 2, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(ctx, window, "Blob", JS_DupValue(ctx, blob_ctor));
  JS_SetPropertyStr(ctx, global, "Blob", blob_ctor);
  JSValue url_object = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, url_object, "createObjectURL", JS_NewCFunction(ctx, UrlCreateObjectUrl, "createObjectURL", 1));
  JS_SetPropertyStr(ctx, url_object, "revokeObjectURL", JS_NewCFunction(ctx, UrlRevokeObjectUrl, "revokeObjectURL", 1));
  JS_SetPropertyStr(ctx, window, "URL", JS_DupValue(ctx, url_object));
  JS_SetPropertyStr(ctx, global, "URL", url_object);
  if (apis.idb_current_version) {
    JSValue idb = JS_NewObject(ctx);
    JSValue open_fn = JS_NewCFunction(ctx, IdbOpen, "open", 1);
    JS_SetPropertyStr(ctx, idb, "open", open_fn); // steals open_fn
    JSValue delete_fn = JS_NewCFunction(ctx, IdbDeleteDatabase, "deleteDatabase", 1);
    JS_SetPropertyStr(ctx, idb, "deleteDatabase", delete_fn);           // steals delete_fn
    JS_SetPropertyStr(ctx, window, "indexedDB", JS_DupValue(ctx, idb)); // steals
    JS_SetPropertyStr(ctx, global, "indexedDB", idb);                   // steals
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

  for (auto& node_entry : event_handlers) {
    for (auto& handler_entry : node_entry.second) {
      JS_FreeValue(ctx, handler_entry.second);
    }
  }
  event_handlers.clear();

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
                           "_w",
                           "_d",
                           "$",
                           "jQuery",
                           "PerformanceObserver",
                           "serviceWorker",
                           "visualViewport",
                           "Feedback",
                           "BM",
                           "Log",
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
                            "HTMLIFrameElement",
                            "SVGElement",
                            "MutationObserver",
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
                           "fetch",
                           "indexedDB"}) {
    JSAtom atom = JS_NewAtom(ctx, name);
    JS_DeleteProperty(ctx, global, atom, JS_PROP_THROW);
    JS_FreeAtom(ctx, atom);
  }
  JS_FreeValue(ctx, global);
  // (window === global here, so "document" was already deleted above; the
  // separate window object no longer exists.)

  // Free cached DOMImplementation singleton and prototypes/window.  This value
  // is not a property on the global object; it is held by the binder for the
  // document wrapper, so its ref must be released explicitly before the runtime
  // teardown can pass the GC assertion check.
  if (!JS_IsUndefined(document_implementation)) {
    JS_FreeValue(ctx, document_implementation);
    document_implementation = JS_UNDEFINED;
  }
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
  JS_FreeValue(ctx, node_list_proto);
  JS_FreeValue(ctx, html_iframe_element_proto);
  JS_FreeValue(ctx, svg_element_proto);
  for (MutationObserver& observer : mutation_observers) {
    JS_FreeValue(ctx, observer.callback);
    JS_FreeValue(ctx, observer.self);
  }
  mutation_observers.clear();
  JS_FreeValue(ctx, window);

  // Free the IndexedDB object-model references (databases/transactions/stores).
  for (auto& handle : idb_handles) {
    if (!JS_IsUndefined(handle->object)) {
      JS_FreeValue(ctx, handle->object);
      handle->object = JS_UNDEFINED;
    }
  }
  idb_handles.clear();
  idb_requests.clear();

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
  case dom::NodeType::kElement: {
    const auto* element = static_cast<const dom::Element*>(node);
    if (element->namespace_uri() == "http://www.w3.org/2000/svg") {
      return svg_element_proto;
    }
    if (element->tag_name() == "iframe") {
      return html_iframe_element_proto;
    }
    return element_proto;
  }
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
  JS_SetPrototype(ctx, arr, node_list_proto);
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    JSValue w = WrapNode(nodes[i]);
    JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), w); // steals w
  }
  return arr;
}

JSValue Impl::MakeElementArray(const std::vector<dom::Element*>& elements)
{
  JSValue arr = JS_NewArray(ctx);
  JS_SetPrototype(ctx, arr, node_list_proto);
  for (std::size_t i = 0; i < elements.size(); ++i) {
    JSValue w = WrapNode(elements[i]);
    JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), w); // steals w
  }
  return arr;
}

void Impl::RecordChildListMutation(dom::Node* target,
                                   std::vector<dom::Node*> added,
                                   std::vector<dom::Node*> removed)
{
  for (MutationObserver& observer : mutation_observers) {
    if (observer.target == nullptr || !observer.child_list) {
      continue;
    }
    bool matches = target == observer.target;
    if (!matches && observer.subtree) {
      for (dom::Node* ancestor = target->parent(); ancestor != nullptr; ancestor = ancestor->parent()) {
        if (ancestor == observer.target) {
          matches = true;
          break;
        }
      }
    }
    if (matches) {
      observer.records.push_back(MutationRecord{"childList", target, std::move(added), std::move(removed), {}});
    }
  }
}

void Impl::RecordAttributeMutation(dom::Node* target, std::string attribute_name)
{
  for (MutationObserver& observer : mutation_observers) {
    if (observer.target == nullptr || !observer.attributes) {
      continue;
    }
    bool matches = target == observer.target;
    if (!matches && observer.subtree) {
      for (dom::Node* ancestor = target->parent(); ancestor != nullptr; ancestor = ancestor->parent()) {
        if (ancestor == observer.target) {
          matches = true;
          break;
        }
      }
    }
    if (matches) {
      observer.records.push_back(MutationRecord{"attributes", target, {}, {}, std::move(attribute_name)});
    }
  }
}

void Impl::RecordCharacterDataMutation(dom::Node* target)
{
  for (MutationObserver& observer : mutation_observers) {
    if (observer.target == nullptr || !observer.character_data) {
      continue;
    }
    bool matches = target == observer.target;
    if (!matches && observer.subtree) {
      for (dom::Node* ancestor = target->parent(); ancestor != nullptr; ancestor = ancestor->parent()) {
        if (ancestor == observer.target) {
          matches = true;
          break;
        }
      }
    }
    if (matches) {
      observer.records.push_back(MutationRecord{"characterData", target, {}, {}, {}});
    }
  }
}

void Impl::DeliverMutationObservers()
{
  if (delivering_mutation_observers) {
    return;
  }
  delivering_mutation_observers = true;
  for (MutationObserver& observer : mutation_observers) {
    if (observer.records.empty()) {
      continue;
    }
    JSValue records = JS_NewArray(ctx);
    for (std::size_t i = 0; i < observer.records.size(); ++i) {
      const MutationRecord& record = observer.records[i];
      JSValue item = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, item, "type", JS_NewString(ctx, record.type.c_str()));
      JS_SetPropertyStr(ctx, item, "target", WrapNode(record.target));
      JS_SetPropertyStr(ctx,
                        item,
                        "attributeName",
                        record.attribute_name.empty() ? JS_NULL
                                                      : JS_NewString(ctx, record.attribute_name.c_str()));
      JS_SetPropertyStr(ctx, item, "addedNodes", MakeNodeArray(record.added_nodes));
      JS_SetPropertyStr(ctx, item, "removedNodes", MakeNodeArray(record.removed_nodes));
      JS_SetPropertyUint32(ctx, records, static_cast<uint32_t>(i), item);
    }
    observer.records.clear();
    JSValue args[2] = {records, JS_DupValue(ctx, observer.self)};
    JSValue result = JS_Call(ctx, observer.callback, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, records);
    if (JS_IsException(result)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
    } else {
      JS_FreeValue(ctx, result);
    }
  }
  delivering_mutation_observers = false;
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

// Returns the legacy UI Events keyCode for a DOM key string (0 when unknown).
int KeyCodeFor(std::string_view key)
{
  if (key.size() == 1) {
    const unsigned char c = static_cast<unsigned char>(key[0]);
    return c == ' ' ? 32 : static_cast<int>(c);
  }
  static const std::unordered_map<std::string_view, int> kMap = {
      {"Enter", 13},   {"Backspace", 8},   {"Tab", 9},        {"Escape", 27},   {"Delete", 46},
      {"Home", 36},    {"End", 35},        {"PageUp", 33},    {"PageDown", 34}, {"ArrowLeft", 37},
      {"ArrowUp", 38}, {"ArrowRight", 39}, {"ArrowDown", 40}, {"Shift", 16},    {"Control", 17},
      {"Alt", 18},     {"Meta", 91},       {"F1", 112},       {"F2", 113},      {"F3", 114},
      {"F4", 115},     {"F5", 116},        {"F6", 117},       {"F7", 118},      {"F8", 119},
      {"F9", 120},     {"F10", 121},       {"F11", 122},      {"F12", 123},
  };
  const auto it = kMap.find(key);
  return it != kMap.end() ? it->second : 0;
}

JSValue Impl::MakeKeyboardEvent(
    std::string type, bool bubbles, bool cancelable, std::string key, std::string code)
{
  auto* w = new EventWrapper{this, std::move(type), bubbles, cancelable};
  w->key = std::move(key);
  w->code = std::move(code);
  w->key_code = KeyCodeFor(w->key);
  JSValue obj = JS_NewObjectClass(ctx, g_event_class_id);
  JS_SetOpaque(obj, w);
  JS_SetPrototype(ctx, obj, event_proto);
  return obj;
}

JSValue Impl::MakeMouseEvent(
    std::string type, bool bubbles, bool cancelable, double client_x, double client_y, int button)
{
  auto* w = new EventWrapper{this, std::move(type), bubbles, cancelable};
  w->client_x = client_x;
  w->client_y = client_y;
  w->button = button;
  JSValue obj = JS_NewObjectClass(ctx, g_event_class_id);
  JS_SetOpaque(obj, w);
  JS_SetPrototype(ctx, obj, event_proto);
  return obj;
}

JSValue Impl::MakeWheelEvent(std::string type, bool bubbles, bool cancelable, double delta_y)
{
  auto* w = new EventWrapper{this, std::move(type), bubbles, cancelable};
  w->delta_y = delta_y;
  JSValue obj = JS_NewObjectClass(ctx, g_event_class_id);
  JS_SetOpaque(obj, w);
  JS_SetPrototype(ctx, obj, event_proto);
  return obj;
}

// Dispatches a cancelable keyboard event (keydown/keyup) to |node| with the
// UI Events key/code strings.  Returns whether the event was NOT canceled.
bool Impl::DispatchKeyboardToNode(dom::Node* node,
                                  std::string_view type,
                                  std::string_view key,
                                  std::string_view code)
{
  if (node == nullptr) {
    return true;
  }
  JSValue event = MakeKeyboardEvent(std::string(type),
                                    /*bubbles=*/true,
                                    /*cancelable=*/true,
                                    std::string(key),
                                    std::string(code));
  const bool not_canceled = DispatchPropagated(node, event);
  JS_FreeValue(ctx, event);
  engine.RunPendingJobs();
  return not_canceled;
}

// Dispatches a cancelable pointer event (mousedown/mouseup/click) to |node|
// with client coordinates and the mouse button.  Returns whether NOT canceled.
bool Impl::DispatchMouseToNode(
    dom::Node* node, std::string_view type, double client_x, double client_y, int button)
{
  if (node == nullptr) {
    return true;
  }
  JSValue event = MakeMouseEvent(
      std::string(type), /*bubbles=*/true, /*cancelable=*/true, client_x, client_y, button);
  const bool not_canceled = DispatchPropagated(node, event);
  JS_FreeValue(ctx, event);
  engine.RunPendingJobs();
  return not_canceled;
}

// Dispatches a cancelable wheel event to |node| with the vertical scroll delta
// (px).  Returns whether NOT canceled.
bool Impl::DispatchWheelToNode(dom::Node* node, std::string_view type, double delta_y)
{
  if (node == nullptr) {
    return true;
  }
  JSValue event = MakeWheelEvent(std::string(type), /*bubbles=*/true, /*cancelable=*/true, delta_y);
  const bool not_canceled = DispatchPropagated(node, event);
  JS_FreeValue(ctx, event);
  engine.RunPendingJobs();
  return not_canceled;
}

// Dispatches a focus event ("focus"/"blur") to |node|.  Non-bubbling and
// non-cancelable, matching the UI Events spec.
void Impl::DispatchFocusToNode(dom::Node* node, std::string_view type)
{
  if (node == nullptr) {
    return;
  }
  JSValue event = MakeEvent(std::string(type), /*bubbles=*/false, /*cancelable=*/false);
  (void)DispatchPropagated(node, event);
  JS_FreeValue(ctx, event);
  engine.RunPendingJobs();
}

// Dispatches an "input" event to |node| after a text control's value changed.
// Bubbles, not cancelable.
void Impl::DispatchInputToNode(dom::Node* node)
{
  if (node == nullptr) {
    return;
  }
  JSValue event = MakeEvent("input", /*bubbles=*/true, /*cancelable=*/false);
  (void)DispatchPropagated(node, event);
  JS_FreeValue(ctx, event);
  engine.RunPendingJobs();
}

// Fires the element's global event handler (onclick/oninput/...) for the
// event's type if one is set — either a JS-assigned IDL handler stored on the
// wrapper, or a content attribute (on*="code") compiled to a function on first
// fire and cached on the wrapper.
void Impl::FireEventHandler(dom::Node* node, EventWrapper* w, JSValue event, int phase)
{
  if (node->node_type() != dom::NodeType::kElement) {
    return;
  }
  const char* name = OnHandlerForType(w->type);
  if (name == nullptr) {
    return;
  }
  auto* element = static_cast<dom::Element*>(node);
  JSValue wrapper = WrapNode(element);
  JSValue handler = JS_UNDEFINED;
  const auto attr = element->GetAttribute(name);
  if (attr.has_value()) {
    // Content attribute (on*="code") wins over any IDL handler: compile it to
    // a function on every fire (content attributes change rarely).
    std::string src = "(function(event){\n";
    src.append(attr->begin(), attr->end());
    src.append("\n})");
    JSValue compiled =
        JS_Eval(ctx, src.data(), src.size(), "<inline-handler>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(compiled)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      JS_FreeValue(ctx, wrapper);
      return;
    }
    handler = compiled;
  } else {
    // IDL handler assigned from JS (element.onclick = fn).
    const auto node_it = event_handlers.find(node);
    if (node_it == event_handlers.end()) {
      JS_FreeValue(ctx, wrapper);
      return;
    }
    const auto handler_it = node_it->second.find(name);
    if (handler_it == node_it->second.end() || !JS_IsFunction(ctx, handler_it->second)) {
      JS_FreeValue(ctx, wrapper);
      return;
    }
    handler = JS_DupValue(ctx, handler_it->second);
  }
  // Expose currentTarget/eventPhase to the handler, then call it with
  // this = the element and the event as the single argument.
  if (!JS_IsUndefined(w->current_target)) {
    JS_FreeValue(ctx, w->current_target);
  }
  w->current_target = JS_DupValue(ctx, wrapper);
  w->event_phase = phase;
  JSValue event_arg = JS_DupValue(ctx, event);
  JSValue result = JS_Call(ctx, handler, wrapper, 1, &event_arg);
  JS_FreeValue(ctx, event_arg);
  if (JS_IsException(result)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
  } else {
    JS_FreeValue(ctx, result);
  }
  JS_FreeValue(ctx, handler);
  JS_FreeValue(ctx, wrapper);
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
    // Global event handler attributes (element.onclick = fn) fire like bubble
    // listeners: once per element on the propagation path, never in capture.
    if (!capture_phase) {
      FireEventHandler(node, w, event, phase);
      if (w->immediate_stopped || w->propagation_stopped) {
        return;
      }
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

// A cancelable user-interaction event (click/submit/keydown...): bubbles with
// the canceled flag wired to preventDefault, so the caller learns whether to
// run the default action.
bool Impl::DispatchCancelableToNode(dom::Node* node, std::string_view type)
{
  if (node == nullptr) {
    return true;
  }
  JSValue event = MakeEvent(std::string(type), /*bubbles=*/true, /*cancelable=*/true);
  const bool not_canceled = DispatchPropagated(node, event);
  JS_FreeValue(ctx, event);
  engine.RunPendingJobs();
  return not_canceled;
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
  const base::Result<ScriptValue> result = impl_->engine.Evaluate(source, filename);
  impl_->DeliverMutationObservers();
  return result;
}

void DomBinder::SetCurrentScript(dom::Element* element)
{
  impl_->SetCurrentScript(element);
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

bool DomBinder::DispatchCancelableEvent(dom::Element& element, std::string_view type)
{
  return impl_->DispatchCancelableToNode(&element, type);
}

bool DomBinder::DispatchKeyboardEvent(dom::Element& element,
                                      std::string_view type,
                                      std::string_view key,
                                      std::string_view code)
{
  return impl_->DispatchKeyboardToNode(&element, type, key, code);
}

bool DomBinder::DispatchMouseEvent(
    dom::Element& element, std::string_view type, double client_x, double client_y, int button)
{
  return impl_->DispatchMouseToNode(&element, type, client_x, client_y, button);
}

bool DomBinder::DispatchWheelEvent(dom::Element& element, std::string_view type, double delta_y)
{
  return impl_->DispatchWheelToNode(&element, type, delta_y);
}

void DomBinder::DispatchFocusEvent(dom::Element& element, std::string_view type)
{
  impl_->DispatchFocusToNode(&element, type);
}

void DomBinder::DispatchInputEvent(dom::Element& element)
{
  impl_->DispatchInputToNode(&element);
}

bool DomBinder::TakeDomDirty()
{
  return impl_->TakeDomDirty();
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
