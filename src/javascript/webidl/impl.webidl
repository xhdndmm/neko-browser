// Binding surface extracted from src/javascript/src/binding/impl.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=node_list_proto]
interface ImplNodeListProto {
  [Getter=NodeListLength] readonly attribute any length;
};

[Proto=html_collection_proto]
interface ImplHtmlCollectionProto {
  [Getter=NodeListLength] readonly attribute any length;
};

[Proto=window]
interface ImplWindow {
  [Getter=WindowGetClosed] readonly attribute any closed;
  [Getter=WindowViewportGetter, Magic=0] readonly attribute any innerWidth;
  [Getter=WindowViewportGetter, Magic=1] readonly attribute any innerHeight;
  [Getter=WindowViewportGetter, Magic=0] readonly attribute any outerWidth;
  [Getter=WindowViewportGetter, Magic=2] readonly attribute any outerHeight;
  [Getter=WindowViewportGetter, Magic=3] readonly attribute any devicePixelRatio;
  [Getter=WindowScrollOffsetGetter, Magic=0] readonly attribute any pageXOffset;
  [Getter=WindowScrollOffsetGetter, Magic=1] readonly attribute any pageYOffset;
  [Getter=WindowScrollOffsetGetter, Magic=2] readonly attribute any scrollX;
  [Getter=WindowScrollOffsetGetter, Magic=3] readonly attribute any scrollY;
  [CFunction=WindowAddEventListener] void addEventListener(any a0, any a1);
  [CFunction=WindowRemoveEventListener] void removeEventListener(any a0, any a1);
  [CFunction=WindowDispatchEvent] void dispatchEvent(any a0);
  [CFunction=WindowRequestAnimationFrame] void requestAnimationFrame(any a0);
  [CFunction=WindowCancelAnimationFrame] void cancelAnimationFrame(any a0);
  [CFunction=WindowScrollTo] void scrollTo();
  [CFunction=WindowScrollTo] void scroll();
  [CFunction=WindowScrollBy] void scrollBy();
  [CFunction=WindowGetComputedStyle] void getComputedStyle(any a0);
  [CFunction=WindowRequestAnimationFrame] void requestIdleCallback(any a0);
  [CFunction=WindowCancelAnimationFrame] void cancelIdleCallback(any a0);
  [CFunction=WindowMatchMedia] void matchMedia(any a0);
};

[Proto=character_data_proto]
interface ImplCharacterDataProto {
  [Getter=CharacterDataGetData, Setter=CharacterDataSetData] attribute any data;
};

[Proto=fragment_proto]
interface ImplFragmentProto {
  [Getter=ElementGetChildren] readonly attribute any children;
};

[Proto=document_type_proto]
interface ImplDocumentTypeProto {
  [Getter=DocumentTypeGetName] readonly attribute any name;
  [Getter=DocumentTypeGetPublicId] readonly attribute any publicId;
  [Getter=DocumentTypeGetSystemId] readonly attribute any systemId;
};

[Proto=html_script_element_proto]
interface ImplHtmlScriptElementProto {
  [Getter=ElementGetSrc, Setter=ElementSetSrc] attribute any src;
};

[Proto=html_base_element_proto]
interface ImplHtmlBaseElementProto {
  [Getter=ElementGetHref, Setter=ElementSetHref] attribute any href;
};

[Proto=html_anchor_element_proto]
interface ImplHtmlAnchorElementProto {
  [Getter=ElementGetHref, Setter=ElementSetHref] attribute any href;
  [Getter=ElementGetDownload, Setter=ElementSetDownload] attribute any download;
  [Getter=ElementGetPing, Setter=ElementSetPing] attribute any ping;
};

[Proto=html_iframe_element_proto]
interface ImplHtmlIframeElementProto {
  [Getter=ElementGetSrc, Setter=ElementSetSrc] attribute any src;
  [Getter=ElementGetSrcDoc, Setter=ElementSetSrcDoc] attribute any srcdoc;
  [Getter=ElementGetCredentialless, Setter=ElementSetCredentialless] attribute any credentialless;
};

[Proto=html_form_element_proto]
interface ImplHtmlFormElementProto {
  [Getter=FormGetAction, Setter=FormSetAction] attribute any action;
  [Getter=FormGetEnctype, Setter=FormSetEnctype] attribute any enctype;
  [Getter=FormGetMethod, Setter=FormSetMethod] attribute any method;
  [Getter=FormGetNoValidate, Setter=FormSetNoValidate] attribute any noValidate;
};

