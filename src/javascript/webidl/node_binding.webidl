// Binding surface extracted from src/javascript/src/binding/node_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=node_proto]
interface NodeBindingNodeProto {
  [Getter=NodeGetNodeType] readonly attribute any nodeType;
  [Getter=NodeGetNodeName] readonly attribute any nodeName;
  [Getter=NodeGetBaseURI] readonly attribute any baseURI;
  [Getter=NodeGetTextContent, Setter=NodeSetTextContent] attribute any textContent;
  [Getter=NodeGetParentNode] readonly attribute any parentNode;
  [Getter=NodeGetParentElement] readonly attribute any parentElement;
  [Getter=NodeGetFirstChild] readonly attribute any firstChild;
  [Getter=NodeGetLastChild] readonly attribute any lastChild;
  [Getter=NodeGetChildNodes] readonly attribute any childNodes;
  [Getter=NodeGetNextSibling] readonly attribute any nextSibling;
  [Getter=NodeGetPreviousSibling] readonly attribute any previousSibling;
  [Getter=NodeGetOwnerDocument] readonly attribute any ownerDocument;
  [Getter=NodeGetIsConnected] readonly attribute any isConnected;
  [Getter=NodeGetNodeValue, Setter=NodeSetNodeValue] attribute any nodeValue;
  [CFunction=NodeAppendChild] void appendChild(any a0);
  [CFunction=NodeAppend] void append();
  [CFunction=NodePrepend] void prepend();
  [CFunction=NodeReplaceChildren] void replaceChildren();
  [CFunction=NodeBefore] void before();
  [CFunction=NodeAfter] void after();
  [CFunction=NodeReplaceWith] void replaceWith();
  [CFunction=NodeInsertBefore] void insertBefore(any a0, any a1);
  [CFunction=NodeReplaceChild] void replaceChild(any a0, any a1);
  [CFunction=NodeRemoveChild] void removeChild(any a0);
  [CFunction=NodeHasChildNodes] void hasChildNodes();
  [CFunction=NodeCloneNode] void cloneNode(any a0);
  [CFunction=NodeContains] void contains(any a0);
  [CFunction=NodeNormalize] void normalize();
  [CFunction=NodeAddEventListener] void addEventListener(any a0, any a1);
  [CFunction=NodeRemoveEventListener] void removeEventListener(any a0, any a1);
  [CFunction=NodeDispatchEvent] void dispatchEvent(any a0);
};
