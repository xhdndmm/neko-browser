// neko::javascript DOM bindings — Event family.
//
// Part of the dom_binding split (see binding/binding_internal.h): the Event
// wrapper class and its opaque state helpers, the Event/CustomEvent
// constructors and accessors, MutationObserver, the element global event
// handler attributes (onclick/oninput/...), and DefineEventPrototype.

#include "binding_internal.h"

#include <array>
#include <cstring>
#include <quickjs.h>
#include <string>

namespace neko::javascript {

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

JSValue MutationObserverDisconnect(JSContext* ctx,
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
    JS_SetPropertyStr(
        ctx,
        item,
        "attributeName",
        record.attribute_name.empty() ? JS_NULL : JS_NewString(ctx, record.attribute_name.c_str()));
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
  JS_SetPropertyStr(
      ctx, observer, "observe", JS_NewCFunction(ctx, MutationObserverObserve, "observe", 2));
  JS_SetPropertyStr(ctx,
                    observer,
                    "disconnect",
                    JS_NewCFunction(ctx, MutationObserverDisconnect, "disconnect", 0));
  JS_SetPropertyStr(ctx,
                    observer,
                    "takeRecords",
                    JS_NewCFunction(ctx, MutationObserverTakeRecords, "takeRecords", 0));
  Impl::MutationObserver state;
  state.callback = JS_DupValue(ctx, argv[0]);
  state.self = JS_DupValue(ctx, observer);
  impl->mutation_observers.push_back(std::move(state));
  return observer;
}

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

void DefineElementEventHandlers(JSContext* ctx, Impl& impl)
{
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
}

void EnsureEventClassRegistered(JSRuntime* rt)
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

} // namespace neko::javascript
