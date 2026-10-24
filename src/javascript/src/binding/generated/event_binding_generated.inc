// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/event_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr JSCFunctionListEntry kEventBindingEventProtoMethods[] = {
    JS_CFUNC_DEF("preventDefault", 0, EventPreventDefault),
    JS_CFUNC_DEF("stopPropagation", 0, EventStopPropagation),
    JS_CFUNC_DEF("stopImmediatePropagation", 0, EventStopImmediatePropagation),
    JS_CFUNC_DEF("composedPath", 0, EventComposedPath),
    JS_CFUNC_DEF("initEvent", 3, EventInitEvent),
    JS_CFUNC_DEF("initCustomEvent", 4, EventInitCustomEvent),
    JS_CFUNC_DEF("initMouseEvent", 15, EventInitMouseEvent),
};

} // namespace

void DefineGeneratedEventBindingEventProto(JSContext* ctx, Impl& impl)
{
  JS_SetPropertyFunctionList(ctx,
                             impl.event_proto,
                             kEventBindingEventProtoMethods,
                             static_cast<int>(std::size(kEventBindingEventProtoMethods)));
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

void DefineGeneratedEventBindingMessageEventProto(JSContext* ctx, Impl& impl)
{
  DefineGetter(ctx, impl.message_event_proto, "data", MakeGetter(ctx, "data", MessageEventGetData));
  DefineGetter(
      ctx, impl.message_event_proto, "origin", MakeGetter(ctx, "origin", MessageEventGetOrigin));
  DefineGetter(ctx,
               impl.message_event_proto,
               "lastEventId",
               MakeGetter(ctx, "lastEventId", MessageEventGetLastEventId));
  DefineGetter(
      ctx, impl.message_event_proto, "source", MakeGetter(ctx, "source", MessageEventGetSource));
  DefineGetter(
      ctx, impl.message_event_proto, "ports", MakeGetter(ctx, "ports", MessageEventGetPorts));
}
