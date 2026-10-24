// Binding surface extracted from src/javascript/src/binding/event_target_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=event_target_proto]
interface EventTargetBindingEventTargetProto {
  [CFunction=EventTargetAddEventListener] void addEventListener(any a0, any a1);
  [CFunction=EventTargetRemoveEventListener] void removeEventListener(any a0, any a1);
  [CFunction=EventTargetDispatchEvent] void dispatchEvent(any a0);
};
