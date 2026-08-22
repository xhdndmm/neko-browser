// Internal header shared by the dom_binding implementation files.
//
// dom_binding.cpp used to be a single 8600-line translation unit; it is now
// split by API family (node/element/document/style/event/storage/global/ui
// bindings) around this header.  Everything here is private to the
// neko::javascript module: the header lives in src/ and is never installed.
//
// Threading: thread-confined, like ScriptEngine.  Use a binder from one
// thread at a time.

#pragma once

#include "neko/javascript/dom_binding.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <quickjs.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neko::javascript {

// ---------------------------------------------------------------------------
// Process-wide QuickJS plumbing.
//
// Every DomBinder creates its own runtime, but the wrapper class ids and the
// ctx -> Impl registry are shared across runtimes.  Each class id is assigned
// once (JS_NewClassID is idempotent for a non-zero output) and the class is
// registered once per runtime (see Ensure*ClassRegistered below).
// ---------------------------------------------------------------------------

extern JSClassID g_node_class_id;
extern std::mutex g_class_mutex;
extern std::unordered_set<JSRuntime*> g_class_registered;

// Opaque data attached to every node wrapper (also reused for style objects,
// whose opaque carries the owning element).
struct NodeWrapper
{
  Impl* impl = nullptr;
  dom::Node* node = nullptr; // null when detached
};

// Registers the "Node" wrapper class on |rt| when not yet registered.
void EnsureNodeClassRegistered(JSRuntime* rt);

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

extern JSClassID g_event_class_id;
extern std::mutex g_event_class_mutex;
extern std::unordered_set<JSRuntime*> g_event_class_registered;

// Dataset registry lives in element_binding.cpp; the destructor (impl.cpp)
// erases runtimes from it.
extern std::mutex g_dataset_class_mutex;
extern std::unordered_set<JSRuntime*> g_dataset_class_registered;

// Registers the "Event" wrapper class on |rt| when not yet registered.
void EnsureEventClassRegistered(JSRuntime* rt);

// Registers the "DOMStringMap" (element.dataset) exotic class on |rt| when
// not yet registered.
void EnsureDatasetClassRegistered(JSRuntime* rt);

extern std::mutex g_ctx_mutex;
extern std::unordered_map<JSContext*, Impl*> g_ctx_to_impl;

// ---------------------------------------------------------------------------
// Shared helpers (defined in impl.cpp unless noted otherwise).
// ---------------------------------------------------------------------------

// Resolves the owning Impl for a C callback.  Prefers the opaque wrapper on
// |this_val| (node methods), then falls back to the ctx registry (global
// functions like setTimeout, whose this_val is the global object).
Impl* ImplFor(JSContext* ctx, JSValueConst this_val);

dom::Node* UnwrapNode(JSValueConst this_val);
dom::Element* AsElement(dom::Node* node);
int32_t NodeTypeNumber(dom::NodeType type);

EventWrapper* UnwrapEvent(JSValueConst value);
// Reads the 'type' of an event-like value: the opaque wrapper's type for a
// real Event, else the JS 'type' property (for plain {type: ...} objects,
// which dispatchEvent still accepts as a shortcut).
bool EventTypeOf(JSContext* ctx, JSValueConst value, std::string* out);
// True when |value| is a real Event object.
bool IsEvent(JSValueConst value);

std::string NodeNameOf(const dom::Node& node);

// Converts an argument to a string (JS ToString semantics).  On conversion
// failure a pending exception is cleared and false is returned.
std::string ArgString(JSContext* ctx, JSValueConst value, bool* ok);

std::string ToLower(std::string s);
std::string ToUpper(std::string s);

bool IsAncestorOf(const dom::Node* ancestor, const dom::Node* node);

// Deep or shallow clone of a node (cloneNode).  Documents cannot be cloned.
std::unique_ptr<dom::Node> CloneNodeImpl(const dom::Node& source, bool deep);

// Throws a DOMException with the given WebIDL exception name (DOM spec §4.4,
// WebIDL §3.5).  DOM tree operations report NotFoundError, HierarchyRequestError,
// InvalidNodeTypeError, etc. rather than a plain TypeError.
JSValue ThrowDomException(JSContext* ctx, std::string_view name, std::string_view message);

// ---------------------------------------------------------------------------
// Inline style attribute helpers (backing CSSStyleDeclaration); defined in
// style_binding.cpp.
// ---------------------------------------------------------------------------

struct InlineDecl
{
  std::string property; // lowercased
  std::string value;    // trimmed, without !important
  bool important = false;
};

std::vector<InlineDecl> ParseInlineStyle(std::string_view attr);
std::string SerializeInlineStyle(const std::vector<InlineDecl>& decls);
std::string CurrentStyleAttr(const dom::Element& element);
void SetStyleAttr(dom::Element& element, const std::string& value);

