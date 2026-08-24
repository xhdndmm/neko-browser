#include "binding_internal.h"

#include <array>
#include <memory>
#include <utility>

namespace neko::javascript {
namespace {

struct MessagePortEndpoint
{
  Impl* impl = nullptr;
  std::weak_ptr<MessagePortEndpoint> peer;
  JSValue onmessage = JS_NULL;
  std::vector<JSValue> listeners;
  bool started = false;
  bool closed = false;
};

struct MessagePortWrapper
{
  std::shared_ptr<MessagePortEndpoint> endpoint;
};

JSClassID g_message_port_class_id = 0;
std::mutex g_message_port_class_mutex;
std::unordered_set<JSRuntime*> g_message_port_class_registered;

void MessagePortFinalizer(JSRuntime* rt, JSValue object)
{
  auto* wrapper = static_cast<MessagePortWrapper*>(JS_GetOpaque(object, g_message_port_class_id));
  if (wrapper == nullptr)
    return;
  auto endpoint = wrapper->endpoint;
  if (endpoint != nullptr) {
    endpoint->closed = true;
    for (JSValue listener : endpoint->listeners)
      JS_FreeValueRT(rt, listener);
    endpoint->listeners.clear();
    JS_FreeValueRT(rt, endpoint->onmessage);
    endpoint->onmessage = JS_UNDEFINED;
  }
  delete wrapper;
}

void EnsureClassRegistered(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_message_port_class_mutex);
  if (g_message_port_class_id == 0)
    JS_NewClassID(rt, &g_message_port_class_id);
  if (g_message_port_class_registered.contains(rt))
    return;
  JSClassDef class_definition{};
  class_definition.class_name = "MessagePort";
  class_definition.finalizer = MessagePortFinalizer;
  JS_NewClass(rt, g_message_port_class_id, &class_definition);
  g_message_port_class_registered.insert(rt);
}

MessagePortEndpoint* Unwrap(JSValueConst value)
{
  auto* wrapper = static_cast<MessagePortWrapper*>(JS_GetOpaque(value, g_message_port_class_id));
  return wrapper != nullptr ? wrapper->endpoint.get() : nullptr;
}

JSValue NewPort(JSContext* ctx, Impl& impl, const std::shared_ptr<MessagePortEndpoint>& endpoint)
{
  JSValue object = JS_NewObjectProtoClass(ctx, impl.message_port_proto, g_message_port_class_id);
  if (JS_IsException(object))
    return object;
  auto* wrapper = new MessagePortWrapper{endpoint};
  JS_SetOpaque(object, wrapper);
  impl.message_port_endpoints.push_back(endpoint);
  return object;
}

JSValue
PortConstructor(JSContext* ctx, JSValueConst /*new_target*/, int /*argc*/, JSValueConst* /*argv*/)
{
  return JS_ThrowTypeError(ctx, "Illegal constructor");
}

JSValue
ChannelConstructor(JSContext* ctx, JSValueConst new_target, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, new_target);
  if (impl == nullptr)
    return JS_ThrowInternalError(ctx, "MessageChannel is unavailable");

  auto first = std::make_shared<MessagePortEndpoint>();
  auto second = std::make_shared<MessagePortEndpoint>();
  first->impl = impl;
  second->impl = impl;
  first->peer = second;
  second->peer = first;

  JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
  JSValue object = JS_NewObjectProto(ctx, prototype);
  JS_FreeValue(ctx, prototype);
  JSValue port1 = NewPort(ctx, *impl, first);
  JSValue port2 = NewPort(ctx, *impl, second);
  if (JS_IsException(port1) || JS_IsException(port2)) {
    JS_FreeValue(ctx, port1);
    JS_FreeValue(ctx, port2);
    JS_FreeValue(ctx, object);
    return JS_EXCEPTION;
  }
  JS_SetPropertyStr(ctx, object, "port1", port1);
  JS_SetPropertyStr(ctx, object, "port2", port2);
  return object;
}

