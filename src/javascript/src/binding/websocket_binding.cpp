// neko::javascript DOM bindings — WebSocket (RFC 6455).
//
// Threading model (important):
//   QuickJS is not thread-safe, so the socket's I/O thread NEVER touches a
//   JSContext.  It only appends events to a mutex-protected queue.  The page's
//   event pump (Impl::RunPendingTimers -> PumpWebSocketEvents) drains the queue
//   on the runtime's own thread and dispatches the JS handlers there.
//
//    JS thread (main)                     receive thread
//    ----------------                     --------------
//    new WebSocket()  ---- state ---->    connect()
//    send(data)       -- outgoing -->     write frame
//    close()          -- flag ------>     send close, exit
//    pump: drain events <-- queue --      read frame -> enqueue event
//
//   State is shared_ptr-owned by the receive thread and by the registry entry,
//   so a garbage-collected socket object can never dangle the I/O thread, and
//   a detached thread can never touch a freed JSContext.
//
// Implemented surface: constructor(url[, protocols]), url/protocol/readyState,
// onopen/onmessage/onerror/onclose, addEventListener/removeEventListener for
// the same types, send(string|ArrayBuffer|TypedArray), close(code, reason).
//
// Documented approximations:
//   * The event objects are plain Event objects with the extra fields set
//     (data / code / reason / wasClean), not full MessageEvent/CloseEvent
//     subclasses.
//   * Binary frames are delivered as strings; there is no Blob/ArrayBuffer
//     message transformation yet.
//   * No automatic reconnect, no ping keep-alive scheduling.
//   * Events are delivered by the page pump (the same clock as timers), so
//     delivery latency is one pump tick, not a dedicated task queue.

#include "neko/base/logging.h"
#include "neko/network/websocket.h"

#include "binding_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <quickjs.h>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neko::javascript {
namespace {

constexpr int kConnecting = 0;
constexpr int kOpen = 1;
constexpr int kClosing = 2;
constexpr int kClosed = 3;

// How long the receive thread blocks in Receive() before re-checking its
// control flags.  Bounds how quickly close() takes effect.
constexpr int kReceivePollMs = 100;

struct PendingEvent
{
  enum class Kind
  {
    Open,
    Message,
    Error,
    Close,
  };
  Kind kind = Kind::Message;
  std::string data;   // Message
  uint16_t code = 0;  // Close
  std::string reason; // Close
  bool was_clean = false;
};

struct Outgoing
{
  bool binary = false;
  std::string data;
};

// Shared between the JS-thread wrapper and the detached receive thread.
// Contains no JSValue; every member is guarded by |mutex|.
struct WebSocketState
{
  std::string url;
  std::vector<std::string> protocols;

  std::mutex mutex;
  std::deque<PendingEvent> events;
  std::deque<Outgoing> outgoing;
  bool close_requested = false;
  bool shutdown = false; // binder teardown: stop without dispatching
  bool done = false;     // receive thread finished

