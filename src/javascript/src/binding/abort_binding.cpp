// neko::javascript DOM bindings — AbortController / AbortSignal.
//
// DOM Standard §4.1 subset: new AbortController(), controller.signal,
// controller.abort(reason) (idempotent, fires a synchronous "abort" Event on
// the signal), signal.aborted/reason/throwIfAborted() and
// addEventListener/removeEventListener("abort", ...).  Sites use this to time
// out fetch() calls (acxun.github.io's daily-quote fetch); a missing global
// killed the whole inline script at the first reference.
//
// Documented limitations: AbortSignal.timeout()/AbortSignal.any() statics are
// not implemented; fetch()/XHR do not observe the signal yet (the abort state
// and events are script-visible only).

#include "binding_internal.h"

#include <cstring>
#include <quickjs.h>
#include <string>
#include <vector>

namespace neko::javascript {

JSClassID g_abort_signal_class_id = 0;
std::mutex g_abort_signal_class_mutex;
std::unordered_set<JSRuntime*> g_abort_signal_class_registered;

namespace {

// Opaque state attached to every AbortSignal object.  The reason and the
// "abort" listeners are owned JSValues freed in the finalizer.
struct AbortSignalWrapper
{
  Impl* impl = nullptr;
  bool aborted = false;
  JSValue reason = JS_UNDEFINED;
  std::vector<JSValue> abort_listeners;
};

void AbortSignalFinalizer(JSRuntime* rt, JSValue obj)
{
  auto* w = static_cast<AbortSignalWrapper*>(JS_GetOpaque(obj, g_abort_signal_class_id));
  if (w == nullptr) {
    return;
  }
  JS_FreeValueRT(rt, w->reason);
  for (JSValue listener : w->abort_listeners) {
    JS_FreeValueRT(rt, listener);
  }
  delete w;
}

// The wrapper holds owned JSValues (reason/listeners); the GC cannot see them
// unless gc_mark marks them (same rationale as EventGcMark in
// event_binding.cpp — otherwise JS_FreeRuntime trips its gc assertion).
void AbortSignalGcMark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
  auto* w = static_cast<AbortSignalWrapper*>(JS_GetOpaque(val, g_abort_signal_class_id));
  if (w == nullptr) {
    return;
  }
  JS_MarkValue(rt, w->reason, mark_func);
  for (JSValue listener : w->abort_listeners) {
    JS_MarkValue(rt, listener, mark_func);
  }
}

void EnsureAbortSignalClassRegistered(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_abort_signal_class_mutex);
  JS_NewClassID(rt, &g_abort_signal_class_id);
  if (g_abort_signal_class_registered.contains(rt)) {
    return;
  }
  JSClassDef def;
  std::memset(&def, 0, sizeof(def));
  def.class_name = "AbortSignal";
  def.finalizer = &AbortSignalFinalizer;
  def.gc_mark = &AbortSignalGcMark;
  JS_NewClass(rt, g_abort_signal_class_id, &def);
  g_abort_signal_class_registered.insert(rt);
}

AbortSignalWrapper* UnwrapAbortSignal(JSValueConst value)
{
  return static_cast<AbortSignalWrapper*>(JS_GetOpaque(value, g_abort_signal_class_id));
}

// Reads the signal hidden on an AbortController (or returns null).  The
// property value is owned, so it is released here before returning the
// unwrapped wrapper.
AbortSignalWrapper* SignalOfController(JSContext* ctx, JSValueConst controller)
{
  JSValue signal = JS_GetPropertyStr(ctx, controller, "_nekoAbortSignal");
  AbortSignalWrapper* w = JS_IsUndefined(signal) ? nullptr : UnwrapAbortSignal(signal);
  JS_FreeValue(ctx, signal);
  return w;
}

// The default abort reason (DOM Standard §4.1): an AbortError DOMException.
// The engine represents DOMExceptions as Error objects with a .name (see
// ThrowDomException in impl.cpp), so the reason is the same shape.
JSValue MakeAbortError(JSContext* ctx)
{
  JSValue err = JS_NewError(ctx);
  JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, "AbortError"));
  JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, "signal is aborted without reason"));
  return err;
}

