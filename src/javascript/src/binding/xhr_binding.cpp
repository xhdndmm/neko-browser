// neko::javascript DOM bindings — XMLHttpRequest family.
//
// Implements the practical XHR subset real pages rely on (AMD loaders such
// as esl.js, legacy jQuery code): constructor, open/setRequestHeader/send/
// abort, readyState/status/statusText/responseText/response/responseURL,
// getResponseHeader/getAllResponseHeaders, onreadystatechange/onload/onerror/
// onabort plus addEventListener for the same event types.
//
// Documented approximations:
//   * The transport is synchronous (the engine's page pipeline is
//     synchronous), so the DONE transition fires inside send().  Handlers
//     registered before send() observe the full lifecycle, which is the
//     universal loader pattern; code that assigns handlers after send()
//     misses events (browsers deliver them asynchronously).
//   * Non-GET methods depend on the browser layer's transport wiring; the
//     callback rejects what it cannot perform.
//   * withCredentials/timeout are accepted but inert; cookies always follow
//     the browser layer's subresource policy.  No CORS yet (Phase 10).

#include "binding_internal.h"

#include <algorithm>
#include <cstring>
#include <quickjs.h>
#include <string>
#include <vector>

namespace neko::javascript {

namespace {

JSClassID g_xhr_class_id = 0;
std::mutex g_xhr_class_mutex;
std::unordered_set<JSRuntime*> g_xhr_class_registered;

void XhrFinalizer(JSRuntime* rt, JSValue obj)
{
  auto* w = static_cast<XhrWrapper*>(JS_GetOpaque(obj, g_xhr_class_id));
  if (w == nullptr) {
    return;
  }
  // Handlers/listeners are owned JSValues: free them with the runtime-scoped
  // API (the context may already be gone).
  JS_FreeValueRT(rt, w->on_ready_state_change);
  JS_FreeValueRT(rt, w->on_load);
  JS_FreeValueRT(rt, w->on_error);
  JS_FreeValueRT(rt, w->on_abort);
  for (auto& entry : w->listeners) {
    JS_FreeValueRT(rt, entry.second);
  }
  delete w;
}

// The wrapper owns handler JSValues in its opaque payload; without gc_mark a
// live XMLHttpRequest could collect its handlers at runtime teardown (same
// requirement as EventWrapper).
void XhrGcMark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
  auto* w = static_cast<XhrWrapper*>(JS_GetOpaque(val, g_xhr_class_id));
  if (w == nullptr) {
    return;
  }
  JS_MarkValue(rt, w->on_ready_state_change, mark_func);
  JS_MarkValue(rt, w->on_load, mark_func);
  JS_MarkValue(rt, w->on_error, mark_func);
  JS_MarkValue(rt, w->on_abort, mark_func);
  for (auto& entry : w->listeners) {
    JS_MarkValue(rt, entry.second, mark_func);
  }
}

XhrWrapper* XhrOf(JSContext* /*ctx*/, JSValueConst this_val)
{
  auto* w = static_cast<XhrWrapper*>(JS_GetOpaque(this_val, g_xhr_class_id));
  if (w != nullptr && w->impl != nullptr) {
    return w;
  }
  return nullptr;
}

// Fires |type| ("readystatechange"|"load"|"error"|"abort") on the XHR:
// invokes the matching on* handler and every registered listener with the
// XHR object as |this|.  A throwing handler is reported like other page
// callbacks (console sink) and does not stop the remaining ones.
void FireXhrEvent(JSContext* ctx, XhrWrapper* w, JSValueConst obj, const char* type)
{
  std::vector<JSValue> handlers;
  if (type == std::string("readystatechange") && !JS_IsUndefined(w->on_ready_state_change) &&
      !JS_IsNull(w->on_ready_state_change)) {
    handlers.push_back(JS_DupValue(ctx, w->on_ready_state_change));
  }
  if (std::string(type) == "load" && !JS_IsUndefined(w->on_load) && !JS_IsNull(w->on_load)) {
    handlers.push_back(JS_DupValue(ctx, w->on_load));
  }
  if (std::string(type) == "error" && !JS_IsUndefined(w->on_error) && !JS_IsNull(w->on_error)) {
    handlers.push_back(JS_DupValue(ctx, w->on_error));
  }
  if (std::string(type) == "abort" && !JS_IsUndefined(w->on_abort) && !JS_IsNull(w->on_abort)) {
    handlers.push_back(JS_DupValue(ctx, w->on_abort));
  }
  for (auto& entry : w->listeners) {
    if (entry.first == type && JS_IsFunction(ctx, entry.second)) {
      handlers.push_back(JS_DupValue(ctx, entry.second));
    }
  }
  for (JSValue handler : handlers) {
    JSValue result = JS_Call(ctx, handler, obj, 0, nullptr);
    if (JS_IsException(result)) {
      JS_FreeValue(ctx, JS_GetException(ctx)); // reported via console sink by callers otherwise
    }
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, handler);
  }
}