// The CSS property names reachable through the direct style accessors.  The
// magic value of a getter/setter indexes this table.
inline constexpr std::array<std::string_view, 18> kStyleProps = {
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

inline void
DefineAccessor(JSContext* ctx, JSValue proto, const char* name, JSValue getter, JSValue setter)
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

inline void DefineGetter(JSContext* ctx, JSValue proto, const char* name, JSValue getter)
{
  DefineAccessor(ctx, proto, name, getter, JS_UNDEFINED);
}

// DOM eventPhase values (DOM Standard §2.9).
inline constexpr int kEventNone = 0;
inline constexpr int kEventCapturing = 1;
inline constexpr int kEventAtTarget = 2;
inline constexpr int kEventBubbling = 3;

// ---------------------------------------------------------------------------
// Prototype/interface construction (implemented across the binding files).
// ---------------------------------------------------------------------------

void DefineNodePrototype(JSContext* ctx, Impl& impl);      // node_binding.cpp
void DefineElementPrototype(JSContext* ctx, Impl& impl);   // element_binding.cpp
void DefineDocumentPrototype(JSContext* ctx, Impl& impl);  // document_binding.cpp
void DefineStylePrototype(JSContext* ctx, Impl& impl);     // style_binding.cpp
void DefineEventPrototype(JSContext* ctx, Impl& impl);     // event_binding.cpp
void DefineClassListPrototype(JSContext* ctx, Impl& impl); // element_binding.cpp
// Element-level global event handler attributes (onclick/oninput/...),
// defined in event_binding.cpp and called from DefineElementPrototype.
void DefineElementEventHandlers(JSContext* ctx, Impl& impl);

// Web IDL-style interface constructor support (impl.cpp): exposes each DOM
// interface name as a global constructor whose .prototype is the live
// prototype ("Illegal constructor" when constructed directly).
JSValue IllegalConstructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
void DefineInterface(
    JSContext* ctx, JSValue global, const char* name, JSValue proto, bool set_constructor = true);

// ---------------------------------------------------------------------------
// Native callbacks referenced directly by Impl's constructor / methods.
// Definitions live in the binding file that owns the API family.
// ---------------------------------------------------------------------------

// node_binding.cpp
JSValue CharacterDataGetData(JSContext* ctx, JSValueConst this_val);
JSValue CharacterDataSetData(JSContext* ctx, JSValueConst this_val, JSValueConst value);
JSValue NodeListItem(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue NodeListLength(JSContext* ctx, JSValueConst this_val);
// Referenced from other binding files (innerText getter, window listener
// forwarding, insertAdjacentHTML reference-node resolution).
JSValue NodeGetTextContent(JSContext* ctx, JSValueConst this_val);
JSValue NodeAddEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue
NodeRemoveEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue NodeDispatchEvent(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
dom::Node* SiblingOf(dom::Node* node, int offset);

// event_binding.cpp — maps an event type to its global handler property name
// ("click" -> "onclick"); used by Impl::FireEventHandler (impl.cpp).
const char* OnHandlerForType(std::string_view type);

// document_binding.cpp — first matching element by tag (used by
// DocGetHead/DocGetTitle and the element file's head/title helpers).
dom::Element* FindElementByTag(const dom::Node& root, std::string_view tag);

// ui_binding.cpp — referenced by DefineElementPrototype (element_binding.cpp).
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
JSValue
ElementGetBoundingClientRect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue ElementGetOffsetWidth(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue ElementGetOffsetHeight(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue ElementGetOffsetTop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue ElementGetOffsetLeft(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue ElementGetOffsetParent(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue ElementGetClientWidth(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue ElementGetClientHeight(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue ElementGetClientTop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue ElementGetClientLeft(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue ElementGetValue(JSContext* ctx, JSValueConst this_val);
JSValue ElementSetValue(JSContext* ctx, JSValueConst this_val, JSValueConst value);
JSValue ElementGetChecked(JSContext* ctx, JSValueConst this_val);
JSValue ElementSetChecked(JSContext* ctx, JSValueConst this_val, JSValueConst value);
JSValue ElementGetType(JSContext* ctx, JSValueConst this_val);
JSValue ElementSetType(JSContext* ctx, JSValueConst this_val, JSValueConst value);
JSValue ElementGetPlaceholder(JSContext* ctx, JSValueConst this_val);
JSValue ElementSetPlaceholder(JSContext* ctx, JSValueConst this_val, JSValueConst value);
JSValue ElementGetDisabled(JSContext* ctx, JSValueConst this_val);
JSValue ElementSetDisabled(JSContext* ctx, JSValueConst this_val, JSValueConst value);
JSValue ElementGetName(JSContext* ctx, JSValueConst this_val);
JSValue ElementSetName(JSContext* ctx, JSValueConst this_val, JSValueConst value);
JSValue ElementGetHref(JSContext* ctx, JSValueConst this_val);
JSValue ElementSetHref(JSContext* ctx, JSValueConst this_val, JSValueConst value);
JSValue ElementGetAnchorUrlPart(JSContext* ctx, JSValueConst this_val, int magic);
JSValue ElementGetTarget(JSContext* ctx, JSValueConst this_val);
JSValue ElementSetTarget(JSContext* ctx, JSValueConst this_val, JSValueConst value);
JSValue ElementGetRel(JSContext* ctx, JSValueConst this_val);
JSValue ElementSetRel(JSContext* ctx, JSValueConst this_val, JSValueConst value);
JSValue ElementGetSrc(JSContext* ctx, JSValueConst this_val);
JSValue ElementSetSrc(JSContext* ctx, JSValueConst this_val, JSValueConst value);
JSValue ElementGetAlt(JSContext* ctx, JSValueConst this_val);
JSValue ElementSetAlt(JSContext* ctx, JSValueConst this_val, JSValueConst value);
JSValue ElementGetWidth(JSContext* ctx, JSValueConst this_val);
JSValue ElementSetWidth(JSContext* ctx, JSValueConst this_val, JSValueConst value);
JSValue ElementGetHeight(JSContext* ctx, JSValueConst this_val);
JSValue ElementSetHeight(JSContext* ctx, JSValueConst this_val, JSValueConst value);
JSValue ElementGetNaturalWidth(JSContext* ctx, JSValueConst this_val);
JSValue ElementGetNaturalHeight(JSContext* ctx, JSValueConst this_val);
JSValue ElementGetComplete(JSContext* ctx, JSValueConst this_val);
JSValue ElementGetCurrentSrc(JSContext* ctx, JSValueConst this_val);
JSValue ElementPlayVideo(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue ElementPauseVideo(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue ElementGetVideoDuration(JSContext* ctx, JSValueConst this_val);
JSValue ElementGetVideoCurrentTime(JSContext* ctx, JSValueConst this_val);
JSValue ElementSetVideoCurrentTime(JSContext* ctx, JSValueConst this_val, JSValueConst value);
JSValue ElementGetVideoPaused(JSContext* ctx, JSValueConst this_val);

// event_binding.cpp
JSValue
MutationObserverConstructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue EventConstructor(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue CustomEventConstructor(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

// global_binding.cpp
const char* NavigatorPlatform();
JSValue TimerCreate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);
JSValue TimerClear(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);
JSValue WindowAddEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue
WindowRemoveEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue WindowDispatchEvent(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue
WindowRequestAnimationFrame(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue
WindowCancelAnimationFrame(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue WindowScrollTo(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue WindowScrollBy(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue WindowGetComputedStyle(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue WindowMatchMedia(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue HistoryBack(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue HistoryForward(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue HistoryGo(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue HistoryPushState(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue HistoryReplaceState(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue HistoryGetLength(JSContext* ctx, JSValueConst this_val);
JSValue PerformanceNow(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue LocationHrefGetter(JSContext* ctx, JSValueConst this_val);
JSValue LocationHrefSetter(JSContext* ctx, JSValueConst this_val, JSValueConst value);
JSValue LocationPropGetter(JSContext* ctx, JSValueConst this_val, int magic);
JSValue LocationAssign(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue LocationReplace(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue LocationReload(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue LocationToString(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

// storage_binding.cpp
JSValue LocalStorageGetItem(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue LocalStorageSetItem(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue LocalStorageRemoveItem(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue LocalStorageClear(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue LocalStorageKey(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue LocalStorageLength(JSContext* ctx, JSValueConst this_val);
JSValue JsFetch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue BlobConstructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue UrlCreateObjectUrl(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue UrlRevokeObjectUrl(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue IdbOpen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue IdbDeleteDatabase(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

// impl.cpp — new Image([width[, height]]) creates a detached <img> element.
JSValue ImageConstructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

// ---------------------------------------------------------------------------
// XMLHttpRequest (xhr_binding.cpp).
//
// State lives in the wrapper attached to each JS object; the wrapper owns
// the handler/listener JSValues and frees them in its finalizer.
// ---------------------------------------------------------------------------

struct XhrWrapper
{
  Impl* impl = nullptr;
  std::string method;   // uppercased by open()
  std::string url;      // absolute after open() resolution
  bool async_requested = true;
  std::vector<std::pair<std::string, std::string>> request_headers;
  int ready_state = 0; // UNSENT .. DONE (DOM Standard §5.1)
  int status = 0;
  std::string status_text;
  std::string response_text;
  std::string response_url;
  std::vector<std::pair<std::string, std::string>> response_headers;
  JSValue on_ready_state_change = JS_UNDEFINED;
  JSValue on_load = JS_UNDEFINED;
  JSValue on_error = JS_UNDEFINED;
  JSValue on_abort = JS_UNDEFINED;
  // addEventListener("load"|"error"|"readystatechange"|"abort", fn) entries.
  std::vector<std::pair<std::string, JSValue>> listeners;
};

void EnsureXhrClassRegistered(JSRuntime* rt);
XhrWrapper* UnwrapXhr(JSValueConst value);

void InstallXhrGlobal(JSContext* ctx, Impl& impl);

// ---------------------------------------------------------------------------
// Impl — the binder's per-document state (was file-local to dom_binding.cpp).
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
  JSValue xhr_proto = JS_UNDEFINED;
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

} // namespace neko::javascript