  std::atomic<bool> thread_done{false};
};

struct WebSocketWrapper
{
  Impl* impl = nullptr;
  std::string url;
  std::weak_ptr<WebSocketState> state;
};

JSClassID g_websocket_class_id = 0;
std::mutex g_websocket_class_mutex;
std::unordered_set<JSRuntime*> g_websocket_class_registered;

// Per-Impl registry of live sockets.  The entry holds a strong reference to
// the socket object while the connection is active (mirroring the browser
// behavior of keeping an object with pending network activity alive); the
// pump drops the entry once the exchange has closed and its events drained.
struct RegistryEntry
{
  std::shared_ptr<WebSocketState> state;
  JSValue self = JS_UNDEFINED; // owned
};

std::mutex g_registry_mutex;
std::unordered_map<Impl*, std::vector<RegistryEntry>> g_registry;

// The hidden own-property that stores addEventListener registrations.  Kept on
// the instance (not in C++) so all JSValue handling stays on the JS thread.
constexpr const char* kListenersKey = "__nekoWsListeners__";

// Call fn(event) for every listener of |type|: the on* handler property plus
// the addEventListener entries.  Both live on the instance, so this runs only
// on the JS thread.
void InvokeHandlers(Impl& impl, JSValueConst self, const char* type, JSValueConst event)
{
  const std::string handler_name = std::string("on") + type;
  JSValue handler = JS_GetPropertyStr(impl.ctx, self, handler_name.c_str());
  if (JS_IsFunction(impl.ctx, handler)) {
    JSValue ret = JS_Call(impl.ctx, handler, self, 1, &event);
    if (JS_IsException(ret)) {
      JSValue exception = JS_GetException(impl.ctx);
      JS_FreeValue(impl.ctx, exception);
    }
    JS_FreeValue(impl.ctx, ret);
  }
  JS_FreeValue(impl.ctx, handler);

  JSValue listeners = JS_GetPropertyStr(impl.ctx, self, kListenersKey);
  if (!JS_IsArray(listeners)) {
    JS_FreeValue(impl.ctx, listeners);
    return;
  }
  JSValue length_value = JS_GetPropertyStr(impl.ctx, listeners, "length");
  int32_t length = 0;
  JS_ToInt32(impl.ctx, &length, length_value);
  JS_FreeValue(impl.ctx, length_value);
  for (int32_t i = 0; i + 1 < length; i += 2) {
    JSValue entry_type = JS_GetPropertyUint32(impl.ctx, listeners, static_cast<uint32_t>(i));
    const char* entry_type_str = JS_ToCString(impl.ctx, entry_type);
    const bool matches = entry_type_str != nullptr && std::string_view(entry_type_str) == type;
    if (entry_type_str != nullptr) {
      JS_FreeCString(impl.ctx, entry_type_str);
    }
    JS_FreeValue(impl.ctx, entry_type);
    if (!matches) {
      continue;
    }
    JSValue fn = JS_GetPropertyUint32(impl.ctx, listeners, static_cast<uint32_t>(i + 1));
    if (JS_IsFunction(impl.ctx, fn)) {
      JSValue ret = JS_Call(impl.ctx, fn, self, 1, &event);
      if (JS_IsException(ret)) {
        JSValue exception = JS_GetException(impl.ctx);
        JS_FreeValue(impl.ctx, exception);
      }
      JS_FreeValue(impl.ctx, ret);
    }
    JS_FreeValue(impl.ctx, fn);
  }
  JS_FreeValue(impl.ctx, listeners);
}

// Sets an extra field on a dispatched event.  Uses DefineOwnProperty
// semantics: Event.prototype already defines getter-only accessors for fields
// like "code" (KeyboardEvent), and a plain [[Set]] would silently fail
// against a getter without a setter.  Defining an own data property shadows
// the prototype accessor, which is what a CloseEvent's code needs.
void SetEventField(Impl& impl, JSValueConst event, const char* name, JSValueConst value)
{
  JS_DefinePropertyValueStr(impl.ctx, event, name, value, JS_PROP_C_W_E);
}

// Dispatches one queued event to the socket object (JS thread only).
void DispatchPendingEvent(Impl& impl, JSValueConst self, const PendingEvent& pending)
{
  switch (pending.kind) {
  case PendingEvent::Kind::Open: {
    JSValue event = impl.MakeEvent("open", false, false);
    InvokeHandlers(impl, self, "open", event);
    JS_FreeValue(impl.ctx, event);
    break;
  }
  case PendingEvent::Kind::Message: {
    JSValue event = impl.MakeEvent("message", false, false);
    SetEventField(
        impl, event, "data", JS_NewStringLen(impl.ctx, pending.data.data(), pending.data.size()));
    InvokeHandlers(impl, self, "message", event);
    JS_FreeValue(impl.ctx, event);
    break;
  }
  case PendingEvent::Kind::Error: {
    JSValue event = impl.MakeEvent("error", false, false);
    InvokeHandlers(impl, self, "error", event);
    JS_FreeValue(impl.ctx, event);
    break;
  }
  case PendingEvent::Kind::Close: {
    JSValue event = impl.MakeEvent("close", false, false);
    SetEventField(impl, event, "code", JS_NewInt32(impl.ctx, pending.code));
    SetEventField(impl,
                  event,
                  "reason",
                  JS_NewStringLen(impl.ctx, pending.reason.data(), pending.reason.size()));
    SetEventField(impl, event, "wasClean", JS_NewBool(impl.ctx, pending.was_clean));
    InvokeHandlers(impl, self, "close", event);
    JS_FreeValue(impl.ctx, event);
    break;
  }
  }
}

// Reads an int property (default when absent).
int ReadIntProperty(Impl& impl, JSValueConst obj, const char* name, int fallback)
{
  JSValue value = JS_GetPropertyStr(impl.ctx, obj, name);
  int32_t result = 0;
  const int rc = JS_ToInt32(impl.ctx, &result, value);
  JS_FreeValue(impl.ctx, value);
  return rc == 0 ? static_cast<int>(result) : fallback;
}

void WriteIntProperty(Impl& impl, JSValueConst obj, const char* name, int value)
{
  JS_SetPropertyStr(impl.ctx, obj, name, JS_NewInt32(impl.ctx, value));
}

// --- receive thread --------------------------------------------------------

// Appends |event| to the state queue (thread-safe).
void Enqueue(const std::shared_ptr<WebSocketState>& state, PendingEvent event)
{
  std::lock_guard lock(state->mutex);
  state->events.push_back(std::move(event));
}

// Drains queued outgoing messages to the wire.
bool FlushOutgoing(network::WebSocket& socket, const std::shared_ptr<WebSocketState>& state)
{
  std::deque<Outgoing> batch;
  {
    std::lock_guard lock(state->mutex);
    batch.swap(state->outgoing);
  }
  for (const Outgoing& message : batch) {
    const auto sent =
        message.binary ? socket.SendBinary(message.data) : socket.SendText(message.data);
    if (!sent.has_value()) {
      return false;
    }
  }
  return true;
}

void ReceiveLoop(const std::shared_ptr<WebSocketState>& state)
{
  network::TlsOptions tls_options;
  tls_options.timeout_ms = 30000;

  auto connected =
      network::WebSocket::Connect(url::Url::Parse(state->url).value(), "", state->protocols);
  if (!connected.has_value()) {
    Enqueue(state, PendingEvent{.kind = PendingEvent::Kind::Error});
    PendingEvent closed;
    closed.kind = PendingEvent::Kind::Close;
    closed.code = 1006; // abnormal closure
    closed.was_clean = false;
    Enqueue(state, std::move(closed));
    std::lock_guard lock(state->mutex);
    state->done = true;
    state->thread_done.store(true);
    return;
  }

  auto socket = std::move(connected.value());
  Enqueue(state, PendingEvent{.kind = PendingEvent::Kind::Open});

  while (true) {
    {
      std::lock_guard lock(state->mutex);
      if (state->shutdown) {
        break;
      }
      if (state->close_requested) {
        break;
      }
    }

    if (!FlushOutgoing(socket, state)) {
      PendingEvent event;
      event.kind = PendingEvent::Kind::Close;
      event.code = 1006;
      event.was_clean = false;
      Enqueue(state, std::move(event));
      break;
    }

    auto message = socket.Receive(kReceivePollMs);
    if (!message.has_value()) {
      if (message.error().category() == base::ErrorCategory::kIo) {
        // Deadline: re-check control flags and keep waiting.
        std::lock_guard lock(state->mutex);
        if (state->close_requested || state->shutdown) {
          break;
        }
        continue;
      }
      // Peer closed (with or without a close frame) or the I/O failed.
      PendingEvent closed;
      closed.kind = PendingEvent::Kind::Close;
      closed.code = 1006; // abnormal closure (RFC 6455 §7.1.5)
      closed.was_clean = false;
      Enqueue(state, std::move(closed));
      break;
    }

    PendingEvent event;
    event.kind = PendingEvent::Kind::Message;
    event.data = std::move(message.value().data);
    if (event.data.empty() && message.value().type == network::WebSocketMessageType::Text &&
        !socket.IsOpen()) {
      // The peer closed cleanly.
      PendingEvent closed;
      closed.kind = PendingEvent::Kind::Close;
      closed.code = 1000;
      closed.was_clean = true;
      Enqueue(state, std::move(closed));
      break;
    }
    Enqueue(state, std::move(event));
  }

  // Graceful close when the page asked for it.
  bool close_requested = false;
  {
    std::lock_guard lock(state->mutex);
    close_requested = state->close_requested && !state->shutdown;
  }
  if (close_requested) {
    (void)socket.Close(network::WebSocketCloseCode::Normal);
    PendingEvent closed;
    closed.kind = PendingEvent::Kind::Close;
    closed.code = 1000;
    closed.was_clean = true;
    Enqueue(state, std::move(closed));
  }

  {
    std::lock_guard lock(state->mutex);
    state->done = true;
  }
  state->thread_done.store(true);
}

// --- class plumbing --------------------------------------------------------

void WebSocketFinalizer(JSRuntime* /*rt*/, JSValue val)
{
  auto* wrapper = static_cast<WebSocketWrapper*>(JS_GetOpaque(val, g_websocket_class_id));
  if (wrapper == nullptr) {
    return;
  }
  if (auto state = wrapper->state.lock()) {
    std::lock_guard lock(state->mutex);
    state->shutdown = true;
  }
  delete wrapper;
}

void EnsureWebSocketClassRegistered(JSRuntime* rt)
{
  std::lock_guard lock(g_websocket_class_mutex);
  if (g_websocket_class_registered.count(rt) != 0) {
    return;
  }
  if (g_websocket_class_id == 0) {
    JS_NewClassID(rt, &g_websocket_class_id);
  }
  JSClassDef def{};
  def.class_name = "WebSocket";
  def.finalizer = WebSocketFinalizer;
  JS_NewClass(rt, g_websocket_class_id, &def);
  g_websocket_class_registered.insert(rt);
}

WebSocketWrapper* UnwrapWebSocket(JSValueConst value)
{
  return static_cast<WebSocketWrapper*>(JS_GetOpaque(value, g_websocket_class_id));
}

Impl* ImplForCtx(JSContext* ctx)
{
  // The codebase keeps a ctx -> Impl registry (see ImplFor in impl.cpp);
  // JS_GetContextOpaque is not populated, so always go through that helper.
  return ImplFor(ctx, JS_UNDEFINED);
}

// --- prototype methods -----------------------------------------------------

JSValue WebSocketGetUrl(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  auto* wrapper = UnwrapWebSocket(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not a WebSocket");
  }
  return JS_NewString(ctx, wrapper->url.c_str());
}

JSValue
WebSocketGetReadyState(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  auto* wrapper = UnwrapWebSocket(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not a WebSocket");
  }
  return JS_NewInt32(ctx, ReadIntProperty(*wrapper->impl, this_val, "__nekoReadyState", kClosed));
}

JSValue
WebSocketGetProtocol(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  auto* wrapper = UnwrapWebSocket(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not a WebSocket");
  }
  if (auto state = wrapper->state.lock()) {
    std::lock_guard lock(state->mutex);
    // The negotiated subprotocol is not exposed by WebSocketState today; the
    // requested subprotocol is the honest answer until it is.
  }
  return JS_NewString(ctx, "");
}

JSValue WebSocketSend(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = UnwrapWebSocket(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not a WebSocket");
  }
  Impl& impl = *wrapper->impl;
  const int ready_state = ReadIntProperty(impl, this_val, "__nekoReadyState", kClosed);
  if (ready_state != kOpen) {
    return ThrowDomException(ctx, "InvalidStateError", "WebSocket is not open");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "send() requires 1 argument");
  }

