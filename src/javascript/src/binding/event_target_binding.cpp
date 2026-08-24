#include "binding_internal.h"

#include <algorithm>
#include <mutex>
#include <quickjs.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neko::javascript {
namespace {

struct EventTargetWrapper
{
  std::unordered_map<std::string, std::vector<JSValue>> listeners;
};

JSClassID g_event_target_class_id = 0;
std::mutex g_event_target_class_mutex;
std::unordered_set<JSRuntime*> g_event_target_class_registered;

void EventTargetFinalizer(JSRuntime* rt, JSValue value)
{
  auto* wrapper =
      static_cast<EventTargetWrapper*>(JS_GetOpaque(value, g_event_target_class_id));
  if (wrapper == nullptr) {
    return;
  }
  for (auto& [type, listeners] : wrapper->listeners) {
    (void)type;
    for (JSValue listener : listeners) {
      JS_FreeValueRT(rt, listener);
    }
  }
  delete wrapper;
}

void EventTargetGcMark(JSRuntime* rt, JSValueConst value, JS_MarkFunc* mark_func)
{
  auto* wrapper =
      static_cast<EventTargetWrapper*>(JS_GetOpaque(value, g_event_target_class_id));
  if (wrapper == nullptr) {
    return;
  }
  for (const auto& [type, listeners] : wrapper->listeners) {
    (void)type;
    for (JSValue listener : listeners) {
      JS_MarkValue(rt, listener, mark_func);
    }
  }
}

void EnsureEventTargetClassRegistered(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_event_target_class_mutex);
  if (g_event_target_class_id == 0) {
    JS_NewClassID(rt, &g_event_target_class_id);
  }
  if (g_event_target_class_registered.contains(rt)) {
    return;
  }
  JSClassDef definition{};
  definition.class_name = "EventTarget";
  definition.finalizer = EventTargetFinalizer;
  definition.gc_mark = EventTargetGcMark;
  JS_NewClass(rt, g_event_target_class_id, &definition);
  g_event_target_class_registered.insert(rt);
}

EventTargetWrapper* UnwrapEventTarget(JSValueConst value)
{
  return static_cast<EventTargetWrapper*>(JS_GetOpaque(value, g_event_target_class_id));
}

JSValue EventTargetConstructor(JSContext* ctx,
                               JSValueConst /*new_target*/,
                               int /*argc*/,
                               JSValueConst* /*argv*/)
{
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue constructor = JS_GetPropertyStr(ctx, global, "EventTarget");
  JSValue prototype = JS_GetPropertyStr(ctx, constructor, "prototype");
  JSValue object = JS_NewObjectProtoClass(ctx, prototype, g_event_target_class_id);
  JS_FreeValue(ctx, prototype);
  JS_FreeValue(ctx, constructor);
  JS_FreeValue(ctx, global);
  if (JS_IsException(object)) {
    return object;
  }
  JS_SetOpaque(object, new EventTargetWrapper());
  return object;
}

JSValue EventTargetAddEventListener(JSContext* ctx,
                                    JSValueConst this_val,
                                    int argc,
                                    JSValueConst* argv)
{
  EventTargetWrapper* wrapper = UnwrapEventTarget(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not an EventTarget");
  }
  if (argc < 2 || JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])) {
    return JS_UNDEFINED;
  }
  bool ok = false;
  const std::string type = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  auto& listeners = wrapper->listeners[type];
  const auto duplicate = std::find_if(listeners.begin(), listeners.end(), [&](JSValue listener) {
    return JS_IsStrictEqual(ctx, listener, argv[1]);
  });
  if (duplicate == listeners.end()) {
    listeners.push_back(JS_DupValue(ctx, argv[1]));
  }
  return JS_UNDEFINED;
}

