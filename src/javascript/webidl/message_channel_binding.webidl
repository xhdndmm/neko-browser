// Binding surface extracted from src/javascript/src/binding/message_channel_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=message_port_proto]
interface MessageChannelBindingMessagePortProto {
  [Getter=GetOnMessage, Setter=SetOnMessage] attribute any onmessage;
  [CFunction=PostMessage] void postMessage(any a0);
  [CFunction=Start] void start();
  [CFunction=Close] void close();
  [CFunction=AddEventListener] void addEventListener(any a0, any a1);
};