  Outgoing outgoing;
  // Binary inputs are sent as binary frames; everything else converts to
  // string (HTML serializes non-BufferSource values via ToString).
  if (JS_IsArrayBuffer(argv[0])) {
    std::size_t size = 0;
    std::uint8_t* data = JS_GetArrayBuffer(ctx, &size, argv[0]);
    if (data == nullptr) {
      return JS_EXCEPTION;
    }
    outgoing.binary = true;
    outgoing.data.assign(reinterpret_cast<char*>(data), size);
  } else {
    // Typed arrays (Uint8Array etc.) expose their backing buffer; anything
    // without one (including DataView, until it gets explicit support) falls
    // back to string conversion.
    std::size_t byte_offset = 0;
    std::size_t byte_length = 0;
    std::size_t bytes_per_element = 0;
    JSValue array_buffer =
        JS_GetTypedArrayBuffer(ctx, argv[0], &byte_offset, &byte_length, &bytes_per_element);
    if (!JS_IsException(array_buffer) && !JS_IsUndefined(array_buffer) &&
        !JS_IsNull(array_buffer)) {
      std::size_t size = 0;
      std::uint8_t* data = JS_GetArrayBuffer(ctx, &size, array_buffer);
      if (data != nullptr && byte_offset + byte_length <= size) {
        outgoing.binary = true;
        outgoing.data.assign(reinterpret_cast<char*>(data) + byte_offset, byte_length);
      }
      JS_FreeValue(ctx, array_buffer);
    } else {
      JS_FreeValue(ctx, array_buffer);
      // Not a typed array: clear the probe's exception and fall through to
      // string conversion (a plain `send({})` must not throw here).
      JSValue pending = JS_GetException(ctx);
      JS_FreeValue(ctx, pending);
    }
    if (!outgoing.binary) {
      const char* text = JS_ToCString(ctx, argv[0]);
      if (text == nullptr) {
        return JS_EXCEPTION;
      }
      outgoing.data = text;
      JS_FreeCString(ctx, text);
    }
  }