JSValue EventTargetRemoveEventListener(JSContext* ctx,
                                       JSValueConst this_val,
                                       int argc,
                                       JSValueConst* argv)
{
  EventTargetWrapper* wrapper = UnwrapEventTarget(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not an EventTarget");
  }
  if (argc < 2) {
    return JS_UNDEFINED;
  }
  bool ok = false;
  const std::string type = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  auto it = wrapper->listeners.find(type);
  if (it == wrapper->listeners.end()) {
    return JS_UNDEFINED;
  }
  auto& listeners = it->second;
  const auto listener = std::find_if(listeners.begin(), listeners.end(), [&](JSValue value) {
    return JS_IsStrictEqual(ctx, value, argv[1]);
  });
  if (listener != listeners.end()) {
    JS_FreeValue(ctx, *listener);
    listeners.erase(listener);
  }
  return JS_UNDEFINED;
}

JSValue EventTargetDispatchEvent(JSContext* ctx,
                                 JSValueConst this_val,
                                 int argc,
                                 JSValueConst* argv)
{
  EventTargetWrapper* wrapper = UnwrapEventTarget(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not an EventTarget");
  }
  EventWrapper* event = argc > 0 ? UnwrapEvent(argv[0]) : nullptr;
  if (event == nullptr) {
    return JS_ThrowTypeError(ctx, "dispatchEvent requires an Event");
  }
  if (event->event_phase != 0) {
    return JS_ThrowTypeError(ctx, "event is already being dispatched");
  }

  JS_FreeValue(ctx, event->target);
  event->target = JS_DupValue(ctx, this_val);
  JS_FreeValue(ctx, event->current_target);
  event->current_target = JS_DupValue(ctx, this_val);
  event->event_phase = 2;
  event->propagation_stopped = false;
  event->immediate_stopped = false;

  const auto found = wrapper->listeners.find(event->type);
  if (found != wrapper->listeners.end()) {
    std::vector<JSValue> listeners;
    listeners.reserve(found->second.size());
    for (JSValue listener : found->second) {
      listeners.push_back(JS_DupValue(ctx, listener));
    }
    for (JSValue listener : listeners) {
      if (!event->immediate_stopped) {
        JSValue result = JS_Call(ctx, listener, this_val, 1, argv);
        if (JS_IsException(result)) {
          JS_FreeValue(ctx, result);
          for (JSValue remaining : listeners) {
            JS_FreeValue(ctx, remaining);
          }
          event->event_phase = 0;
          JS_FreeValue(ctx, event->current_target);
          event->current_target = JS_UNDEFINED;
          return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, result);
      }
    }
    for (JSValue listener : listeners) {
      JS_FreeValue(ctx, listener);
    }
  }

  event->event_phase = 0;
  JS_FreeValue(ctx, event->current_target);
  event->current_target = JS_UNDEFINED;
  return JS_NewBool(ctx, !event->default_prevented);
}

} // namespace

void InstallEventTargetGlobal(JSContext* ctx, JSValue global, Impl& impl)
{
  EnsureEventTargetClassRegistered(JS_GetRuntime(ctx));
  impl.event_target_proto = JS_NewObject(ctx);
  static const JSCFunctionListEntry kMethods[] = {
      JS_CFUNC_DEF("addEventListener", 2, EventTargetAddEventListener),
      JS_CFUNC_DEF("removeEventListener", 2, EventTargetRemoveEventListener),
      JS_CFUNC_DEF("dispatchEvent", 1, EventTargetDispatchEvent),
  };
  JS_SetPropertyFunctionList(ctx, impl.event_target_proto, kMethods, 3);
  JSValue constructor = JS_NewCFunction2(
      ctx, EventTargetConstructor, "EventTarget", 0, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(
      ctx, constructor, "prototype", JS_DupValue(ctx, impl.event_target_proto));
  JS_SetPropertyStr(ctx, impl.event_target_proto, "constructor", JS_DupValue(ctx, constructor));
  JS_SetPropertyStr(ctx, global, "EventTarget", constructor);
}

void ForgetEventTargetRuntime(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_event_target_class_mutex);
  g_event_target_class_registered.erase(rt);
}

} // namespace neko::javascript