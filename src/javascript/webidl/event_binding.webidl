// Binding surface extracted from src/javascript/src/binding/event_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=event_proto]
interface EventBindingEventProto {
  [Getter=EventGetType] readonly attribute any type;
  [Getter=EventGetKey] readonly attribute any key;
  [Getter=EventGetCode] readonly attribute any code;
  [Getter=EventGetKeyCode] readonly attribute any keyCode;
  [Getter=EventGetClientX] readonly attribute any clientX;
  [Getter=EventGetClientY] readonly attribute any clientY;
  [Getter=EventGetButton] readonly attribute any button;
  [Getter=EventGetDeltaY] readonly attribute any deltaY;
  [Getter=EventGetTarget] readonly attribute any target;
  [Getter=EventGetCurrentTarget] readonly attribute any currentTarget;
  [Getter=EventGetBubbles] readonly attribute any bubbles;
  [Getter=EventGetCancelable] readonly attribute any cancelable;
  [Getter=EventGetDefaultPrevented] readonly attribute any defaultPrevented;
  [Getter=EventGetEventPhase] readonly attribute any eventPhase;
  [Getter=EventGetTimeStamp] readonly attribute any timeStamp;
  [Getter=EventGetIsTrusted] readonly attribute any isTrusted;
  [CFunction=EventPreventDefault] void preventDefault();
  [CFunction=EventStopPropagation] void stopPropagation();
  [CFunction=EventStopImmediatePropagation] void stopImmediatePropagation();
  [CFunction=EventComposedPath] void composedPath();
  [CFunction=EventInitEvent] void initEvent(any a0, any a1, any a2);
  [CFunction=EventInitCustomEvent] void initCustomEvent(any a0, any a1, any a2, any a3);
  [CFunction=EventInitMouseEvent] void initMouseEvent(any a0, any a1, any a2, any a3, any a4, any a5, any a6, any a7, any a8, any a9, any a10, any a11, any a12, any a13, any a14);
};

[Proto=message_event_proto]
interface EventBindingMessageEventProto {
  [Getter=MessageEventGetData] readonly attribute any data;
  [Getter=MessageEventGetOrigin] readonly attribute any origin;
  [Getter=MessageEventGetLastEventId] readonly attribute any lastEventId;
  [Getter=MessageEventGetSource] readonly attribute any source;
  [Getter=MessageEventGetPorts] readonly attribute any ports;
};