  if (outgoing.data.size() > 16u * 1024u * 1024u) {
    return ThrowDomException(ctx, "SyntaxError", "message exceeds the 16 MiB limit");
  }

  auto state = wrapper->state.lock();
  if (state == nullptr) {
    return ThrowDomException(ctx, "InvalidStateError", "WebSocket connection is gone");
  }
  {
    std::lock_guard lock(state->mutex);
    if (state->done || state->close_requested) {
      return ThrowDomException(ctx, "InvalidStateError", "WebSocket is closing");
    }
    state->outgoing.push_back(std::move(outgoing));
  }
  return JS_UNDEFINED;
}

JSValue WebSocketClose(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = UnwrapWebSocket(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not a WebSocket");
  }
  Impl& impl = *wrapper->impl;
  const int ready_state = ReadIntProperty(impl, this_val, "__nekoReadyState", kClosed);
  if (ready_state == kClosing || ready_state == kClosed) {
    return JS_UNDEFINED;
  }
  int code = 1000;
  if (argc >= 1) {
    int32_t parsed = 0;
    if (JS_ToInt32(ctx, &parsed, argv[0]) == 0) {
      code = static_cast<int>(parsed);
    }
  }
  // Per WHATWG: a code outside [1000, 4999) or 1004/1005/1006/1015 throws.
  if (argc >= 1 && (code < 1000 || code >= 5000 || code == 1004 || code == 1005 || code == 1006 ||
                    code == 1015)) {
    return ThrowDomException(ctx, "InvalidAccessError", "invalid close code");
  }
  auto state = wrapper->state.lock();
  if (state != nullptr) {
    std::lock_guard lock(state->mutex);
    state->close_requested = true;
  }
  WriteIntProperty(impl, this_val, "__nekoReadyState", kClosing);
  return JS_UNDEFINED;
}

