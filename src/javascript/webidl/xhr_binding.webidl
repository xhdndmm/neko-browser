// Binding surface extracted from src/javascript/src/binding/xhr_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=proto, LocalProto]
interface XhrBindingProto {
  [Getter=XhrGetReadyState] readonly attribute any readyState;
  [Getter=XhrGetStatus] readonly attribute any status;
  [Getter=XhrGetStatusText] readonly attribute any statusText;
  [Getter=XhrGetResponseText] readonly attribute any responseText;
  [Getter=XhrGetResponseText] readonly attribute any response;
  [Getter=XhrGetResponseUrl] readonly attribute any responseURL;
  [Getter=XhrGetResponseType, Setter=XhrSetResponseType] attribute any responseType;
  [Getter=XhrGetWithCredentials, Setter=XhrSetWithCredentials] attribute any withCredentials;
  [Getter=XhrGetTimeout, Setter=XhrSetTimeout] attribute any timeout;
  [CFunction=XhrOpen] void open(any a0, any a1, any a2);
  [CFunction=XhrSetRequestHeader] void setRequestHeader(any a0, any a1);
  [CFunction=XhrSend] void send(any a0);
  [CFunction=XhrAbort] void abort();
  [CFunction=XhrGetResponseHeader] void getResponseHeader(any a0);
  [CFunction=XhrGetAllResponseHeaders] void getAllResponseHeaders();
  [CFunction=XhrAddEventListener] void addEventListener(any a0, any a1);
  [CFunction=XhrRemoveEventListener] void removeEventListener(any a0, any a1);
};