[Proto=html_button_element_proto]
interface ImplHtmlButtonElementProto {
  [Getter=ElementGetFormAction, Setter=ElementSetFormAction] attribute any formAction;
};

[Proto=html_input_element_proto]
interface ImplHtmlInputElementProto {
  [Getter=ElementGetFormAction, Setter=ElementSetFormAction] attribute any formAction;
  [Getter=ElementGetValue, Setter=ElementSetValue] attribute any value;
  [Getter=ElementGetDefaultValue, Setter=ElementSetDefaultValue] attribute any defaultValue;
  [Getter=ElementGetMultiple, Setter=ElementSetMultiple] attribute any multiple;
  [Getter=ElementGetMin, Setter=ElementSetMin] attribute any min;
  [Getter=ElementGetMax, Setter=ElementSetMax] attribute any max;
  [Getter=ElementGetStep, Setter=ElementSetStep] attribute any step;
};

[Proto=html_image_element_proto]
interface ImplHtmlImageElementProto {
  [Getter=ElementGetSrc, Setter=ElementSetSrc] attribute any src;
  [Getter=ElementGetSrcSet, Setter=ElementSetSrcSet] attribute any srcset;
};

[Proto=html_media_element_proto]
interface ImplHtmlMediaElementProto {
  [Getter=ElementGetSrc, Setter=ElementSetSrc] attribute any src;
  [Getter=ElementGetCurrentSrc] readonly attribute any currentSrc;
};

[Proto=attr_proto]
interface ImplAttrProto {
  [Getter=AttrGetValue, Setter=AttrSetValue] attribute any value;
  [Getter=AttrGetNodeValue, Setter=AttrSetNodeValue] attribute any nodeValue;
  [Getter=AttrGetOwnerElement] readonly attribute any ownerElement;
};

[Proto=navigator_proto, LocalProto]
interface ImplNavigatorProto {
  [Getter=NavigatorGetLanguages] readonly attribute any languages;
};

[Proto=history_proto, LocalProto]
interface ImplHistoryProto {
  [CFunction=HistoryBack] void back();
  [CFunction=HistoryForward] void forward();
  [CFunction=HistoryGo] void go(any a0);
  [CFunction=HistoryPushState] void pushState(any a0, any a1, any a2);
  [CFunction=HistoryReplaceState] void replaceState(any a0, any a1, any a2);
};

[Proto=performance_proto, LocalProto]
interface ImplPerformanceProto {
  [CFunction=PerformanceNow] void now();
  [CFunction=PerformanceGetEntries] void getEntries();
  [CFunction=PerformanceGetEntriesByType] void getEntriesByType(any a0);
  [CFunction=PerformanceSetResourceTimingBufferSize] void setResourceTimingBufferSize(any a0);
  [CFunction=PerformanceClearResourceTimings] void clearResourceTimings();
};

[Proto=named_node_map_proto]
interface ImplNamedNodeMapProto {
  [CFunction=NamedNodeMapItem] void item(any a0);
  [CFunction=NamedNodeMapGetNamedItem] void getNamedItem(any a0);
};

[Proto=tree_walker_proto]
interface ImplTreeWalkerProto {
  [CFunction=TreeWalkerNextNode] void nextNode();
};

[Proto=mutation_observer_proto]
interface ImplMutationObserverProto {
  [CFunction=MutationObserverObserve] void observe(any a0, any a1);
  [CFunction=MutationObserverDisconnect] void disconnect();
  [CFunction=MutationObserverTakeRecords] void takeRecords();
};

[Proto=dom_implementation_proto]
interface ImplDomImplementationProto {
  [CFunction=DocumentImplementationHasFeature] void hasFeature(any a0, any a1);
  [CFunction=DocumentImplementationCreateHTMLDocument] void createHTMLDocument(any a0);
};

[Proto=storage_proto, LocalProto]
interface ImplStorageProto {
  [CFunction=LocalStorageGetItem] void getItem(any a0);
  [CFunction=LocalStorageSetItem] void setItem(any a0, any a1);
  [CFunction=LocalStorageRemoveItem] void removeItem(any a0);
  [CFunction=LocalStorageClear] void clear();
  [CFunction=LocalStorageKey] void key(any a0);
};