JSValue
WebSocketAddEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = UnwrapWebSocket(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not a WebSocket");
  }
  if (argc < 2 || !JS_IsFunction(ctx, argv[1])) {
    return JS_UNDEFINED;
  }
  const char* type = JS_ToCString(ctx, argv[0]);
  if (type == nullptr) {
    return JS_EXCEPTION;
  }
  JSValue listeners = JS_GetPropertyStr(ctx, this_val, kListenersKey);
  if (!JS_IsArray(listeners)) {
    JS_FreeValue(ctx, listeners);
    listeners = JS_NewArray(ctx);
    JS_SetPropertyStr(ctx, this_val, kListenersKey, JS_DupValue(ctx, listeners));
  }
  JSValue length_value = JS_GetPropertyStr(ctx, listeners, "length");
  uint32_t length = 0;
  JS_ToUint32(ctx, &length, length_value);
  JS_FreeValue(ctx, length_value);
  JS_SetPropertyUint32(ctx, listeners, length, JS_NewString(ctx, type));
  JS_SetPropertyUint32(ctx, listeners, length + 1, JS_DupValue(ctx, argv[1]));
  JS_FreeCString(ctx, type);
  JS_FreeValue(ctx, listeners);
  return JS_UNDEFINED;
}

JSValue
WebSocketRemoveEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = UnwrapWebSocket(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not a WebSocket");
  }
  if (argc < 2) {
    return JS_UNDEFINED;
  }
  const char* type = JS_ToCString(ctx, argv[0]);
  if (type == nullptr) {
    return JS_EXCEPTION;
  }
  JSValue listeners = JS_GetPropertyStr(ctx, this_val, kListenersKey);
  if (JS_IsArray(listeners)) {
    JSValue kept = JS_NewArray(ctx);
    uint32_t keep_count = 0;
    JSValue length_value = JS_GetPropertyStr(ctx, listeners, "length");
    uint32_t length = 0;
    JS_ToUint32(ctx, &length, length_value);
    JS_FreeValue(ctx, length_value);
    for (uint32_t i = 0; i + 1 < length; i += 2) {
      JSValue entry_type = JS_GetPropertyUint32(ctx, listeners, i);
      JSValue entry_fn = JS_GetPropertyUint32(ctx, listeners, i + 1);
      const char* entry_type_str = JS_ToCString(ctx, entry_type);
      const bool same_type = entry_type_str != nullptr && std::string_view(entry_type_str) == type;
      if (entry_type_str != nullptr) {
        JS_FreeCString(ctx, entry_type_str);
      }
      const bool same_fn = JS_IsStrictEqual(ctx, entry_fn, argv[1]);
      if (same_type && same_fn) {
        JS_FreeValue(ctx, entry_type);
        JS_FreeValue(ctx, entry_fn);
        continue;
      }
      JS_SetPropertyUint32(ctx, kept, keep_count++, JS_DupValue(ctx, entry_type));
      JS_SetPropertyUint32(ctx, kept, keep_count++, JS_DupValue(ctx, entry_fn));
      JS_FreeValue(ctx, entry_type);
      JS_FreeValue(ctx, entry_fn);
    }
    JS_SetPropertyStr(ctx, this_val, kListenersKey, JS_DupValue(ctx, kept));
    JS_FreeValue(ctx, kept);
  }
  JS_FreeValue(ctx, listeners);
  JS_FreeCString(ctx, type);
  return JS_UNDEFINED;
}

