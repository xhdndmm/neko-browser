// Binding surface extracted from src/javascript/src/binding/document_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=css_style_sheet_proto]
interface DocumentBindingCssStyleSheetProto {
  [Getter=StyleSheetGetHref] readonly attribute any href;
  [Getter=StyleSheetGetOwnerNode] readonly attribute any ownerNode;
  [Getter=StyleSheetGetCssRules] readonly attribute any cssRules;
};

[Proto=document_proto]
interface DocumentBindingDocumentProto {
  [Getter=DocGetImplementation] readonly attribute any implementation;
  [Getter=DocGetDocumentElement] readonly attribute any documentElement;
  [Getter=DocGetDoctype] readonly attribute any doctype;
  [Getter=DocGetBody] readonly attribute any body;
  [Getter=DocGetActiveElement] readonly attribute any activeElement;
  [Getter=DocGetHead] readonly attribute any head;
  [Getter=DocGetReadyState] readonly attribute any readyState;
  [Getter=DocGetTitle, Setter=DocSetTitle] attribute any title;
  [Getter=DocGetURL] readonly attribute any URL;
  [Getter=DocGetDomain, Setter=DocSetDomain] attribute any domain;
  [Getter=DocGetCookie, Setter=DocSetCookie] attribute any cookie;
  [Getter=DocGetDefaultView] readonly attribute any defaultView;
  [Getter=DocGetBaseURI] readonly attribute any baseURI;
  [Getter=DocGetDocumentURI] readonly attribute any documentURI;
  [Getter=DocGetCharacterSet] readonly attribute any characterSet;
  [Getter=DocGetContentType] readonly attribute any contentType;
  [Getter=DocGetReferrer] readonly attribute any referrer;
  [Getter=DocGetForms] readonly attribute any forms;
  [Getter=DocGetImages] readonly attribute any images;
  [Getter=DocGetLinks] readonly attribute any links;
  [Getter=DocGetScripts] readonly attribute any scripts;
  [Getter=DocGetStyleSheets] readonly attribute any styleSheets;
  [Getter=DocGetCurrentScript] readonly attribute any currentScript;
  [CFunction=DocGetElementById] void getElementById(any a0);
  [CFunction=DocCreateElement] void createElement(any a0);
  [CFunction=DocCreateElementNS] void createElementNS(any a0, any a1);
  [CFunction=DocCreateTextNode] void createTextNode(any a0);
  [CFunction=DocCreateDocumentFragment] void createDocumentFragment();
  [CFunction=DocCreateTreeWalker] void createTreeWalker(any a0);
  [CFunction=DocCreateRange] void createRange();
  [CFunction=DocCreateComment] void createComment(any a0);
  [CFunction=DocCreateEvent] void createEvent(any a0);
  [CFunction=DocGetSelection] void getSelection();
  [CFunction=DocQuerySelector] void querySelector(any a0);
  [CFunction=DocQuerySelectorAll] void querySelectorAll(any a0);
  [CFunction=DocGetElementsByTagName] void getElementsByTagName(any a0);
  [CFunction=DocGetElementsByClassName] void getElementsByClassName(any a0);
  [CFunction=DocWrite] void write(any a0);
  [CFunction=DocWrite] void writeln(any a0);
  [CFunction=DocHasFocus] void hasFocus();
};