JSValue
XhrConstructor(JSContext* ctx, JSValueConst /*new_target*/, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  auto* w = new XhrWrapper();
  w->impl = impl;
  JSValue obj = JS_NewObjectProtoClass(ctx, impl->xhr_proto, g_xhr_class_id);
  if (JS_IsException(obj)) {
    delete w;
    return obj;
  }
  JS_SetOpaque(obj, w);
  return obj;
}

JSValue XhrOpen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* w = XhrOf(ctx, this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an XMLHttpRequest");
  }
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "open requires (method, url)");
  }
  bool ok = false;
  const std::string method = ToUpper(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::string raw_url = ArgString(ctx, argv[1], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
    w->async_requested = JS_ToBool(ctx, argv[2]) > 0;
  }
  w->method = method;
  // Resolve relative URLs against the page base (fetch()/location share it).
  if (w->impl->apis.resolve_url) {
    w->url = w->impl->apis.resolve_url(raw_url);
    if (w->url.empty()) {
      w->url = raw_url;
    }
  } else {
    w->url = raw_url;
  }
  w->request_headers.clear();
  w->status = 0;
  w->status_text.clear();
  w->response_text.clear();
  w->response_headers.clear();
  w->ready_state = 1; // OPENED
  FireXhrEvent(ctx, w, this_val, "readystatechange");
  return JS_UNDEFINED;
}

JSValue XhrSetRequestHeader(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* w = XhrOf(ctx, this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an XMLHttpRequest");
  }
  if (argc < 2 || w->ready_state != 1) {
    return w->ready_state != 1 ? JS_ThrowTypeError(ctx, "setRequestHeader before open()")
                               : JS_ThrowTypeError(ctx, "setRequestHeader requires (name, value)");
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
  w->request_headers.emplace_back(name, value);
  return JS_UNDEFINED;
}

JSValue XhrSend(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* w = XhrOf(ctx, this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an XMLHttpRequest");
  }
  if (w->ready_state != 1) {
    return JS_ThrowTypeError(ctx, "send before open()");
  }
  if (!w->impl->apis.xhr_request) {
    return JS_ThrowTypeError(ctx, "XMLHttpRequest is not available");
  }
  std::string body;
  if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
    bool ok = false;
    body = ArgString(ctx, argv[0], &ok);
    if (!ok) {
      return JS_EXCEPTION;
    }
  }
  const base::Result<FetchResponse> response =
      w->impl->apis.xhr_request(w->url, w->method, w->request_headers, body);
  w->ready_state = 4; // DONE
  if (!response.has_value()) {
    // Network error: status stays 0, body empty; fire readystatechange then
    // error (DOM Standard §5.1 "the error steps").
    FireXhrEvent(ctx, w, this_val, "readystatechange");
    FireXhrEvent(ctx, w, this_val, "error");
    return JS_UNDEFINED;
  }
  w->status = response.value().status;
  w->status_text = response.value().status_text;
  w->response_text = response.value().body;
  w->response_url = response.value().final_url.empty() ? w->url : response.value().final_url;
  w->response_headers = response.value().headers;
  FireXhrEvent(ctx, w, this_val, "readystatechange");
  FireXhrEvent(ctx, w, this_val, "load");
  return JS_UNDEFINED;
}

JSValue XhrAbort(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  auto* w = XhrOf(ctx, this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an XMLHttpRequest");
  }
  // Nothing is ever in flight under the synchronous transport; reset the
  // object and report abort when it had been opened.
  if (w->ready_state != 0) {
    w->ready_state = 0;
    w->status = 0;
    w->status_text.clear();
    w->response_text.clear();
    w->response_headers.clear();
    FireXhrEvent(ctx, w, this_val, "readystatechange");
    FireXhrEvent(ctx, w, this_val, "abort");
  }
  return JS_UNDEFINED;
}