JSValue WebSocketConstructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "WebSocket() requires at least 1 argument");
  }
  const char* url_text = JS_ToCString(ctx, argv[0]);
  if (url_text == nullptr) {
    return JS_EXCEPTION;
  }
  std::string url(url_text);
  JS_FreeCString(ctx, url_text);

  std::vector<std::string> protocols;
  if (argc >= 2) {
    if (JS_IsString(argv[1])) {
      const char* protocol = JS_ToCString(ctx, argv[1]);
      if (protocol != nullptr) {
        protocols.emplace_back(protocol);
        JS_FreeCString(ctx, protocol);
      }
    } else if (JS_IsArray(argv[1]) > 0) {
      JSValue length_value = JS_GetPropertyStr(ctx, argv[1], "length");
      uint32_t length = 0;
      JS_ToUint32(ctx, &length, length_value);
      JS_FreeValue(ctx, length_value);
      for (uint32_t i = 0; i < length; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, argv[1], i);
        const char* protocol = JS_ToCString(ctx, item);
        if (protocol != nullptr) {
          protocols.emplace_back(protocol);
          JS_FreeCString(ctx, protocol);
        }
        JS_FreeValue(ctx, item);
      }
    }
  }

  Impl* impl = ImplForCtx(ctx);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "WebSocket is not available on this page");
  }

  JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
  if (JS_IsException(proto)) {
    return proto;
  }
  JSValue self = JS_NewObjectProtoClass(ctx, proto, g_websocket_class_id);
  JS_FreeValue(ctx, proto);
  if (JS_IsException(self)) {
    return self;
  }

  auto* wrapper = new WebSocketWrapper;
  wrapper->impl = impl;
  wrapper->url = url;
  JS_SetOpaque(self, wrapper);

  auto state = std::make_shared<WebSocketState>();
  state->url = url;
  state->protocols = protocols;
  wrapper->state = state;

  WriteIntProperty(*impl, self, "__nekoReadyState", kConnecting);
  JS_SetPropertyStr(ctx, self, "url", JS_NewString(ctx, url.c_str()));
  JS_SetPropertyStr(ctx, self, "binaryType", JS_NewString(ctx, "blob"));

  // Register first so the pump delivers the first events, then start I/O.
  {
    std::lock_guard lock(g_registry_mutex);
    g_registry[impl].push_back(RegistryEntry{state, JS_DupValue(ctx, self)});
  }

  // Validate the URL before spawning: an unparsable URL fails asynchronously
  // (error + close) exactly like an unreachable host.
  const auto parsed = url::Url::Parse(url);
  const bool valid =
      parsed.has_value() && (parsed.value().scheme() == "ws" || parsed.value().scheme() == "wss");
  if (!valid) {
    Enqueue(state, PendingEvent{.kind = PendingEvent::Kind::Error});
    PendingEvent closed;
    closed.kind = PendingEvent::Kind::Close;
    closed.code = 1006;
    Enqueue(state, std::move(closed));
    std::lock_guard lock(state->mutex);
    state->done = true;
  } else {
    std::thread(ReceiveLoop, state).detach();
  }

  return self;
}

