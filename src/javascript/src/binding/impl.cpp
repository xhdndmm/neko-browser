// neko::javascript DomBinder core — shared helpers and Impl state.
//
// Part of the dom_binding split (see binding/binding_internal.h): the
// process-wide QuickJS plumbing (wrapper class ids, ctx -> Impl registry),
// the string/node helper functions used across the binding files, the Web
// IDL interface-constructor support, and the Impl constructor/destructor
// plus its methods (wrapping, timers, event dispatch).

#include "neko/base/status.h"
#include "neko/base/version.h"
#include "neko/dom/element.h"
#include "neko/dom/node.h"
#include "neko/javascript/script_engine_internal.h"

#include "binding_internal.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <quickjs.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neko::javascript {

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

JSClassID g_attr_class_id = 0;
std::mutex g_attr_class_mutex;
std::unordered_set<JSRuntime*> g_attr_class_registered;

void NodeFinalizer(JSRuntime* /*rt*/, JSValue obj)
{
  auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(obj, g_node_class_id));
  delete w;
}

void EnsureNodeClassRegistered(JSRuntime* rt)
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

void AttrFinalizer(JSRuntime* /*rt*/, JSValue obj)
{
  auto* wrapper = static_cast<AttrWrapper*>(JS_GetOpaque(obj, g_attr_class_id));
  delete wrapper;
}

void EnsureAttrClassRegistered(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_attr_class_mutex);
  JS_NewClassID(rt, &g_attr_class_id);
  if (g_attr_class_registered.find(rt) == g_attr_class_registered.end()) {
    JSClassDef def;
    std::memset(&def, 0, sizeof(def));
    def.class_name = "Attr";
    def.finalizer = &AttrFinalizer;
    JS_NewClass(rt, g_attr_class_id, &def);
    g_attr_class_registered.insert(rt);
  }
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

JSValue NavigatorGetLanguages(JSContext* ctx, JSValueConst this_val)
{
  return JS_GetPropertyStr(ctx, this_val, "__nekoNavigatorLanguages");
}

JSValue DateTimeFormatResolvedOptions(JSContext* ctx,
                                      JSValueConst /*this_val*/,
                                      int /*argc*/,
                                      JSValueConst* /*argv*/)
{
  JSValue options = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, options, "locale", JS_NewString(ctx, "en-US"));
  JS_SetPropertyStr(ctx, options, "calendar", JS_NewString(ctx, "gregory"));
  JS_SetPropertyStr(ctx, options, "numberingSystem", JS_NewString(ctx, "latn"));
  JS_SetPropertyStr(ctx, options, "timeZone", JS_NewString(ctx, "UTC"));
  return options;
}

