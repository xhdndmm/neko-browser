// Binding surface extracted from src/javascript/src/binding/websocket_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=proto, LocalProto]
interface WebsocketBindingProto {
  [Getter=WebSocketGetUrl] readonly attribute any url;
  [Getter=WebSocketGetReadyState] readonly attribute any readyState;
  [Getter=WebSocketGetProtocol] readonly attribute any protocol;
};