JSValue PostMessage(JSContext* ctx, JSValueConst this_value, int argc, JSValueConst* argv)
{
  MessagePortEndpoint* endpoint = Unwrap(this_value);
  if (endpoint == nullptr)
    return JS_ThrowTypeError(ctx, "invalid MessagePort receiver");
  auto peer = endpoint->peer.lock();
  if (endpoint->closed || peer == nullptr || peer->closed)
    return JS_UNDEFINED;

  size_t size = 0;
  const JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
  uint8_t* bytes = JS_WriteObject(ctx, &size, value, JS_WRITE_OBJ_REFERENCE);
  if (bytes == nullptr) {
    JS_GetException(ctx);
    return ThrowDomException(ctx, "DataCloneError", "The message could not be cloned");
  }
  Impl::MessagePortTask task;
  task.endpoint = peer;
  task.serialized_data.assign(bytes, bytes + size);
  js_free(ctx, bytes);
  endpoint->impl->message_port_tasks.push_back(std::move(task));
  return JS_UNDEFINED;
}

JSValue Start(JSContext* ctx, JSValueConst this_value, int /*argc*/, JSValueConst* /*argv*/)
{
  MessagePortEndpoint* endpoint = Unwrap(this_value);
  if (endpoint == nullptr)
    return JS_ThrowTypeError(ctx, "invalid MessagePort receiver");
  if (!endpoint->closed)
    endpoint->started = true;
  return JS_UNDEFINED;
}

JSValue Close(JSContext* ctx, JSValueConst this_value, int /*argc*/, JSValueConst* /*argv*/)
{
  MessagePortEndpoint* endpoint = Unwrap(this_value);
  if (endpoint == nullptr)
    return JS_ThrowTypeError(ctx, "invalid MessagePort receiver");
  endpoint->closed = true;
  if (auto peer = endpoint->peer.lock())
    peer->closed = true;
  return JS_UNDEFINED;
}

JSValue GetOnMessage(JSContext* ctx, JSValueConst this_value)
{
  MessagePortEndpoint* endpoint = Unwrap(this_value);
  if (endpoint == nullptr)
    return JS_ThrowTypeError(ctx, "invalid MessagePort receiver");
  return JS_DupValue(ctx, endpoint->onmessage);
}

JSValue SetOnMessage(JSContext* ctx, JSValueConst this_value, JSValueConst value)
{
  MessagePortEndpoint* endpoint = Unwrap(this_value);
  if (endpoint == nullptr)
    return JS_ThrowTypeError(ctx, "invalid MessagePort receiver");
  JS_FreeValue(ctx, endpoint->onmessage);
  endpoint->onmessage = JS_IsFunction(ctx, value) ? JS_DupValue(ctx, value) : JS_NULL;
  if (JS_IsFunction(ctx, value) && !endpoint->closed)
    endpoint->started = true;
  return JS_UNDEFINED;
}

JSValue AddEventListener(JSContext* ctx, JSValueConst this_value, int argc, JSValueConst* argv)
{
  MessagePortEndpoint* endpoint = Unwrap(this_value);
  if (endpoint == nullptr)
    return JS_ThrowTypeError(ctx, "invalid MessagePort receiver");
  bool ok = false;
  const std::string type = argc > 0 ? ArgString(ctx, argv[0], &ok) : std::string();
  if (ok && type == "message" && argc > 1 && JS_IsFunction(ctx, argv[1])) {
    endpoint->listeners.push_back(JS_DupValue(ctx, argv[1]));
  }
  return JS_UNDEFINED;
}

void InvokeListener(JSContext* ctx, JSValueConst listener, JSValueConst event)
{
  JSValue result = JS_Call(ctx, listener, JS_UNDEFINED, 1, &event);
  if (JS_IsException(result)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
  } else {
    JS_FreeValue(ctx, result);
  }
}

} // namespace