JSValue SignalGetAborted(JSContext* ctx, JSValueConst this_val)
{
  const AbortSignalWrapper* w = UnwrapAbortSignal(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an AbortSignal");
  }
  return JS_NewBool(ctx, w->aborted);
}

JSValue SignalGetReason(JSContext* ctx, JSValueConst this_val)
{
  AbortSignalWrapper* w = UnwrapAbortSignal(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an AbortSignal");
  }
  return JS_DupValue(ctx, w->reason);
}

JSValue SignalAddEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  AbortSignalWrapper* w = UnwrapAbortSignal(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an AbortSignal");
  }
  if (argc < 2 || JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])) {
    return JS_UNDEFINED;
  }
  bool ok = false;
  const std::string type = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (type != "abort") {
    return JS_UNDEFINED; // only the "abort" type is modeled
  }
  const auto duplicate =
      std::find_if(w->abort_listeners.begin(), w->abort_listeners.end(), [&](JSValue listener) {
        return JS_IsStrictEqual(ctx, listener, argv[1]);
      });
  if (duplicate == w->abort_listeners.end()) {
    w->abort_listeners.push_back(JS_DupValue(ctx, argv[1]));
  }
  return JS_UNDEFINED;
}

JSValue
SignalRemoveEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  AbortSignalWrapper* w = UnwrapAbortSignal(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an AbortSignal");
  }
  if (argc < 2) {
    return JS_UNDEFINED;
  }
  bool ok = false;
  const std::string type = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (type != "abort") {
    return JS_UNDEFINED;
  }
  const auto listener =
      std::find_if(w->abort_listeners.begin(), w->abort_listeners.end(), [&](JSValue value) {
        return JS_IsStrictEqual(ctx, value, argv[1]);
      });
  if (listener != w->abort_listeners.end()) {
    JS_FreeValue(ctx, *listener);
    w->abort_listeners.erase(listener);
  }
  return JS_UNDEFINED;
}

JSValue
SignalThrowIfAborted(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  AbortSignalWrapper* w = UnwrapAbortSignal(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an AbortSignal");
  }
  if (!w->aborted) {
    return JS_UNDEFINED;
  }
  if (JS_IsUndefined(w->reason)) {
    JSValue err = MakeAbortError(ctx);
    return JS_Throw(ctx, err);
  }
  return JS_Throw(ctx, JS_DupValue(ctx, w->reason));
}

// new AbortSignal() is an illegal constructor (DOM Standard §4.1): signals are
// created only through AbortController or the (unimplemented) statics.
JSValue AbortSignalConstructor(JSContext* ctx,
                               JSValueConst /*new_target*/,
                               int /*argc*/,
                               JSValueConst* /*argv*/)
{
  return JS_ThrowTypeError(ctx, "Illegal constructor");
}

JSValue AbortControllerConstructor(JSContext* ctx,
                                   JSValueConst /*new_target*/,
                                   int /*argc*/,
                                   JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  JSValue signal = JS_NewObjectProtoClass(ctx, impl->abort_signal_proto, g_abort_signal_class_id);
  if (JS_IsException(signal)) {
    return signal;
  }
  auto* wrapper = new AbortSignalWrapper();
  wrapper->impl = impl;
  JS_SetOpaque(signal, wrapper);

  JSValue controller = JS_NewObjectProto(ctx, impl->abort_controller_proto);
  if (JS_IsException(controller)) {
    JS_FreeValue(ctx, signal);
    return controller;
  }
  JS_DefinePropertyValueStr(ctx, controller, "_nekoAbortSignal", signal, JS_PROP_CONFIGURABLE);
  return controller;
}

JSValue ControllerGetSignal(JSContext* ctx, JSValueConst this_val)
{
  if (SignalOfController(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "not an AbortController");
  }
  JSValue signal = JS_GetPropertyStr(ctx, this_val, "_nekoAbortSignal");
  return signal; // owned; handed to the caller
}

