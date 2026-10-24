// Binding surface extracted from src/javascript/src/binding/abort_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=abort_signal_proto]
interface AbortBindingAbortSignalProto {
  [Getter=SignalGetAborted] readonly attribute any aborted;
  [Getter=SignalGetReason] readonly attribute any reason;
  [CFunction=SignalAddEventListener] void addEventListener(any a0, any a1);
  [CFunction=SignalRemoveEventListener] void removeEventListener(any a0, any a1);
  [CFunction=SignalThrowIfAborted] void throwIfAborted();
};

[Proto=abort_controller_proto]
interface AbortBindingAbortControllerProto {
  [Getter=ControllerGetSignal] readonly attribute any signal;
  [CFunction=ControllerAbort] void abort();
};