JSValue
DateTimeFormatConstructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* /*argv*/)
{
  if (argc != 0) {
    return ThrowDomException(
        ctx, "NotSupportedError", "Intl.DateTimeFormat locale and options are not implemented");
  }
  JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
  if (JS_IsException(prototype)) {
    return prototype;
  }
  JSValue formatter = JS_NewObjectProto(ctx, prototype);
  JS_FreeValue(ctx, prototype);
  return formatter;
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

std::string ToUpper(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
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
    JSContext* ctx, JSValue global, const char* name, JSValue proto, bool set_constructor)
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
// Impl definition.
// ---------------------------------------------------------------------------

Impl::Impl(dom::Document& doc, const PageApis& page_apis) : document(doc), apis(page_apis)
{
  if (apis.location_href) {
    document_url = apis.location_href();
  }
  navigation_start_epoch_ms =
      static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count());
  ctx = static_cast<JSContext*>(ScriptEngineContext(engine));
  if (ctx == nullptr) {
    return;
  }

  // ES module loading (<script type="module">): remote module sources come
  // through the same PageApis::fetch path as window.fetch (network stack
  // with cookies).  HTTP error statuses reject the load — a 404 HTML body
  // would only surface as a confusing SyntaxError otherwise.  QuickJS caches
  // compiled modules by URL, so repeated imports of the same specifier hit
  // the network once.
  if (apis.fetch) {
    engine.SetModuleFetcher([this](const std::string& url) -> base::Result<std::string> {
      const base::Result<FetchResponse> response = this->apis.fetch(url);
      if (!response.has_value()) {
        return base::Err(response.error());
      }
      if (response.value().status >= 400) {
        return base::Err(base::Error::Javascript("HTTP " + std::to_string(response.value().status) +
                                                 " " + response.value().status_text));
      }
      return base::Ok(std::move(response.value().body));
    });
  }

  JSRuntime* rt = JS_GetRuntime(ctx);
  EnsureNodeClassRegistered(rt);
  EnsureAttrClassRegistered(rt);
  EnsureEventClassRegistered(rt);
  EnsureDatasetClassRegistered(rt);
  EnsureXhrClassRegistered(rt);
  EnsureCanvasClassRegistered(rt);
  {
    std::lock_guard<std::mutex> lock(g_ctx_mutex);
    g_ctx_to_impl[ctx] = this;
  }

  // Prototypes (element/text/... inherit Node).
  node_proto = JS_NewObject(ctx);
  element_proto = JS_NewObjectProto(ctx, node_proto);
  character_data_proto = JS_NewObjectProto(ctx, node_proto);
  document_type_proto = JS_NewObjectProto(ctx, node_proto);
  text_proto = JS_NewObjectProto(ctx, character_data_proto);
  comment_proto = JS_NewObjectProto(ctx, character_data_proto);
  document_proto = JS_NewObjectProto(ctx, node_proto);
  fragment_proto = JS_NewObjectProto(ctx, node_proto);
  style_proto = JS_NewObject(ctx);
  event_proto = JS_NewObject(ctx);
  // CustomEvent.prototype inherits Event.prototype (spec: CustomEvent extends
  // Event); instances are created with this prototype by CustomEventConstructor.
  custom_event_proto = JS_NewObjectProto(ctx, event_proto);
  message_event_proto = JS_NewObjectProto(ctx, event_proto);
  class_list_proto = JS_NewObject(ctx);
  JSValue global_for_array = JS_GetGlobalObject(ctx);
  JSValue array_ctor = JS_GetPropertyStr(ctx, global_for_array, "Array");
  JSValue array_proto = JS_GetPropertyStr(ctx, array_ctor, "prototype");
  node_list_proto = JS_NewObjectProto(ctx, array_proto);
  html_collection_proto = JS_NewObjectProto(ctx, array_proto);
  named_node_map_proto = JS_NewObjectProto(ctx, array_proto);
  attr_proto = JS_NewObject(ctx);
  tree_walker_proto = JS_NewObject(ctx);
  range_proto = JS_NewObject(ctx);
  mutation_observer_proto = JS_NewObject(ctx);
  dom_parser_proto = JS_NewObject(ctx);
  xml_serializer_proto = JS_NewObject(ctx);
  dom_implementation_proto = JS_NewObject(ctx);
  JS_FreeValue(ctx, array_proto);
  JS_FreeValue(ctx, array_ctor);
  JS_FreeValue(ctx, global_for_array);
  JS_SetPropertyStr(ctx, node_list_proto, "item", JS_NewCFunction(ctx, NodeListItem, "item", 1));
  DefineGetter(ctx, node_list_proto, "length", MakeGetter(ctx, "length", NodeListLength));
  JS_SetPropertyStr(
      ctx, html_collection_proto, "item", JS_NewCFunction(ctx, NodeListItem, "item", 1));
  JS_SetPropertyStr(ctx,
                    html_collection_proto,
                    "namedItem",
                    JS_NewCFunction(ctx, HTMLCollectionNamedItem, "namedItem", 1));
  DefineGetter(ctx, html_collection_proto, "length", MakeGetter(ctx, "length", NodeListLength));
  html_base_element_proto = JS_NewObjectProto(ctx, element_proto);
  html_script_element_proto = JS_NewObjectProto(ctx, element_proto);
  html_anchor_element_proto = JS_NewObjectProto(ctx, element_proto);
  html_iframe_element_proto = JS_NewObjectProto(ctx, element_proto);
  html_form_element_proto = JS_NewObjectProto(ctx, element_proto);
  html_button_element_proto = JS_NewObjectProto(ctx, element_proto);
  html_input_element_proto = JS_NewObjectProto(ctx, element_proto);
  html_image_element_proto = JS_NewObjectProto(ctx, element_proto);
  html_media_element_proto = JS_NewObjectProto(ctx, element_proto);
  html_video_element_proto = JS_NewObjectProto(ctx, html_media_element_proto);
  html_canvas_element_proto = JS_NewObjectProto(ctx, element_proto);
  svg_element_proto = JS_NewObjectProto(ctx, element_proto);

  DefineNodePrototype(ctx, *this);
  DefineElementPrototype(ctx, *this);
  DefineCanvasPrototype(ctx, *this);
  DefineDocumentPrototype(ctx, *this);
  DefineStylePrototype(ctx, *this);
  DefineStyleSheetPrototype(ctx, *this);
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
  DefineGetter(ctx, window, "closed", MakeGetter(ctx, "closed", WindowGetClosed));

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
  InstallCustomElementRegistry(ctx, global);
  InstallCrypto(ctx, global);
  InstallEventTargetGlobal(ctx, global, *this);
  InstallAbortGlobals(ctx, global, *this);
  InstallIntersectionObserverGlobal(ctx, global, *this);
  InstallMessageChannelGlobals(ctx, global, *this);

  // Legacy jQuery compatibility aliases (common on ad/tracking/bootstraps):
  // these are intentionally minimal and only prevent startup ReferenceErrors.
  // A real selector engine is still out of scope.
  JSValue jquery = JS_NewCFunction(
      ctx,
      [](JSContext* inner_ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
          -> JSValue { return JS_NewArray(inner_ctx); },
      "jQuery",
      1);
  JSValue jquery_ready = JS_NewCFunction(
      ctx,
      [](JSContext* inner_ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) -> JSValue {
        if (argc > 0 && JS_IsFunction(inner_ctx, argv[0])) {
          JSValue callback = JS_DupValue(inner_ctx, argv[0]);
          JSValue set_timeout =
              JS_GetPropertyStr(inner_ctx, JS_GetGlobalObject(inner_ctx), "setTimeout");
          JSValue args[2] = {callback, JS_NewInt32(inner_ctx, 0)};
          JSValue result = JS_Call(inner_ctx, set_timeout, JS_UNDEFINED, 2, args);
          JS_FreeValue(inner_ctx, callback);
          JS_FreeValue(inner_ctx, set_timeout);
          JS_FreeValue(inner_ctx, result);
        }
        return JS_UNDEFINED;
      },
      "ready",
      1);
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
    JSValue history_proto = JS_NewObject(ctx);
    static const std::array<JSCFunctionListEntry, 5> kHistory = {{
        JS_CFUNC_DEF("back", 0, HistoryBack),
        JS_CFUNC_DEF("forward", 0, HistoryForward),
        JS_CFUNC_DEF("go", 1, HistoryGo),
        JS_CFUNC_DEF("pushState", 3, HistoryPushState),
        JS_CFUNC_DEF("replaceState", 3, HistoryReplaceState),
    }};
    JS_SetPropertyFunctionList(
        ctx, history_proto, kHistory.data(), static_cast<int>(kHistory.size()));
    DefineGetter(ctx, history_proto, "length", MakeGetter(ctx, "length", HistoryGetLength));
    DefineGetter(ctx, history_proto, "state", MakeGetter(ctx, "state", HistoryGetState));
    DefineInterface(ctx, global, "History", history_proto);
    JSValue history = JS_NewObjectProto(ctx, history_proto);
    JS_FreeValue(ctx, history_proto);
    JS_SetPropertyStr(ctx, window, "history", JS_DupValue(ctx, history)); // steals dup
    JS_SetPropertyStr(ctx, global, "history", history);                   // steals
  }

  // window.performance exposes the current document's navigation entry.
  {
    JSValue performance_proto = JS_NewObject(ctx);
    static const std::array<JSCFunctionListEntry, 3> kPerformance = {{
        JS_CFUNC_DEF("now", 0, PerformanceNow),
        JS_CFUNC_DEF("getEntries", 0, PerformanceGetEntries),
        JS_CFUNC_DEF("getEntriesByType", 1, PerformanceGetEntriesByType),
    }};
    JS_SetPropertyFunctionList(
        ctx, performance_proto, kPerformance.data(), static_cast<int>(kPerformance.size()));
    DefineInterface(ctx, global, "Performance", performance_proto);
    JSValue performance = JS_NewObjectProto(ctx, performance_proto);
    JS_FreeValue(ctx, performance_proto);
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
  // CharacterData.data is shared by Text and Comment.
  DefineAccessor(ctx,
                 character_data_proto,
                 "data",
                 MakeGetter(ctx, "data", CharacterDataGetData),
                 MakeSetter(ctx, "data", CharacterDataSetData));
  DefineGetter(ctx, fragment_proto, "children", MakeGetter(ctx, "children", ElementGetChildren));
  DefineInterface(ctx, global, "CharacterData", character_data_proto);
  DefineInterface(ctx, global, "DocumentType", document_type_proto);
  DefineInterface(ctx, global, "Text", text_proto);
  DefineInterface(ctx, global, "Comment", comment_proto);
  DefineInterface(ctx, global, "DocumentFragment", fragment_proto);
  DefineInterface(ctx, global, "CSSStyleDeclaration", style_proto);
  DefineInterface(ctx, global, "Element", element_proto);
  DefineInterface(ctx, global, "HTMLElement", element_proto, /*set_constructor=*/false);
  DefineAccessor(ctx,
                 html_script_element_proto,
                 "src",
                 MakeGetter(ctx, "src", ElementGetSrc),
                 MakeSetter(ctx, "src", ElementSetSrc));
  DefineInterface(ctx, global, "HTMLScriptElement", html_script_element_proto);
  DefineAccessor(ctx,
                 html_base_element_proto,
                 "href",
                 MakeGetter(ctx, "href", ElementGetHref),
                 MakeSetter(ctx, "href", ElementSetHref));
  DefineInterface(ctx, global, "HTMLBaseElement", html_base_element_proto);
  DefineAccessor(ctx,
                 html_anchor_element_proto,
                 "href",
                 MakeGetter(ctx, "href", ElementGetHref),
                 MakeSetter(ctx, "href", ElementSetHref));
  DefineAccessor(ctx,
                 html_anchor_element_proto,
                 "download",
                 MakeGetter(ctx, "download", ElementGetDownload),
                 MakeSetter(ctx, "download", ElementSetDownload));
  DefineAccessor(ctx,
                 html_anchor_element_proto,
                 "ping",
                 MakeGetter(ctx, "ping", ElementGetPing),
                 MakeSetter(ctx, "ping", ElementSetPing));
  DefineInterface(ctx, global, "HTMLAnchorElement", html_anchor_element_proto);
  DefineAccessor(ctx,
                 html_iframe_element_proto,
                 "src",
                 MakeGetter(ctx, "src", ElementGetSrc),
                 MakeSetter(ctx, "src", ElementSetSrc));
  DefineAccessor(ctx,
                 html_iframe_element_proto,
                 "srcdoc",
                 MakeGetter(ctx, "srcdoc", ElementGetSrcDoc),
                 MakeSetter(ctx, "srcdoc", ElementSetSrcDoc));
  DefineAccessor(ctx,
                 html_iframe_element_proto,
                 "credentialless",
                 MakeGetter(ctx, "credentialless", ElementGetCredentialless),
                 MakeSetter(ctx, "credentialless", ElementSetCredentialless));
  DefineInterface(ctx, global, "HTMLIFrameElement", html_iframe_element_proto);
  DefineAccessor(ctx,
                 html_form_element_proto,
                 "action",
                 MakeGetter(ctx, "action", FormGetAction),
                 MakeSetter(ctx, "action", FormSetAction));
  DefineAccessor(ctx,
                 html_form_element_proto,
                 "enctype",
                 MakeGetter(ctx, "enctype", FormGetEnctype),
                 MakeSetter(ctx, "enctype", FormSetEnctype));
  DefineAccessor(ctx,
                 html_form_element_proto,
                 "method",
                 MakeGetter(ctx, "method", FormGetMethod),
                 MakeSetter(ctx, "method", FormSetMethod));
  JS_SetPropertyStr(
      ctx, html_form_element_proto, "submit", JS_NewCFunction(ctx, FormSubmit, "submit", 0));
  JS_SetPropertyStr(ctx,
                    html_form_element_proto,
                    "requestSubmit",
                    JS_NewCFunction(ctx, FormRequestSubmit, "requestSubmit", 1));
  DefineInterface(ctx, global, "HTMLFormElement", html_form_element_proto);
  DefineAccessor(ctx,
                 html_button_element_proto,
                 "formAction",
                 MakeGetter(ctx, "formAction", ElementGetFormAction),
                 MakeSetter(ctx, "formAction", ElementSetFormAction));
  DefineInterface(ctx, global, "HTMLButtonElement", html_button_element_proto);
  DefineAccessor(ctx,
                 html_input_element_proto,
                 "formAction",
                 MakeGetter(ctx, "formAction", ElementGetFormAction),
                 MakeSetter(ctx, "formAction", ElementSetFormAction));
  DefineAccessor(ctx,
                 html_input_element_proto,
                 "value",
                 MakeGetter(ctx, "value", ElementGetValue),
                 MakeSetter(ctx, "value", ElementSetValue));
  DefineGetter(
      ctx, html_input_element_proto, "validity", MakeGetter(ctx, "validity", ElementGetValidity));
  DefineInterface(ctx, global, "HTMLInputElement", html_input_element_proto);
  {
    JSValue validity_state_proto = JS_NewObject(ctx);
    DefineGetter(
        ctx, validity_state_proto, "valid", MakeGetter(ctx, "valid", ValidityStateGetValid));
    DefineGetter(ctx,
                 validity_state_proto,
                 "typeMismatch",
                 MakeGetter(ctx, "typeMismatch", ValidityStateGetTypeMismatch));
    DefineInterface(ctx, global, "ValidityState", validity_state_proto);
    JS_FreeValue(ctx, validity_state_proto);
  }
  DefineAccessor(ctx,
                 html_image_element_proto,
                 "src",
                 MakeGetter(ctx, "src", ElementGetSrc),
                 MakeSetter(ctx, "src", ElementSetSrc));
  DefineAccessor(ctx,
                 html_image_element_proto,
                 "currentSrc",
                 MakeGetter(ctx, "currentSrc", ElementGetCurrentSrc),
                 JS_UNDEFINED);
  DefineAccessor(ctx,
                 html_image_element_proto,
                 "srcset",
                 MakeGetter(ctx, "srcset", ElementGetSrcSet),
                 MakeSetter(ctx, "srcset", ElementSetSrcSet));
  DefineInterface(ctx, global, "HTMLImageElement", html_image_element_proto);
  DefineAccessor(ctx,
                 html_media_element_proto,
                 "src",
                 MakeGetter(ctx, "src", ElementGetSrc),
                 MakeSetter(ctx, "src", ElementSetSrc));
  DefineGetter(ctx,
               html_media_element_proto,
               "currentSrc",
               MakeGetter(ctx, "currentSrc", ElementGetCurrentSrc));
  DefineInterface(ctx, global, "HTMLMediaElement", html_media_element_proto);
  DefineInterface(ctx, global, "HTMLVideoElement", html_video_element_proto);
  DefineInterface(ctx, global, "HTMLCanvasElement", html_canvas_element_proto);
  DefineInterface(ctx, global, "CanvasRenderingContext2D", canvas_2d_proto);
  DefineInterface(ctx, global, "SVGElement", svg_element_proto);
  DefineInterface(ctx, global, "NodeList", node_list_proto);
  DefineInterface(ctx, global, "HTMLCollection", html_collection_proto);
  DefineAccessor(ctx, attr_proto, "name", MakeGetter(ctx, "name", AttrGetName), JS_UNDEFINED);
  DefineAccessor(ctx,
                 attr_proto,
                 "value",
                 MakeGetter(ctx, "value", AttrGetValue),
                 MakeSetter(ctx, "value", AttrSetValue));
  DefineAccessor(ctx,
                 attr_proto,
                 "nodeValue",
                 MakeGetter(ctx, "nodeValue", AttrGetNodeValue),
                 MakeSetter(ctx, "nodeValue", AttrSetNodeValue));
  DefineGetter(
      ctx, attr_proto, "ownerElement", MakeGetter(ctx, "ownerElement", AttrGetOwnerElement));
  DefineInterface(ctx, global, "Attr", attr_proto);
  DefineInterface(ctx, global, "Range", range_proto);
  static const std::array<JSCFunctionListEntry, 2> kNamedNodeMap = {{
      JS_CFUNC_DEF("item", 1, NamedNodeMapItem),
      JS_CFUNC_DEF("getNamedItem", 1, NamedNodeMapGetNamedItem),
  }};
  JS_SetPropertyFunctionList(
      ctx, named_node_map_proto, kNamedNodeMap.data(), static_cast<int>(kNamedNodeMap.size()));
  DefineInterface(ctx, global, "NamedNodeMap", named_node_map_proto);
  static const std::array<JSCFunctionListEntry, 1> kTreeWalker = {{
      JS_CFUNC_DEF("nextNode", 0, TreeWalkerNextNode),
  }};
  JS_SetPropertyFunctionList(
      ctx, tree_walker_proto, kTreeWalker.data(), static_cast<int>(kTreeWalker.size()));
  DefineInterface(ctx, global, "TreeWalker", tree_walker_proto);
  JSValue node_filter = JS_NewObject(ctx);
  static constexpr std::array<std::pair<const char*, uint32_t>, 15> kNodeFilterConstants = {{
      {"FILTER_ACCEPT", 1},
      {"FILTER_REJECT", 2},
      {"FILTER_SKIP", 3},
      {"SHOW_ALL", 0xFFFFFFFFU},
      {"SHOW_ELEMENT", 0x1U},
      {"SHOW_ATTRIBUTE", 0x2U},
      {"SHOW_TEXT", 0x4U},
      {"SHOW_CDATA_SECTION", 0x8U},
      {"SHOW_ENTITY_REFERENCE", 0x10U},
      {"SHOW_ENTITY", 0x20U},
      {"SHOW_PROCESSING_INSTRUCTION", 0x40U},
      {"SHOW_COMMENT", 0x80U},
      {"SHOW_DOCUMENT", 0x100U},
      {"SHOW_DOCUMENT_TYPE", 0x200U},
      {"SHOW_DOCUMENT_FRAGMENT", 0x400U},
  }};
  for (const auto& [name, value] : kNodeFilterConstants) {
    JS_SetPropertyStr(ctx, node_filter, name, JS_NewUint32(ctx, value));
  }
  JS_SetPropertyStr(ctx, global, "NodeFilter", node_filter);
  DefineInterface(ctx, global, "DOMTokenList", class_list_proto);
  static const std::array<JSCFunctionListEntry, 3> kMutationObserver = {{
      JS_CFUNC_DEF("observe", 2, MutationObserverObserve),
      JS_CFUNC_DEF("disconnect", 0, MutationObserverDisconnect),
      JS_CFUNC_DEF("takeRecords", 0, MutationObserverTakeRecords),
  }};
  JS_SetPropertyFunctionList(ctx,
                             mutation_observer_proto,
                             kMutationObserver.data(),
                             static_cast<int>(kMutationObserver.size()));
  JSValue mutation_observer_constructor = JS_NewCFunction2(
      ctx, MutationObserverConstructor, "MutationObserver", 1, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(
      ctx, mutation_observer_constructor, "prototype", JS_DupValue(ctx, mutation_observer_proto));
  JS_SetPropertyStr(
      ctx, mutation_observer_proto, "constructor", JS_DupValue(ctx, mutation_observer_constructor));
  JS_SetPropertyStr(ctx, global, "MutationObserver", mutation_observer_constructor);
  JS_SetPropertyStr(ctx,
                    dom_parser_proto,
                    "parseFromString",
                    JS_NewCFunction(ctx, DOMParserParseFromString, "parseFromString", 2));
  JSValue dom_parser_constructor =
      JS_NewCFunction2(ctx, DOMParserConstructor, "DOMParser", 0, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(ctx, dom_parser_constructor, "prototype", JS_DupValue(ctx, dom_parser_proto));
  JS_SetPropertyStr(ctx, dom_parser_proto, "constructor", JS_DupValue(ctx, dom_parser_constructor));
  JS_SetPropertyStr(ctx, global, "DOMParser", dom_parser_constructor);
  JS_SetPropertyStr(ctx,
                    xml_serializer_proto,
                    "serializeToString",
                    JS_NewCFunction(ctx, XMLSerializerSerializeToString, "serializeToString", 1));
  JSValue xml_serializer_constructor =
      JS_NewCFunction2(ctx, XMLSerializerConstructor, "XMLSerializer", 0, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(
      ctx, xml_serializer_constructor, "prototype", JS_DupValue(ctx, xml_serializer_proto));
  JS_SetPropertyStr(
      ctx, xml_serializer_proto, "constructor", JS_DupValue(ctx, xml_serializer_constructor));
  JS_SetPropertyStr(ctx, global, "XMLSerializer", xml_serializer_constructor);
  static const std::array<JSCFunctionListEntry, 2> kDOMImplementation = {{
      JS_CFUNC_DEF("hasFeature", 2, DocumentImplementationHasFeature),
      JS_CFUNC_DEF("createHTMLDocument", 1, DocumentImplementationCreateHTMLDocument),
  }};
  JS_SetPropertyFunctionList(ctx,
                             dom_implementation_proto,
                             kDOMImplementation.data(),
                             static_cast<int>(kDOMImplementation.size()));
  DefineInterface(ctx, global, "DOMImplementation", dom_implementation_proto);
  // Event is constructable: new Event(type, {bubbles, cancelable}).
  {
    JSValue event_ctor =
        JS_NewCFunction2(ctx, EventConstructor, "Event", 1, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, event_ctor, "prototype", JS_DupValue(ctx, event_proto));   // steals
    JS_SetPropertyStr(ctx, event_proto, "constructor", JS_DupValue(ctx, event_ctor)); // steals
    JS_SetPropertyStr(ctx, global, "Event", event_ctor); // steals event_ctor
  }
  {
    JSValue ui_event_proto = JS_NewObjectProto(ctx, event_proto);
    JSValue ui_event_ctor =
        JS_NewCFunction2(ctx, UIEventConstructor, "UIEvent", 1, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, ui_event_ctor, "prototype", JS_DupValue(ctx, ui_event_proto));
    JS_SetPropertyStr(ctx, ui_event_proto, "constructor", JS_DupValue(ctx, ui_event_ctor));
    JS_SetPropertyStr(ctx, global, "UIEvent", ui_event_ctor);
    JS_FreeValue(ctx, ui_event_proto);
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
  // MessageEvent is constructable and inherits Event.prototype.
  {
    JSValue message_event_ctor =
        JS_NewCFunction2(ctx, MessageEventConstructor, "MessageEvent", 1, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(
        ctx, message_event_ctor, "prototype", JS_DupValue(ctx, message_event_proto)); // steals
    JS_SetPropertyStr(
        ctx, message_event_proto, "constructor", JS_DupValue(ctx, message_event_ctor)); // steals
    JS_SetPropertyStr(ctx, global, "MessageEvent", message_event_ctor);                 // steals
  }
  {
    JSValue csp_event_proto = JS_NewObjectProto(ctx, event_proto);
    JSValue csp_event_ctor = JS_NewCFunction2(ctx,
                                              SecurityPolicyViolationEventConstructor,
                                              "SecurityPolicyViolationEvent",
                                              1,
                                              JS_CFUNC_constructor,
                                              0);
    JS_SetPropertyStr(ctx, csp_event_ctor, "prototype", JS_DupValue(ctx, csp_event_proto));
    JS_SetPropertyStr(ctx, csp_event_proto, "constructor", JS_DupValue(ctx, csp_event_ctor));
    JS_SetPropertyStr(ctx, global, "SecurityPolicyViolationEvent", csp_event_ctor);
    JS_FreeValue(ctx, csp_event_proto);
  }

  {
    JSValue intl = JS_NewObject(ctx);
    JSValue date_time_format_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,
                      date_time_format_proto,
                      "resolvedOptions",
                      JS_NewCFunction(ctx, DateTimeFormatResolvedOptions, "resolvedOptions", 0));
    JSValue date_time_format_ctor = JS_NewCFunction2(
        ctx, DateTimeFormatConstructor, "DateTimeFormat", 0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(
        ctx, date_time_format_ctor, "prototype", JS_DupValue(ctx, date_time_format_proto));
    JS_SetPropertyStr(
        ctx, date_time_format_proto, "constructor", JS_DupValue(ctx, date_time_format_ctor));
    JS_SetPropertyStr(ctx, intl, "DateTimeFormat", date_time_format_ctor);
    JS_FreeValue(ctx, date_time_format_proto);
    JS_SetPropertyStr(ctx, global, "Intl", intl);
  }

  // navigator: engine identity.  The UA matches what the network stack sends;
  // the rest are documented defaults (the browser UI language is not wired
  // yet).  Exposed on both the global scope and the window.
  JSValue navigator_proto = JS_NewObject(ctx);
  JSValue languages = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, languages, 0, JS_NewString(ctx, "en-US")); // steals
  JS_SetPropertyStr(ctx, navigator_proto, "__nekoNavigatorLanguages", languages);
  DefineGetter(
      ctx, navigator_proto, "languages", MakeGetter(ctx, "languages", NavigatorGetLanguages));
  DefineInterface(ctx, global, "Navigator", navigator_proto);
  JSValue navigator_ctor = JS_GetPropertyStr(ctx, global, "Navigator");
  JS_SetPropertyStr(ctx, window, "Navigator", navigator_ctor);
  JSValue navigator = JS_NewObjectProto(ctx, navigator_proto);
  JS_FreeValue(ctx, navigator_proto);
  const std::string user_agent = std::string(base::GetUserAgent());
  JS_SetPropertyStr(ctx, navigator, "userAgent", JS_NewString(ctx, user_agent.c_str()));
  JS_SetPropertyStr(ctx, navigator, "appVersion", JS_NewString(ctx, user_agent.c_str()));
  JS_SetPropertyStr(ctx, navigator, "platform", JS_NewString(ctx, NavigatorPlatform()));
  JS_SetPropertyStr(ctx, navigator, "language", JS_NewString(ctx, "en-US"));
  JS_SetPropertyStr(ctx, navigator, "onLine", JS_TRUE);
  JS_SetPropertyStr(ctx, navigator, "cookieEnabled", JS_TRUE);
  const unsigned cores = std::thread::hardware_concurrency();
  JS_SetPropertyStr(ctx,
                    navigator,
                    "hardwareConcurrency",
                    JS_NewInt32(ctx, static_cast<int>(std::max(1u, cores))));
  JS_SetPropertyStr(ctx, navigator, "vendor", JS_NewString(ctx, ""));
  JSValue mime_types = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, mime_types, "length", JS_NewInt32(ctx, 0));
  auto return_null = [](JSContext* /*inner_ctx*/,
                        JSValueConst /*this_val*/,
                        int /*argc*/,
                        JSValueConst* /*argv*/) -> JSValue { return JS_NULL; };
  JS_SetPropertyStr(ctx, mime_types, "item", JS_NewCFunction(ctx, return_null, "item", 1));
  JS_SetPropertyStr(
      ctx, mime_types, "namedItem", JS_NewCFunction(ctx, return_null, "namedItem", 1));
  JS_SetPropertyStr(ctx, navigator, "mimeTypes", mime_types);

  // navigator.sendBeacon: minimal stub (returns true without sending).
  auto send_beacon = [](JSContext* /*inner_ctx*/,
                        JSValueConst /*this_val*/,
                        int /*argc*/,
                        JSValueConst* /*argv*/) -> JSValue { return JS_TRUE; };
  JS_SetPropertyStr(
      ctx, navigator, "sendBeacon", JS_NewCFunction(ctx, send_beacon, "sendBeacon", 2));

  // navigator.clipboard: stub object with writeText/readText returning resolved promises.
  JSValue clipboard = JS_NewObject(ctx);
  auto clipboard_write_text = [](JSContext* inner_ctx,
                                 JSValueConst /*this_val*/,
                                 int /*argc*/,
                                 JSValueConst* /*argv*/) -> JSValue {
    JSValue resolve_fn = JS_NewCFunction(
        inner_ctx,
        [](JSContext* /*c*/, JSValueConst /*t*/, int /*a*/, JSValueConst* /*v*/) -> JSValue {
          return JS_UNDEFINED;
        },
        "",
        0);
    JSValue promise = JS_Call(inner_ctx, resolve_fn, JS_UNDEFINED, 0, nullptr);
    JS_FreeValue(inner_ctx, resolve_fn);
    return promise;
  };
  JS_SetPropertyStr(
      ctx, clipboard, "writeText", JS_NewCFunction(ctx, clipboard_write_text, "writeText", 1));
  JS_SetPropertyStr(
      ctx, clipboard, "readText", JS_NewCFunction(ctx, clipboard_write_text, "readText", 0));
  JS_SetPropertyStr(ctx, navigator, "clipboard", clipboard);

  // navigator.geolocation: stub with getCurrentPosition/watchPosition that never invoke callbacks.
  JSValue geolocation = JS_NewObject(ctx);
  auto geo_noop = [](JSContext* /*inner_ctx*/,
                     JSValueConst /*this_val*/,
                     int /*argc*/,
                     JSValueConst* /*argv*/) -> JSValue { return JS_UNDEFINED; };
  JS_SetPropertyStr(ctx,
                    geolocation,
                    "getCurrentPosition",
                    JS_NewCFunction(ctx, geo_noop, "getCurrentPosition", 1));
  JS_SetPropertyStr(
      ctx, geolocation, "watchPosition", JS_NewCFunction(ctx, geo_noop, "watchPosition", 1));
  JS_SetPropertyStr(
      ctx, geolocation, "clearWatch", JS_NewCFunction(ctx, geo_noop, "clearWatch", 1));
  JS_SetPropertyStr(ctx, navigator, "geolocation", geolocation);

  // navigator.mediaDevices: stub returning empty enumerateDevices().
  JSValue media_devices = JS_NewObject(ctx);
  auto enumerate_devices = [](JSContext* inner_ctx,
                              JSValueConst /*this_val*/,
                              int /*argc*/,
                              JSValueConst* /*argv*/) -> JSValue {
    JSValue arr = JS_NewArray(inner_ctx);
    JSValue resolve_fn = JS_NewCFunction(
        inner_ctx,
        [](JSContext* c, JSValueConst /*t*/, int a, JSValueConst* v) -> JSValue {
          return a > 0 ? JS_DupValue(c, v[0]) : JS_UNDEFINED;
        },
        "",
        1);
    JSValueConst args[] = {arr};
    JSValue promise = JS_Call(inner_ctx, resolve_fn, JS_UNDEFINED, 1, args);
    JS_FreeValue(inner_ctx, resolve_fn);
    JS_FreeValue(inner_ctx, arr);
    return promise;
  };
  JS_SetPropertyStr(ctx,
                    media_devices,
                    "enumerateDevices",
                    JS_NewCFunction(ctx, enumerate_devices, "enumerateDevices", 0));
  JS_SetPropertyStr(ctx, navigator, "mediaDevices", media_devices);

  // navigator.permissions: stub query() returning { state: "granted" }.
  JSValue permissions = JS_NewObject(ctx);
  auto permissions_query = [](JSContext* inner_ctx,
                              JSValueConst /*this_val*/,
                              int /*argc*/,
                              JSValueConst* /*argv*/) -> JSValue {
    JSValue status = JS_NewObject(inner_ctx);
    JS_SetPropertyStr(inner_ctx, status, "state", JS_NewString(inner_ctx, "granted"));
    JSValue resolve_fn = JS_NewCFunction(
        inner_ctx,
        [](JSContext* c, JSValueConst /*t*/, int a, JSValueConst* v) -> JSValue {
          return a > 0 ? JS_DupValue(c, v[0]) : JS_UNDEFINED;
        },
        "",
        1);
    JSValueConst args[] = {status};
    JSValue promise = JS_Call(inner_ctx, resolve_fn, JS_UNDEFINED, 1, args);
    JS_FreeValue(inner_ctx, resolve_fn);
    JS_FreeValue(inner_ctx, status);
    return promise;
  };
  JS_SetPropertyStr(ctx, permissions, "query", JS_NewCFunction(ctx, permissions_query, "query", 1));
  JS_SetPropertyStr(ctx, navigator, "permissions", permissions);

  // navigator.connection: stub NetworkInformation object.
  JSValue connection = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, connection, "effectiveType", JS_NewString(ctx, "4g"));
  JS_SetPropertyStr(ctx, connection, "downlink", JS_NewFloat64(ctx, 10.0));
  JS_SetPropertyStr(ctx, connection, "rtt", JS_NewInt32(ctx, 50));
  JS_SetPropertyStr(ctx, connection, "saveData", JS_FALSE);
  JS_SetPropertyStr(ctx, navigator, "connection", connection);

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
    return JS_NewCFunction(
        ctx,
        [](JSContext* /*inner_ctx*/,
           JSValueConst /*this_val*/,
           int /*argc*/,
           JSValueConst* /*argv*/) -> JSValue { return JS_UNDEFINED; },
        name,
        0);
  };

  JSValue perf_observer_proto = JS_NewObject(ctx);
  auto performance_observer_noop = [](JSContext* /*inner_ctx*/,
                                      JSValueConst /*this_val*/,
                                      int /*argc*/,
                                      JSValueConst* /*argv*/) -> JSValue { return JS_UNDEFINED; };
  auto performance_observer_take_records = [](JSContext* inner_ctx,
                                              JSValueConst /*this_val*/,
                                              int /*argc*/,
                                              JSValueConst* /*argv*/) -> JSValue {
    return JS_NewArray(inner_ctx);
  };
  JS_SetPropertyStr(ctx,
                    perf_observer_proto,
                    "observe",
                    JS_NewCFunction(ctx, performance_observer_noop, "observe", 1));
  JS_SetPropertyStr(ctx,
                    perf_observer_proto,
                    "disconnect",
                    JS_NewCFunction(ctx, performance_observer_noop, "disconnect", 0));
  JS_SetPropertyStr(ctx,
                    perf_observer_proto,
                    "takeRecords",
                    JS_NewCFunction(ctx, performance_observer_take_records, "takeRecords", 0));
  JS_SetPropertyFunctionList(ctx, perf_observer_proto, nullptr, 0);
  JSValue performance_observer_ctor = JS_NewCFunction2(
      ctx,
      [](JSContext* inner_ctx, JSValueConst new_target, int /*argc*/, JSValueConst* /*argv*/)
          -> JSValue {
        JSValue proto = JS_GetPropertyStr(inner_ctx, new_target, "prototype");
        if (JS_IsException(proto)) {
          return proto;
        }
        JSValue observer = JS_NewObjectProto(inner_ctx, proto);
        JS_FreeValue(inner_ctx, proto);
        return observer;
      },
      "PerformanceObserver",
      1,
      JS_CFUNC_constructor,
      0);
  JS_SetPropertyStr(ctx, performance_observer_ctor, "prototype", perf_observer_proto); // steals
  // PerformanceObserver.supportedEntryTypes: the engine implements no
  // performance entry types today, so the honest list is empty (feature
  // detection then skips the observer path; see the compatibility matrix).
  JS_SetPropertyStr(ctx, performance_observer_ctor, "supportedEntryTypes", JS_NewArray(ctx));
  JS_SetPropertyStr(
      ctx, window, "PerformanceObserver", JS_DupValue(ctx, performance_observer_ctor));
  JS_SetPropertyStr(ctx, global, "PerformanceObserver", performance_observer_ctor);

  JSValue service_worker = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, service_worker, "controller", JS_NULL);
  JS_SetPropertyStr(ctx, navigator, "serviceWorker", JS_DupValue(ctx, service_worker));
  JS_SetPropertyStr(ctx, window, "serviceWorker", JS_DupValue(ctx, service_worker));
  JS_SetPropertyStr(ctx, global, "serviceWorker", service_worker);

  // The window's viewport in CSS pixels (PageApis::viewport_size — the
  // renderer's layout viewport, so it follows window resizes and the page
  // zoom) and the device pixel ratio.  Without the callback the engine default
  // 800x600@1x is reported.
  const int viewport_w = apis.viewport_size ? apis.viewport_size().first : 800;
  const int viewport_h = apis.viewport_size ? apis.viewport_size().second : 600;

  JSValue visual_viewport = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, visual_viewport, "width", JS_NewFloat64(ctx, viewport_w));
  JS_SetPropertyStr(ctx, visual_viewport, "height", JS_NewFloat64(ctx, viewport_h));
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

  // screen: reports the window's viewport (see the declaration above), so it
  // follows window resizes and the page zoom.  A real display-size query (Qt
  // screen geometry) is future work.
  JSValue screen = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, screen, "width", JS_NewInt32(ctx, viewport_w));
  JS_SetPropertyStr(ctx, screen, "height", JS_NewInt32(ctx, viewport_h));
  JS_SetPropertyStr(ctx, screen, "availWidth", JS_NewInt32(ctx, viewport_w));
  JS_SetPropertyStr(ctx, screen, "availHeight", JS_NewInt32(ctx, viewport_h));
  JS_SetPropertyStr(ctx, screen, "colorDepth", JS_NewInt32(ctx, 24));
  JS_SetPropertyStr(ctx, screen, "pixelDepth", JS_NewInt32(ctx, 24));
  JS_SetPropertyStr(ctx, window, "screen", JS_DupValue(ctx, screen)); // steals dup
  JS_SetPropertyStr(ctx, global, "screen", screen);                   // steals

  // Window viewport geometry (live reads, see screen above).  The scroll
  // offsets are live reads off the browser layer's scroll state (PageApis
  // scroll_offset), so window.scrollX/scrollY/pageXOffset/pageYOffset track the
  // GUI scrollbar; without the callback they report 0.
  DefineGetter(ctx,
               window,
               "innerWidth",
               MakeGetterMagic(ctx, "innerWidth", WindowViewportGetter, 0));
  DefineGetter(ctx,
               window,
               "innerHeight",
               MakeGetterMagic(ctx, "innerHeight", WindowViewportGetter, 1));
  DefineGetter(ctx,
               window,
               "outerWidth",
               MakeGetterMagic(ctx, "outerWidth", WindowViewportGetter, 0));
  DefineGetter(ctx,
               window,
               "outerHeight",
               MakeGetterMagic(ctx, "outerHeight", WindowViewportGetter, 2));
  DefineGetter(ctx,
               window,
               "devicePixelRatio",
               MakeGetterMagic(ctx, "devicePixelRatio", WindowViewportGetter, 3));
  DefineGetter(
      ctx, window, "pageXOffset", MakeGetterMagic(ctx, "pageXOffset", WindowScrollOffsetGetter, 0));
  DefineGetter(
      ctx, window, "pageYOffset", MakeGetterMagic(ctx, "pageYOffset", WindowScrollOffsetGetter, 1));
  DefineGetter(
      ctx, window, "scrollX", MakeGetterMagic(ctx, "scrollX", WindowScrollOffsetGetter, 2));
  DefineGetter(
      ctx, window, "scrollY", MakeGetterMagic(ctx, "scrollY", WindowScrollOffsetGetter, 3));

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

  // Page Web APIs (Phase 8 M3 subset): localStorage uses the browser-layer
  // backing store while sessionStorage is isolated to this document runtime.
  {
    JSValue storage_proto = JS_NewObject(ctx);
    static const std::array<JSCFunctionListEntry, 5> kStorage = {{
        JS_CFUNC_DEF("getItem", 1, LocalStorageGetItem),
        JS_CFUNC_DEF("setItem", 2, LocalStorageSetItem),
        JS_CFUNC_DEF("removeItem", 1, LocalStorageRemoveItem),
        JS_CFUNC_DEF("clear", 0, LocalStorageClear),
        JS_CFUNC_DEF("key", 1, LocalStorageKey),
    }};
    JS_SetPropertyFunctionList(
        ctx, storage_proto, kStorage.data(), static_cast<int>(kStorage.size()));
    DefineGetter(ctx, storage_proto, "length", MakeGetter(ctx, "length", LocalStorageLength));
    DefineInterface(ctx, global, "Storage", storage_proto);
    JSValue storage_ctor = JS_GetPropertyStr(ctx, global, "Storage");
    JS_SetPropertyStr(ctx, window, "Storage", storage_ctor);
    JSValue local_storage = JS_NewObjectProto(ctx, storage_proto);
    JS_SetPropertyStr(ctx, window, "localStorage", JS_DupValue(ctx, local_storage)); // steals
    JS_SetPropertyStr(ctx, global, "localStorage", local_storage);                   // steals
    JSValue session_storage_object = JS_NewObjectProto(ctx, storage_proto);
    JS_SetPropertyStr(ctx, session_storage_object, "__nekoSessionStorage", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, window, "sessionStorage", JS_DupValue(ctx, session_storage_object));
    JS_SetPropertyStr(ctx, global, "sessionStorage", session_storage_object);
    JS_FreeValue(ctx, storage_proto);
  }
  if (apis.fetch) {
    InstallResponseGlobal(ctx, global);
    JSValue response_ctor = JS_GetPropertyStr(ctx, global, "Response");
    JS_SetPropertyStr(ctx, window, "Response", response_ctor);
    JSValue fetch_fn = JS_NewCFunction(ctx, JsFetch, "fetch", 1);
    JS_SetPropertyStr(ctx, window, "fetch", JS_DupValue(ctx, fetch_fn)); // steals
    JS_SetPropertyStr(ctx, global, "fetch", fetch_fn);                   // steals
  }
  if (apis.xhr_request) {
    InstallXhrGlobal(ctx, *this);
  }
  // WebSocket is always available (connects directly via neko::network).
  InstallWebSocketGlobal(ctx, *this);
  JSValue blob_ctor = JS_NewCFunction2(ctx, BlobConstructor, "Blob", 2, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(ctx, window, "Blob", JS_DupValue(ctx, blob_ctor));
  JS_SetPropertyStr(ctx, global, "Blob", blob_ctor);
  JSValue image_ctor = JS_NewCFunction2(ctx, ImageConstructor, "Image", 2, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(ctx, window, "Image", JS_DupValue(ctx, image_ctor)); // steals
  JS_SetPropertyStr(ctx, global, "Image", image_ctor);                   // steals
  InstallUrlGlobal(ctx, global);
  JSValue url_constructor = JS_GetPropertyStr(ctx, global, "URL");
  JS_SetPropertyStr(ctx, window, "URL", url_constructor);
  InstallUrlSearchParamsGlobal(ctx, global);
  InstallFormDataGlobal(ctx, global);
  InstallHeadersGlobal(ctx, global);
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
  // Stop WebSocket I/O threads first: they must not outlive the binder.
  ShutdownWebSockets(*this);
  CloseMessagePorts(*this);
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
  for (const auto& entry : canvas_contexts) {
    JS_FreeValue(ctx, entry.second);
  }
  canvas_contexts.clear();
  DetachLiveCollections();

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

  for (MediaListener& listener : media_listeners) {
    JS_FreeValue(ctx, listener.list);
    JS_FreeValue(ctx, listener.callback);
  }
  media_listeners.clear();

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
                           "customElements",
                           "crypto",
                           "Node",
                           "Attr",
                           "Document",
                           "Text",
                           "Comment",
                           "DocumentFragment",
                           "CSSStyleDeclaration",
                           "Element",
                           "HTMLElement",
                           "HTMLIFrameElement",
                           "HTMLVideoElement",
                           "HTMLCanvasElement",
                           "CanvasRenderingContext2D",
                           "SVGElement",
                           "MutationObserver",
                           "Event",
                           "UIEvent",
                           "CustomEvent",
                           "CSSStyleSheet",
                           "CSSRule",
                           "AbortController",
                           "AbortSignal",
                           "IntersectionObserver",
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
                           "FormData",
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
  JS_FreeValue(ctx, character_data_proto);
  JS_FreeValue(ctx, document_type_proto);
  JS_FreeValue(ctx, text_proto);
  JS_FreeValue(ctx, comment_proto);
  JS_FreeValue(ctx, document_proto);
  JS_FreeValue(ctx, fragment_proto);
  JS_FreeValue(ctx, style_proto);
  JS_FreeValue(ctx, event_proto);
  JS_FreeValue(ctx, custom_event_proto);
  JS_FreeValue(ctx, message_event_proto);
  JS_FreeValue(ctx, event_target_proto);
  JS_FreeValue(ctx, abort_signal_proto);
  JS_FreeValue(ctx, abort_controller_proto);
  JS_FreeValue(ctx, intersection_observer_proto);
  JS_FreeValue(ctx, class_list_proto);
  JS_FreeValue(ctx, node_list_proto);
  JS_FreeValue(ctx, html_collection_proto);
  JS_FreeValue(ctx, named_node_map_proto);
  JS_FreeValue(ctx, attr_proto);
  JS_FreeValue(ctx, tree_walker_proto);
  JS_FreeValue(ctx, range_proto);
  JS_FreeValue(ctx, mutation_observer_proto);
  JS_FreeValue(ctx, dom_parser_proto);
  JS_FreeValue(ctx, xml_serializer_proto);
  JS_FreeValue(ctx, dom_implementation_proto);
  JS_FreeValue(ctx, html_base_element_proto);
  JS_FreeValue(ctx, html_script_element_proto);
  JS_FreeValue(ctx, html_anchor_element_proto);
  JS_FreeValue(ctx, html_iframe_element_proto);
  JS_FreeValue(ctx, html_form_element_proto);
  JS_FreeValue(ctx, html_button_element_proto);
  JS_FreeValue(ctx, html_input_element_proto);
  JS_FreeValue(ctx, html_image_element_proto);
  JS_FreeValue(ctx, html_media_element_proto);
  JS_FreeValue(ctx, html_video_element_proto);
  JS_FreeValue(ctx, html_canvas_element_proto);
  JS_FreeValue(ctx, canvas_2d_proto);
  JS_FreeValue(ctx, svg_element_proto);
  JS_FreeValue(ctx, xhr_proto);
  JS_FreeValue(ctx, message_port_proto);
  JS_FreeValue(ctx, css_style_sheet_proto);
  JS_FreeValue(ctx, css_rule_proto);
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
    std::lock_guard<std::mutex> lock(g_attr_class_mutex);
    g_attr_class_registered.erase(rt);
  }
  {
    std::lock_guard<std::mutex> lock(g_event_class_mutex);
    g_event_class_registered.erase(rt);
  }
  {
    std::lock_guard<std::mutex> lock(g_dataset_class_mutex);
    g_dataset_class_registered.erase(rt);
  }
  {
    std::lock_guard<std::mutex> lock(g_canvas_class_mutex);
    g_canvas_class_registered.erase(rt);
  }
  ForgetUrlSearchParamsRuntime(rt);
  ForgetFormDataRuntime(rt);
  ForgetHeadersRuntime(rt);
  ForgetMessageChannelRuntime(rt);
  ForgetEventTargetRuntime(rt);
  ForgetAbortRuntime(rt);
  ForgetIntersectionObserverRuntime(rt);

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

JSValue Impl::WrapAttribute(dom::Element* element, std::string_view name)
{
  if (element == nullptr) {
    return JS_NULL;
  }
  auto* wrapper = new AttrWrapper{this, element, std::string(name)};
  JSValue obj = JS_NewObjectClass(ctx, g_attr_class_id);
  JS_SetOpaque(obj, wrapper);
  JS_SetPrototype(ctx, obj, attr_proto);
  return obj;
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
    if (element->tag_name() == "base") {
      return html_base_element_proto;
    }
    if (element->tag_name() == "script") {
      return html_script_element_proto;
    }
    if (element->tag_name() == "a") {
      return html_anchor_element_proto;
    }
    if (element->tag_name() == "iframe") {
      return html_iframe_element_proto;
    }
    if (element->tag_name() == "form") {
      return html_form_element_proto;
    }
    if (element->tag_name() == "button") {
      return html_button_element_proto;
    }
    if (element->tag_name() == "input") {
      return html_input_element_proto;
    }
    if (element->tag_name() == "img") {
      return html_image_element_proto;
    }
    if (element->tag_name() == "video") {
      return html_video_element_proto;
    }
    if (element->tag_name() == "canvas") {
      return html_canvas_element_proto;
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

JSValue Impl::MakeHtmlCollection(const std::vector<dom::Element*>& elements)
{
  JSValue collection = JS_NewArray(ctx);
  JS_SetPrototype(ctx, collection, html_collection_proto);
  for (std::size_t i = 0; i < elements.size(); ++i) {
    JS_SetPropertyUint32(ctx, collection, static_cast<uint32_t>(i), WrapNode(elements[i]));
  }
  return collection;
}

namespace {

// Recursively collects, in document order, every descendant element of |root|
// (|root| itself excluded) for which |pred| returns true.
template <typename Pred>
void CollectDescendantElements(const dom::Node& root, Pred pred, std::vector<dom::Node*>& out)
{
  for (dom::Node* child : root.ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      if (pred(*el)) {
        out.push_back(el);
      }
      CollectDescendantElements(*el, pred, out);
    }
  }
}

} // namespace

JSValue Impl::MakeLiveCollection(dom::Node* root, LiveKind kind, const std::string& arg)
{
  JSValue array = JS_NewArray(ctx);
  JS_SetPrototype(
      ctx, array, kind == LiveKind::kChildNodes ? node_list_proto : html_collection_proto);
  const std::vector<dom::Node*> nodes = QueryLive(root, kind, arg);
  JS_SetPropertyStr(ctx, array, "length", JS_NewInt32(ctx, static_cast<int32_t>(nodes.size())));
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    JS_SetPropertyUint32(ctx, array, static_cast<uint32_t>(i), WrapNode(nodes[i])); // steals
  }
  // The collection is kept alive by the binder (RefreshLiveCollections) AND
  // returned as an owned reference to the caller, so the registry holds its
  // own duplicate reference.
  live_collections.push_back(LiveCollection{root, kind, arg, JS_DupValue(ctx, array)});
  return array; // one owned reference for the caller
}

std::vector<dom::Node*>
Impl::QueryLive(dom::Node* root, LiveKind kind, const std::string& arg) const
{
  std::vector<dom::Node*> out;
  if (root == nullptr) {
    return out;
  }
  switch (kind) {
  case LiveKind::kChildNodes:
    for (dom::Node* child : root->ChildNodes()) {
      out.push_back(child);
    }
    break;
  case LiveKind::kChildren:
    for (dom::Node* child : root->ChildNodes()) {
      if (AsElement(child) != nullptr) {
        out.push_back(child);
      }
    }
    break;
  case LiveKind::kTagName: {
    const std::string tag = ToLower(arg);
    CollectDescendantElements(
        *root, [&tag](const dom::Element& el) { return tag == "*" || el.tag_name() == tag; }, out);
    break;
  }
  case LiveKind::kClassName: {
    // Space-separated class names: an element matches when its class token
    // list contains every requested token (the getElementsByClassName spec
    // behavior).  Tokenize once per query rather than per element.
    std::vector<std::string> tokens;
    {
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
          tokens.emplace_back(arg.substr(start, i - start));
        }
      }
    }
    CollectDescendantElements(
        *root,
        [&tokens](const dom::Element& el) {
          const std::vector<std::string_view> actual = el.ClassList();
          for (const std::string& token : tokens) {
            if (std::find(actual.begin(), actual.end(), token) == actual.end()) {
              return false;
            }
          }
          return true;
        },
        out);
    break;
  }
  case LiveKind::kForms:
    CollectDescendantElements(
        *root, [](const dom::Element& el) { return el.tag_name() == "form"; }, out);
    break;
  case LiveKind::kImages:
    CollectDescendantElements(
        *root, [](const dom::Element& el) { return el.tag_name() == "img"; }, out);
    break;
  case LiveKind::kLinks:
    CollectDescendantElements(
        *root,
        [](const dom::Element& el) {
          // document.links: <a> and <area> elements with an href.
          return (el.tag_name() == "a" || el.tag_name() == "area") && el.HasAttribute("href");
        },
        out);
    break;
  case LiveKind::kScripts:
    CollectDescendantElements(
        *root, [](const dom::Element& el) { return el.tag_name() == "script"; }, out);
    break;
  }
  return out;
}

