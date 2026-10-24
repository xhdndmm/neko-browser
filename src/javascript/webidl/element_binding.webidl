// Binding surface extracted from src/javascript/src/binding/element_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=class_list_proto]
interface ElementBindingClassListProto {
  [Getter=ClassListGetLength] readonly attribute any length;
  [CFunction=ClassListAdd] void add(any a0);
  [CFunction=ClassListRemove] void remove(any a0);
  [CFunction=ClassListToggle] void toggle(any a0);
  [CFunction=ClassListContains] void contains(any a0);
  [CFunction=ClassListReplace] void replace(any a0, any a1);
  [CFunction=ClassListItem] void item(any a0);
  [CFunction=ClassListToString] void toString();
};

[Proto=element_proto]
interface ElementBindingElementProto {
  [Getter=ElementGetTagName] readonly attribute any tagName;
  [Getter=ElementGetNamespaceURI] readonly attribute any namespaceURI;
  [Getter=ElementGetId, Setter=ElementSetId] attribute any id;
  [Getter=ElementGetClassName, Setter=ElementSetClassName] attribute any className;
  [Getter=ElementGetAttributes] readonly attribute any attributes;
  [Getter=ElementGetOffsetWidth] readonly attribute any offsetWidth;
  [Getter=ElementGetOffsetHeight] readonly attribute any offsetHeight;
  [Getter=ElementGetOffsetTop] readonly attribute any offsetTop;
  [Getter=ElementGetOffsetLeft] readonly attribute any offsetLeft;
  [Getter=ElementGetOffsetParent] readonly attribute any offsetParent;
  [Getter=ElementGetClientWidth] readonly attribute any clientWidth;
  [Getter=ElementGetClientHeight] readonly attribute any clientHeight;
  [Getter=ElementGetClientTop] readonly attribute any clientTop;
  [Getter=ElementGetClientLeft] readonly attribute any clientLeft;
  [Getter=ElementGetScrollTop, Setter=ElementSetScrollTop] attribute any scrollTop;
  [Getter=ElementGetScrollLeft, Setter=ElementSetScrollLeft] attribute any scrollLeft;
  [Getter=ElementGetScrollWidth] readonly attribute any scrollWidth;
  [Getter=ElementGetScrollHeight] readonly attribute any scrollHeight;
  [Getter=ElementGetChildren] readonly attribute any children;
  [Getter=ElementGetFirstElementChild] readonly attribute any firstElementChild;
  [Getter=ElementGetLastElementChild] readonly attribute any lastElementChild;
  [Getter=ElementGetNextElementSibling] readonly attribute any nextElementSibling;
  [Getter=ElementGetPreviousElementSibling] readonly attribute any previousElementSibling;
  [Getter=ElementGetInnerHTML, Setter=ElementSetInnerHTML] attribute any innerHTML;
  [Getter=ElementGetOuterHTML] readonly attribute any outerHTML;
  [Getter=NodeGetTextContent] readonly attribute any innerText;
  [Getter=ElementGetStyle] readonly attribute any style;
  [Getter=ElementGetClassList] readonly attribute any classList;
  [Getter=ElementGetDataset] readonly attribute any dataset;
  [Getter=ElementGetDir, Setter=ElementSetDir] attribute any dir;
  [Getter=ElementGetTabIndex, Setter=ElementSetTabIndex] attribute any tabIndex;
  [Getter=ElementGetValue, Setter=ElementSetValue] attribute any value;
  [Getter=ElementGetChecked, Setter=ElementSetChecked] attribute any checked;
  [Getter=ElementGetType, Setter=ElementSetType] attribute any type;
  [Getter=ElementGetPlaceholder, Setter=ElementSetPlaceholder] attribute any placeholder;
  [Getter=ElementGetDisabled, Setter=ElementSetDisabled] attribute any disabled;
  [Getter=ElementGetName, Setter=ElementSetName] attribute any name;
  [Getter=ElementGetHref, Setter=ElementSetHref] attribute any href;
  [Getter=ElementGetTarget, Setter=ElementSetTarget] attribute any target;
  [Getter=ElementGetRel, Setter=ElementSetRel] attribute any rel;
  [Getter=ElementGetSrc, Setter=ElementSetSrc] attribute any src;
  [Getter=ElementGetCurrentSrc, Setter=ElementSetSrc] attribute any currentSrc;
  [Getter=ElementGetAlt, Setter=ElementSetAlt] attribute any alt;
  [Getter=ElementGetWidth, Setter=ElementSetWidth] attribute any width;
  [Getter=ElementGetHeight, Setter=ElementSetHeight] attribute any height;
  [Getter=ElementGetNaturalWidth] readonly attribute any naturalWidth;
  [Getter=ElementGetNaturalHeight] readonly attribute any naturalHeight;
  [Getter=ElementGetComplete] readonly attribute any complete;
  [Getter=ElementGetVideoDuration] readonly attribute any duration;
  [Getter=ElementGetVideoCurrentTime, Setter=ElementSetVideoCurrentTime] attribute any currentTime;
  [Getter=ElementGetVideoPaused] readonly attribute any paused;
  [CFunction=ElementGetAttribute] void getAttribute(any a0);
  [CFunction=ElementGetAttributeNames] void getAttributeNames();
  [CFunction=ElementSetAttribute] void setAttribute(any a0, any a1);
  [CFunction=ElementRemoveAttribute] void removeAttribute(any a0);
  [CFunction=ElementHasAttribute] void hasAttribute(any a0);
  [CFunction=ElementScrollIntoView] void scrollIntoView(any a0);
  [CFunction=ElementToggleAttribute] void toggleAttribute(any a0);
  [CFunction=ElementQuerySelector] void querySelector(any a0);
  [CFunction=ElementQuerySelectorAll] void querySelectorAll(any a0);
  [CFunction=ElementGetElementsByTagName] void getElementsByTagName(any a0);
  [CFunction=ElementGetElementsByClassName] void getElementsByClassName(any a0);
  [CFunction=ElementMatches] void matches(any a0);
  [CFunction=ElementClosest] void closest(any a0);
  [CFunction=ElementRemove] void remove();
  [CFunction=ElementBlur] void blur();
  [CFunction=ElementInsertAdjacentHTML] void insertAdjacentHTML(any a0, any a1);
  [CFunction=ElementInsertAdjacentElement] void insertAdjacentElement(any a0, any a1);
  [CFunction=ElementInsertAdjacentText] void insertAdjacentText(any a0, any a1);
  [CFunction=ElementGetBoundingClientRect] void getBoundingClientRect();
  [CFunction=ElementPlayVideo] void play();
  [CFunction=ElementPauseVideo] void pause();
};

[Proto=svg_element_proto]
interface ElementBindingSvgElementProto {
  [Getter=ElementGetDataset] readonly attribute any dataset;
};

[Proto=html_template_element_proto]
interface ElementBindingHtmlTemplateElementProto {
  [Getter=ElementGetTemplateContent] readonly attribute any content;
};

[Proto=html_details_element_proto]
interface ElementBindingHtmlDetailsElementProto {
  [Getter=ElementGetOpen, Setter=ElementSetOpen] attribute any open;
};