JSValue XhrGetResponseHeader(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* w = XhrOf(ctx, this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an XMLHttpRequest");
  }
  if (argc < 1) {
    return JS_NULL;
  }
  bool ok = false;
  const std::string name = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    return JS_EXCEPTION;
  }
  for (const auto& header : w->response_headers) {
    if (ToLower(header.first) == name) {
      return JS_NewStringLen(ctx, header.second.data(), header.second.size());
    }
  }
  return JS_NULL;
}

JSValue XhrGetAllResponseHeaders(JSContext* ctx,
                                 JSValueConst this_val,
                                 int /*argc*/,
                                 JSValueConst* /*argv*/)
{
  auto* w = XhrOf(ctx, this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an XMLHttpRequest");
  }
  std::string out;
  for (const auto& header : w->response_headers) {
    out += header.first + ": " + header.second + "\r\n";
  }
  return JS_NewStringLen(ctx, out.data(), out.size());
}

JSValue XhrGetReadyState(JSContext* ctx, JSValueConst this_val)
{
  auto* w = XhrOf(ctx, this_val);
  return JS_NewInt32(ctx, w != nullptr ? w->ready_state : 0);
}

JSValue XhrGetStatus(JSContext* ctx, JSValueConst this_val)
{
  auto* w = XhrOf(ctx, this_val);
  return JS_NewInt32(ctx, w != nullptr ? w->status : 0);
}

JSValue XhrGetStatusText(JSContext* ctx, JSValueConst this_val)
{
  auto* w = XhrOf(ctx, this_val);
  return JS_NewString(ctx, w != nullptr ? w->status_text.c_str() : "");
}

JSValue XhrGetResponseText(JSContext* ctx, JSValueConst this_val)
{
  auto* w = XhrOf(ctx, this_val);
  return JS_NewStringLen(
      ctx, w != nullptr ? w->response_text.data() : "", w != nullptr ? w->response_text.size() : 0);
}

JSValue XhrGetResponseUrl(JSContext* ctx, JSValueConst this_val)
{
  auto* w = XhrOf(ctx, this_val);
  return JS_NewString(ctx, w != nullptr ? w->response_url.c_str() : "");
}

JSValue XhrGetWithCredentials(JSContext* ctx, JSValueConst this_val)
{
  (void)XhrOf(ctx, this_val);
  return JS_FALSE; // accepted-but-inert (documented)
}

// Shared accessor factory for the four on* properties.
JSValue XhrGetHandler(JSContext* ctx, JSValueConst this_val, int magic)
{
  auto* w = XhrOf(ctx, this_val);
  if (w == nullptr) {
    return JS_UNDEFINED;
  }
  JSValue slot = magic == 0   ? w->on_ready_state_change
                 : magic == 1 ? w->on_load
                 : magic == 2 ? w->on_error
                              : w->on_abort;
  return JS_DupValue(ctx, slot);
}

JSValue XhrSetHandler(JSContext* ctx, JSValueConst this_val, JSValueConst value, int magic)
{
  auto* w = XhrOf(ctx, this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an XMLHttpRequest");
  }
  JSValue* slot = magic == 0   ? &w->on_ready_state_change
                  : magic == 1 ? &w->on_load
                  : magic == 2 ? &w->on_error
                               : &w->on_abort;
  JS_FreeValue(ctx, *slot);
  *slot = JS_DupValue(ctx, value);
  return JS_UNDEFINED;
}

JSValue XhrAddEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* w = XhrOf(ctx, this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an XMLHttpRequest");
  }
  if (argc < 2 || !JS_IsFunction(ctx, argv[1])) {
    return JS_ThrowTypeError(ctx, "addEventListener requires (type, callback)");
  }
  bool ok = false;
  const std::string type = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  w->listeners.emplace_back(type, JS_DupValue(ctx, argv[1]));
  return JS_UNDEFINED;
}

JSValue XhrRemoveEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* w = XhrOf(ctx, this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an XMLHttpRequest");
  }
  if (argc < 2) {
    return JS_UNDEFINED;
  }
  bool ok = false;
  const std::string type = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  for (auto it = w->listeners.begin(); it != w->listeners.end(); ++it) {
    if (it->first == type && JS_IsStrictEqual(ctx, it->second, argv[1])) {
      JS_FreeValue(ctx, it->second);
      w->listeners.erase(it);
      break;
    }
  }
  return JS_UNDEFINED;
}

} // namespace