void DefineWebSocketPrototype(JSContext* ctx, Impl& impl)
{
  JSValue proto = JS_NewObject(ctx);
  // Accessors (not data properties): `ws.readyState` must evaluate the
  // getter, and the codebase's MakeGetter/DefineGetter pair wires that
  // through QuickJS's JS_DefinePropertyGetSet.
  DefineGetter(ctx, proto, "url", MakeGetter(ctx, "url", WebSocketGetUrl));
  DefineGetter(ctx, proto, "readyState", MakeGetter(ctx, "readyState", WebSocketGetReadyState));
  DefineGetter(ctx, proto, "protocol", MakeGetter(ctx, "protocol", WebSocketGetProtocol));
  JS_SetPropertyStr(ctx, proto, "send", JS_NewCFunction(ctx, WebSocketSend, "send", 1));
  JS_SetPropertyStr(ctx, proto, "close", JS_NewCFunction(ctx, WebSocketClose, "close", 2));
  JS_SetPropertyStr(ctx,
                    proto,
                    "addEventListener",
                    JS_NewCFunction(ctx, WebSocketAddEventListener, "addEventListener", 2));
  JS_SetPropertyStr(ctx,
                    proto,
                    "removeEventListener",
                    JS_NewCFunction(ctx, WebSocketRemoveEventListener, "removeEventListener", 2));

  JSValue global = JS_GetGlobalObject(ctx);
  JSValue ctor =
      JS_NewCFunction2(ctx, WebSocketConstructor, "WebSocket", 1, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(ctx, ctor, "CONNECTING", JS_NewInt32(ctx, kConnecting));
  JS_SetPropertyStr(ctx, ctor, "OPEN", JS_NewInt32(ctx, kOpen));
  JS_SetPropertyStr(ctx, ctor, "CLOSING", JS_NewInt32(ctx, kClosing));
  JS_SetPropertyStr(ctx, ctor, "CLOSED", JS_NewInt32(ctx, kClosed));
  JS_SetConstructor(ctx, ctor, proto);
  JS_SetPropertyStr(ctx, global, "WebSocket", ctor);
  JS_FreeValue(ctx, global);
  JS_FreeValue(ctx, proto);
  (void)impl;
}

} // namespace