void Impl::RefreshLiveCollections()
{
  if (ctx == nullptr) {
    return;
  }
  for (LiveCollection& collection : live_collections) {
    const std::vector<dom::Node*> nodes =
        QueryLive(collection.root, collection.kind, collection.arg);
    // Setting length first truncates any stale trailing indexes.
    JS_SetPropertyStr(
        ctx, collection.array, "length", JS_NewInt32(ctx, static_cast<int32_t>(nodes.size())));
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      JS_SetPropertyUint32(ctx, collection.array, static_cast<uint32_t>(i), WrapNode(nodes[i]));
    }
  }
}

void Impl::DetachLiveCollections()
{
  for (LiveCollection& collection : live_collections) {
    JS_FreeValue(ctx, collection.array);
  }
  live_collections.clear();
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
      for (dom::Node* ancestor = target->parent(); ancestor != nullptr;
           ancestor = ancestor->parent()) {
        if (ancestor == observer.target) {
          matches = true;
          break;
        }
      }
    }
    if (matches) {
      observer.records.push_back(
          MutationRecord{"childList", target, std::move(added), std::move(removed), {}});
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
      for (dom::Node* ancestor = target->parent(); ancestor != nullptr;
           ancestor = ancestor->parent()) {
        if (ancestor == observer.target) {
          matches = true;
          break;
        }
      }
    }
    if (matches) {
      observer.records.push_back(
          MutationRecord{"attributes", target, {}, {}, std::move(attribute_name)});
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
      for (dom::Node* ancestor = target->parent(); ancestor != nullptr;
           ancestor = ancestor->parent()) {
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
                        record.attribute_name.empty()
                            ? JS_NULL
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
  int ran = RunPendingMessagePortTasks(*this);
  ran += PumpWebSocketEvents(*this);
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
// Global constructors installed by the constructor (see Impl::Impl).
// ---------------------------------------------------------------------------

JSValue ImageConstructor(JSContext* ctx, JSValueConst /*new_target*/, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  // new Image([width[, height]]): a detached <img> element, owned by the
  // binder until script inserts it into the document.
  auto element = std::make_unique<dom::Element>("img");
  dom::Element* raw = element.get();
  for (int i = 0; i < 2; ++i) {
    if (argc > i && !JS_IsUndefined(argv[i]) && !JS_IsNull(argv[i])) {
      int32_t dimension = 0;
      if (JS_ToInt32(ctx, &dimension, argv[i]) == 0 && dimension > 0) {
        element->SetAttribute(i == 0 ? "width" : "height", std::to_string(dimension));
      } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
      }
    }
  }
  impl->created[raw] = std::move(element);
  return impl->WrapNode(raw);
}

} // namespace neko::javascript
