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
  EnsureEventClassRegistered(rt);
  EnsureDatasetClassRegistered(rt);
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
  JS_SetPropertyStr(
      ctx,
      global,
      "MutationObserver",
      JS_NewCFunction2(
          ctx, MutationObserverConstructor, "MutationObserver", 1, JS_CFUNC_constructor, 0));
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
  static const std::array<JSCFunctionListEntry, 3> kPerformanceObserver = {{
      JS_CFUNC_DEF("observe", 0, nullptr),
      JS_CFUNC_DEF("disconnect", 0, nullptr),
      JS_CFUNC_DEF("takeRecords", 0, nullptr),
  }};
  JS_SetPropertyFunctionList(ctx,
                             perf_observer_proto,
                             kPerformanceObserver.data(),
                             static_cast<int>(kPerformanceObserver.size()));
  JSValue performance_observer_ctor = JS_NewCFunction2(
      ctx,
      [](JSContext* inner_ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
          -> JSValue {
        JSValue observer = JS_NewObject(inner_ctx);
        return observer;
      },
      "PerformanceObserver",
      1,
      JS_CFUNC_constructor,
      0);
  JS_SetPropertyStr(ctx, performance_observer_ctor, "prototype", perf_observer_proto); // steals
  JS_SetPropertyStr(
      ctx, window, "PerformanceObserver", JS_DupValue(ctx, performance_observer_ctor));
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
  JS_SetPropertyStr(ctx,
                    url_object,
                    "createObjectURL",
                    JS_NewCFunction(ctx, UrlCreateObjectUrl, "createObjectURL", 1));
  JS_SetPropertyStr(ctx,
                    url_object,
                    "revokeObjectURL",
                    JS_NewCFunction(ctx, UrlRevokeObjectUrl, "revokeObjectURL", 1));
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

} // namespace neko::javascript