void InstallWebSocketGlobal(JSContext* ctx, Impl& impl)
{
  EnsureWebSocketClassRegistered(JS_GetRuntime(ctx));
  DefineWebSocketPrototype(ctx, impl);
}

// Delivers queued socket events to their JS handlers.  Runs on the runtime's
// thread as part of Impl::RunPendingTimers.
int PumpWebSocketEvents(Impl& impl)
{
  std::vector<RegistryEntry> entries;
  {
    std::lock_guard lock(g_registry_mutex);
    const auto it = g_registry.find(&impl);
    if (it == g_registry.end()) {
      return 0;
    }
    entries.swap(it->second);
  }

  int dispatched = 0;
  std::vector<RegistryEntry> keep;
  for (RegistryEntry& entry : entries) {
    std::deque<PendingEvent> events;
    bool done = false;
    bool shutdown = false;
    std::size_t pending_outgoing = 0;
    {
      std::lock_guard lock(entry.state->mutex);
      events.swap(entry.state->events);
      done = entry.state->done;
      shutdown = entry.state->shutdown;
      pending_outgoing = entry.state->outgoing.size();
    }
    if (shutdown) {
      JS_FreeValue(impl.ctx, entry.self);
      continue; // drop: the binder is tearing the socket down
    }

    for (const PendingEvent& pending : events) {
      if (done || pending.kind == PendingEvent::Kind::Close) {
        WriteIntProperty(impl, entry.self, "__nekoReadyState", kClosed);
      } else if (pending.kind == PendingEvent::Kind::Open) {
        WriteIntProperty(impl, entry.self, "__nekoReadyState", kOpen);
      }
      DispatchPendingEvent(impl, entry.self, pending);
      ++dispatched;
      impl.engine.RunPendingJobs();
    }

    // Reap sockets whose exchange finished and whose queue is drained.
    bool finished = false;
    {
      std::lock_guard lock(entry.state->mutex);
      finished = entry.state->done && entry.state->events.empty() &&
                 entry.state->outgoing.empty() && !entry.state->close_requested;
    }
    if (finished || (done && events.empty() && pending_outgoing == 0)) {
      // Give handlers one last look at the closed socket, then release it.
      JS_FreeValue(impl.ctx, entry.self);
      continue;
    }
    keep.push_back(std::move(entry));
  }

  if (!keep.empty()) {
    std::lock_guard lock(g_registry_mutex);
    std::vector<RegistryEntry>& slot = g_registry[&impl];
    for (RegistryEntry& entry : keep) {
      slot.push_back(std::move(entry));
    }
  }
  return dispatched;
}

// Stops every socket registered for |impl| and waits briefly for the I/O
// threads to exit, so no detached thread touches a destroyed binder.
void ShutdownWebSockets(Impl& impl)
{
  std::vector<RegistryEntry> entries;
  {
    std::lock_guard lock(g_registry_mutex);
    const auto it = g_registry.find(&impl);
    if (it == g_registry.end()) {
      return;
    }
    entries.swap(it->second);
    g_registry.erase(it);
  }
  for (RegistryEntry& entry : entries) {
    {
      std::lock_guard lock(entry.state->mutex);
      entry.state->shutdown = true;
    }
    JS_FreeValue(impl.ctx, entry.self);
  }
  // The threads poll their control flags every kReceivePollMs; wait a bounded
  // multiple of that, then let the shared_ptr keep the state alive.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  for (RegistryEntry& entry : entries) {
    while (!entry.state->thread_done.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

} // namespace neko::javascript