void InstallMessageChannelGlobals(JSContext* ctx, JSValue global, Impl& impl)
{
  EnsureClassRegistered(JS_GetRuntime(ctx));
  impl.message_port_proto = JS_NewObjectProto(ctx, impl.event_target_proto);
  static const std::array<JSCFunctionListEntry, 4> methods = {{
      JS_CFUNC_DEF("postMessage", 1, PostMessage),
      JS_CFUNC_DEF("start", 0, Start),
      JS_CFUNC_DEF("close", 0, Close),
      JS_CFUNC_DEF("addEventListener", 2, AddEventListener),
  }};
  JS_SetPropertyFunctionList(
      ctx, impl.message_port_proto, methods.data(), static_cast<int>(methods.size()));
  DefineAccessor(ctx,
                 impl.message_port_proto,
                 "onmessage",
                 MakeGetter(ctx, "onmessage", GetOnMessage),
                 MakeSetter(ctx, "onmessage", SetOnMessage));

  JSValue port_constructor =
      JS_NewCFunction2(ctx, PortConstructor, "MessagePort", 0, JS_CFUNC_constructor, 0);
  JS_SetConstructor(ctx, port_constructor, impl.message_port_proto);
  JS_SetPropertyStr(
      ctx, impl.message_port_proto, "constructor", JS_DupValue(ctx, port_constructor));
  JS_SetPropertyStr(ctx, global, "MessagePort", port_constructor);

  JSValue channel_prototype = JS_NewObject(ctx);
  JSValue channel_constructor =
      JS_NewCFunction2(ctx, ChannelConstructor, "MessageChannel", 0, JS_CFUNC_constructor, 0);
  JS_SetConstructor(ctx, channel_constructor, channel_prototype);
  JS_SetPropertyStr(ctx, channel_prototype, "constructor", JS_DupValue(ctx, channel_constructor));
  JS_SetPropertyStr(ctx, global, "MessageChannel", channel_constructor);
  JS_FreeValue(ctx, channel_prototype);
}

void ForgetMessageChannelRuntime(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_message_port_class_mutex);
  g_message_port_class_registered.erase(rt);
}

void CloseMessagePorts(Impl& impl)
{
  for (const std::weak_ptr<void>& erased : impl.message_port_endpoints) {
    auto endpoint = std::static_pointer_cast<MessagePortEndpoint>(erased.lock());
    if (endpoint == nullptr)
      continue;
    endpoint->closed = true;
    endpoint->impl = nullptr;
    for (JSValue listener : endpoint->listeners)
      JS_FreeValue(impl.ctx, listener);
    endpoint->listeners.clear();
    JS_FreeValue(impl.ctx, endpoint->onmessage);
    endpoint->onmessage = JS_UNDEFINED;
  }
  impl.message_port_tasks.clear();
  impl.message_port_endpoints.clear();
}

int RunPendingMessagePortTasks(Impl& impl)
{
  std::vector<Impl::MessagePortTask> tasks;
  tasks.swap(impl.message_port_tasks);
  int ran = 0;
  for (Impl::MessagePortTask& task : tasks) {
    auto endpoint = std::static_pointer_cast<MessagePortEndpoint>(task.endpoint.lock());
    if (endpoint == nullptr || endpoint->closed || !endpoint->started) {
      if (endpoint != nullptr && !endpoint->closed) {
        impl.message_port_tasks.push_back(std::move(task));
      }
      continue;
    }
    JSValue data = JS_ReadObject(
        impl.ctx, task.serialized_data.data(), task.serialized_data.size(), JS_READ_OBJ_REFERENCE);
    if (JS_IsException(data)) {
      JS_FreeValue(impl.ctx, JS_GetException(impl.ctx));
      continue;
    }
    JSValue event_init = JS_NewObject(impl.ctx);
    JS_SetPropertyStr(impl.ctx, event_init, "data", JS_DupValue(impl.ctx, data));
    JSValue event_args[2] = {JS_NewString(impl.ctx, "message"), event_init};
    JSValue event = MessageEventConstructor(impl.ctx, JS_UNDEFINED, 2, event_args);
    JS_FreeValue(impl.ctx, event_args[0]);
    JS_FreeValue(impl.ctx, event_init);
    if (JS_IsException(event)) {
      JS_FreeValue(impl.ctx, JS_GetException(impl.ctx));
      JS_FreeValue(impl.ctx, data);
      continue;
    }
    if (JS_IsFunction(impl.ctx, endpoint->onmessage)) {
      InvokeListener(impl.ctx, endpoint->onmessage, event);
    }
    for (JSValue listener : endpoint->listeners) {
      InvokeListener(impl.ctx, listener, event);
    }
    JS_FreeValue(impl.ctx, event);
    JS_FreeValue(impl.ctx, data);
    impl.engine.RunPendingJobs();
    ++ran;
  }
  return ran;
}

} // namespace neko::javascript