JSValue ControllerAbort(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  AbortSignalWrapper* w = SignalOfController(ctx, this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an AbortController");
  }
  if (w->aborted) {
    return JS_UNDEFINED; // abort() is idempotent (DOM Standard §4.1)
  }
  w->aborted = true;
  JS_FreeValue(ctx, w->reason);
  if (argc >= 1 && !JS_IsUndefined(argv[0])) {
    w->reason = JS_DupValue(ctx, argv[0]);
  } else {
    w->reason = MakeAbortError(ctx);
  }

  // Fire the "abort" listeners synchronously with a real Event so e.type and
  // the Event helpers work.  Listener copies are Dup'd so a listener that
  // removes itself (or others) during dispatch cannot invalidate the loop.
  Impl* impl = w->impl;
  if (impl == nullptr) {
    return JS_UNDEFINED;
  }
  JSValue event = impl->MakeEvent("abort", false, false);
  std::vector<JSValue> listeners;
  listeners.reserve(w->abort_listeners.size());
  for (JSValue listener : w->abort_listeners) {
    listeners.push_back(JS_DupValue(ctx, listener));
  }
  JSValueConst event_arg[1] = {event};
  JSValue result = JS_UNDEFINED;
  for (JSValue listener : listeners) {
    JS_FreeValue(ctx, result);
    result = JS_Call(ctx, listener, this_val, 1, event_arg);
    if (JS_IsException(result)) {
      for (JSValue remaining : listeners) {
        JS_FreeValue(ctx, remaining);
      }
      JS_FreeValue(ctx, event);
      return JS_EXCEPTION;
    }
  }
  for (JSValue listener : listeners) {
    JS_FreeValue(ctx, listener);
  }
  JS_FreeValue(ctx, result);
  JS_FreeValue(ctx, event);
  return JS_UNDEFINED;
}

} // namespace

void InstallAbortGlobals(JSContext* ctx, JSValue global, Impl& impl)
{
  EnsureAbortSignalClassRegistered(JS_GetRuntime(ctx));

  impl.abort_signal_proto = JS_NewObject(ctx);
  static const std::array<JSCFunctionListEntry, 3> kSignalMethods = {{
      JS_CFUNC_DEF("addEventListener", 2, SignalAddEventListener),
      JS_CFUNC_DEF("removeEventListener", 2, SignalRemoveEventListener),
      JS_CFUNC_DEF("throwIfAborted", 0, SignalThrowIfAborted),
  }};
  JS_SetPropertyFunctionList(
      ctx, impl.abort_signal_proto, kSignalMethods.data(), static_cast<int>(kSignalMethods.size()));
  DefineGetter(
      ctx, impl.abort_signal_proto, "aborted", MakeGetter(ctx, "aborted", SignalGetAborted));
  DefineGetter(ctx, impl.abort_signal_proto, "reason", MakeGetter(ctx, "reason", SignalGetReason));

  JSValue signal_ctor =
      JS_NewCFunction2(ctx, AbortSignalConstructor, "AbortSignal", 0, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(ctx, signal_ctor, "prototype", JS_DupValue(ctx, impl.abort_signal_proto));
  JS_SetPropertyStr(ctx, impl.abort_signal_proto, "constructor", JS_DupValue(ctx, signal_ctor));
  JS_SetPropertyStr(ctx, global, "AbortSignal", signal_ctor); // steals

  impl.abort_controller_proto = JS_NewObject(ctx);
  static const std::array<JSCFunctionListEntry, 1> kControllerMethods = {{
      JS_CFUNC_DEF("abort", 0, ControllerAbort),
  }};
  JS_SetPropertyFunctionList(ctx,
                             impl.abort_controller_proto,
                             kControllerMethods.data(),
                             static_cast<int>(kControllerMethods.size()));
  DefineGetter(
      ctx, impl.abort_controller_proto, "signal", MakeGetter(ctx, "signal", ControllerGetSignal));

  JSValue controller_ctor = JS_NewCFunction2(
      ctx, AbortControllerConstructor, "AbortController", 0, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(
      ctx, controller_ctor, "prototype", JS_DupValue(ctx, impl.abort_controller_proto));
  JS_SetPropertyStr(
      ctx, impl.abort_controller_proto, "constructor", JS_DupValue(ctx, controller_ctor));
  JS_SetPropertyStr(ctx, global, "AbortController", controller_ctor); // steals
}

void ForgetAbortRuntime(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_abort_signal_class_mutex);
  g_abort_signal_class_registered.erase(rt);
}

} // namespace neko::javascript