void EnsureXhrClassRegistered(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_xhr_class_mutex);
  JS_NewClassID(rt, &g_xhr_class_id);
  if (g_xhr_class_registered.find(rt) == g_xhr_class_registered.end()) {
    JSClassDef def;
    std::memset(&def, 0, sizeof(def));
    def.class_name = "XMLHttpRequest";
    def.finalizer = &XhrFinalizer;
    def.gc_mark = &XhrGcMark;
    JS_NewClass(rt, g_xhr_class_id, &def);
    g_xhr_class_registered.insert(rt);
  }
}

XhrWrapper* UnwrapXhr(JSValueConst value)
{
  return static_cast<XhrWrapper*>(JS_GetOpaque(value, g_xhr_class_id));
}

void InstallXhrGlobal(JSContext* ctx, Impl& impl)
{
  JSValue global = JS_GetGlobalObject(ctx);
  // C constructors do not wire new.target's .prototype automatically, so the
  // instance prototype is created here, decorated, kept in Impl (freed with
  // the other prototypes) and used by XhrConstructor via
  // JS_NewObjectProtoClass; the constructor's .prototype points at it so
  // `xhr instanceof XMLHttpRequest` works.
  impl.xhr_proto = JS_NewObject(ctx);
  JSValue proto = JS_DupValue(ctx, impl.xhr_proto);
  JSValue ctor =
      JS_NewCFunction2(ctx, XhrConstructor, "XMLHttpRequest", 0, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(ctx, ctor, "prototype", JS_DupValue(ctx, proto));   // steals dup
  JS_SetPropertyStr(ctx, proto, "constructor", JS_DupValue(ctx, ctor)); // steals dup
  JS_SetPropertyStr(ctx, global, "XMLHttpRequest", ctor);               // steals
  static const std::array<JSCFunctionListEntry, 6> kMethods = {{
      JS_CFUNC_DEF("open", 3, XhrOpen),
      JS_CFUNC_DEF("setRequestHeader", 2, XhrSetRequestHeader),
      JS_CFUNC_DEF("send", 1, XhrSend),
      JS_CFUNC_DEF("abort", 0, XhrAbort),
      JS_CFUNC_DEF("getResponseHeader", 1, XhrGetResponseHeader),
      JS_CFUNC_DEF("getAllResponseHeaders", 0, XhrGetAllResponseHeaders),
  }};
  JS_SetPropertyFunctionList(ctx, proto, kMethods.data(), static_cast<int>(kMethods.size()));
  static const std::array<JSCFunctionListEntry, 2> kListenerMethods = {{
      JS_CFUNC_DEF("addEventListener", 2, XhrAddEventListener),
      JS_CFUNC_DEF("removeEventListener", 2, XhrRemoveEventListener),
  }};
  JS_SetPropertyFunctionList(
      ctx, proto, kListenerMethods.data(), static_cast<int>(kListenerMethods.size()));

  DefineGetter(ctx, proto, "readyState", MakeGetter(ctx, "readyState", XhrGetReadyState));
  DefineGetter(ctx, proto, "status", MakeGetter(ctx, "status", XhrGetStatus));
  DefineGetter(ctx, proto, "statusText", MakeGetter(ctx, "statusText", XhrGetStatusText));
  DefineGetter(ctx, proto, "responseText", MakeGetter(ctx, "responseText", XhrGetResponseText));
  DefineGetter(ctx, proto, "response", MakeGetter(ctx, "response", XhrGetResponseText));
  DefineGetter(ctx, proto, "responseURL", MakeGetter(ctx, "responseURL", XhrGetResponseUrl));
  DefineGetter(
      ctx, proto, "withCredentials", MakeGetter(ctx, "withCredentials", XhrGetWithCredentials));

  struct HandlerSlot
  {
    const char* name;
    int magic;
  };
  static constexpr std::array<HandlerSlot, 4> kHandlers = {{
      {"onreadystatechange", 0},
      {"onload", 1},
      {"onerror", 2},
      {"onabort", 3},
  }};
  for (const HandlerSlot& slot : kHandlers) {
    DefineAccessor(ctx,
                   proto,
                   slot.name,
                   MakeGetterMagic(ctx, slot.name, XhrGetHandler, slot.magic),
                   MakeSetterMagic(ctx, slot.name, XhrSetHandler, slot.magic));
  }

  JS_FreeValue(ctx, proto);
  JS_FreeValue(ctx, global);
}

} // namespace neko::javascript
