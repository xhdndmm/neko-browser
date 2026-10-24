// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/element_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr JSCFunctionListEntry kElementBindingClassListProtoMethods[] = {
    JS_CFUNC_DEF("add", 1, ClassListAdd),
    JS_CFUNC_DEF("remove", 1, ClassListRemove),
    JS_CFUNC_DEF("toggle", 1, ClassListToggle),
    JS_CFUNC_DEF("contains", 1, ClassListContains),
    JS_CFUNC_DEF("replace", 2, ClassListReplace),
    JS_CFUNC_DEF("item", 1, ClassListItem),
    JS_CFUNC_DEF("toString", 0, ClassListToString),
};

} // namespace

void DefineGeneratedElementBindingClassListProto(JSContext* ctx, Impl& impl)
{
  JS_SetPropertyFunctionList(ctx,
                             impl.class_list_proto,
                             kElementBindingClassListProtoMethods,
                             static_cast<int>(std::size(kElementBindingClassListProtoMethods)));
  DefineGetter(ctx, impl.class_list_proto, "length", MakeGetter(ctx, "length", ClassListGetLength));
}

namespace {

constexpr JSCFunctionListEntry kElementBindingElementProtoMethods[] = {
    JS_CFUNC_DEF("getAttribute", 1, ElementGetAttribute),
    JS_CFUNC_DEF("getAttributeNames", 0, ElementGetAttributeNames),
    JS_CFUNC_DEF("setAttribute", 2, ElementSetAttribute),
    JS_CFUNC_DEF("removeAttribute", 1, ElementRemoveAttribute),
    JS_CFUNC_DEF("hasAttribute", 1, ElementHasAttribute),
    JS_CFUNC_DEF("scrollIntoView", 1, ElementScrollIntoView),
    JS_CFUNC_DEF("toggleAttribute", 1, ElementToggleAttribute),
    JS_CFUNC_DEF("querySelector", 1, ElementQuerySelector),
    JS_CFUNC_DEF("querySelectorAll", 1, ElementQuerySelectorAll),
    JS_CFUNC_DEF("getElementsByTagName", 1, ElementGetElementsByTagName),
    JS_CFUNC_DEF("getElementsByClassName", 1, ElementGetElementsByClassName),
    JS_CFUNC_DEF("matches", 1, ElementMatches),
    JS_CFUNC_DEF("closest", 1, ElementClosest),
    JS_CFUNC_DEF("remove", 0, ElementRemove),
    JS_CFUNC_DEF("blur", 0, ElementBlur),
    JS_CFUNC_DEF("insertAdjacentHTML", 2, ElementInsertAdjacentHTML),
    JS_CFUNC_DEF("insertAdjacentElement", 2, ElementInsertAdjacentElement),
    JS_CFUNC_DEF("insertAdjacentText", 2, ElementInsertAdjacentText),
    JS_CFUNC_DEF("getBoundingClientRect", 0, ElementGetBoundingClientRect),
    JS_CFUNC_DEF("play", 0, ElementPlayVideo),
    JS_CFUNC_DEF("pause", 0, ElementPauseVideo),
};

} // namespace

void DefineGeneratedElementBindingElementProto(JSContext* ctx, Impl& impl)
{
  JS_SetPropertyFunctionList(ctx,
                             impl.element_proto,
                             kElementBindingElementProtoMethods,
                             static_cast<int>(std::size(kElementBindingElementProtoMethods)));
  DefineGetter(ctx, impl.element_proto, "tagName", MakeGetter(ctx, "tagName", ElementGetTagName));
  DefineGetter(ctx,
               impl.element_proto,
               "namespaceURI",
               MakeGetter(ctx, "namespaceURI", ElementGetNamespaceURI));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "id",
                 MakeGetter(ctx, "id", ElementGetId),
                 MakeSetter(ctx, "id", ElementSetId));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "className",
                 MakeGetter(ctx, "className", ElementGetClassName),
                 MakeSetter(ctx, "className", ElementSetClassName));
  DefineGetter(
      ctx, impl.element_proto, "attributes", MakeGetter(ctx, "attributes", ElementGetAttributes));
  DefineGetter(ctx,
               impl.element_proto,
               "offsetWidth",
               MakeGetter(ctx, "offsetWidth", ElementGetOffsetWidth));
  DefineGetter(ctx,
               impl.element_proto,
               "offsetHeight",
               MakeGetter(ctx, "offsetHeight", ElementGetOffsetHeight));
  DefineGetter(
      ctx, impl.element_proto, "offsetTop", MakeGetter(ctx, "offsetTop", ElementGetOffsetTop));
  DefineGetter(
      ctx, impl.element_proto, "offsetLeft", MakeGetter(ctx, "offsetLeft", ElementGetOffsetLeft));
  DefineGetter(ctx,
               impl.element_proto,
               "offsetParent",
               MakeGetter(ctx, "offsetParent", ElementGetOffsetParent));
  DefineGetter(ctx,
               impl.element_proto,
               "clientWidth",
               MakeGetter(ctx, "clientWidth", ElementGetClientWidth));
  DefineGetter(ctx,
               impl.element_proto,
               "clientHeight",
               MakeGetter(ctx, "clientHeight", ElementGetClientHeight));
  DefineGetter(
      ctx, impl.element_proto, "clientTop", MakeGetter(ctx, "clientTop", ElementGetClientTop));
  DefineGetter(
      ctx, impl.element_proto, "clientLeft", MakeGetter(ctx, "clientLeft", ElementGetClientLeft));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "scrollTop",
                 MakeGetter(ctx, "scrollTop", ElementGetScrollTop),
                 MakeSetter(ctx, "scrollTop", ElementSetScrollTop));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "scrollLeft",
                 MakeGetter(ctx, "scrollLeft", ElementGetScrollLeft),
                 MakeSetter(ctx, "scrollLeft", ElementSetScrollLeft));
  DefineGetter(ctx,
               impl.element_proto,
               "scrollWidth",
               MakeGetter(ctx, "scrollWidth", ElementGetScrollWidth));
  DefineGetter(ctx,
               impl.element_proto,
               "scrollHeight",
               MakeGetter(ctx, "scrollHeight", ElementGetScrollHeight));
  DefineGetter(
      ctx, impl.element_proto, "children", MakeGetter(ctx, "children", ElementGetChildren));
  DefineGetter(ctx,
               impl.element_proto,
               "firstElementChild",
               MakeGetter(ctx, "firstElementChild", ElementGetFirstElementChild));
  DefineGetter(ctx,
               impl.element_proto,
               "lastElementChild",
               MakeGetter(ctx, "lastElementChild", ElementGetLastElementChild));
  DefineGetter(ctx,
               impl.element_proto,
               "nextElementSibling",
               MakeGetter(ctx, "nextElementSibling", ElementGetNextElementSibling));
  DefineGetter(ctx,
               impl.element_proto,
               "previousElementSibling",
               MakeGetter(ctx, "previousElementSibling", ElementGetPreviousElementSibling));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "innerHTML",
                 MakeGetter(ctx, "innerHTML", ElementGetInnerHTML),
                 MakeSetter(ctx, "innerHTML", ElementSetInnerHTML));
  DefineGetter(
      ctx, impl.element_proto, "outerHTML", MakeGetter(ctx, "outerHTML", ElementGetOuterHTML));
  DefineGetter(
      ctx, impl.element_proto, "innerText", MakeGetter(ctx, "innerText", NodeGetTextContent));
  DefineGetter(ctx, impl.element_proto, "style", MakeGetter(ctx, "style", ElementGetStyle));
  DefineGetter(
      ctx, impl.element_proto, "classList", MakeGetter(ctx, "classList", ElementGetClassList));
  DefineGetter(ctx, impl.element_proto, "dataset", MakeGetter(ctx, "dataset", ElementGetDataset));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "dir",
                 MakeGetter(ctx, "dir", ElementGetDir),
                 MakeSetter(ctx, "dir", ElementSetDir));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "tabIndex",
                 MakeGetter(ctx, "tabIndex", ElementGetTabIndex),
                 MakeSetter(ctx, "tabIndex", ElementSetTabIndex));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "value",
                 MakeGetter(ctx, "value", ElementGetValue),
                 MakeSetter(ctx, "value", ElementSetValue));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "checked",
                 MakeGetter(ctx, "checked", ElementGetChecked),
                 MakeSetter(ctx, "checked", ElementSetChecked));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "type",
                 MakeGetter(ctx, "type", ElementGetType),
                 MakeSetter(ctx, "type", ElementSetType));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "placeholder",
                 MakeGetter(ctx, "placeholder", ElementGetPlaceholder),
                 MakeSetter(ctx, "placeholder", ElementSetPlaceholder));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "disabled",
                 MakeGetter(ctx, "disabled", ElementGetDisabled),
                 MakeSetter(ctx, "disabled", ElementSetDisabled));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "name",
                 MakeGetter(ctx, "name", ElementGetName),
                 MakeSetter(ctx, "name", ElementSetName));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "href",
                 MakeGetter(ctx, "href", ElementGetHref),
                 MakeSetter(ctx, "href", ElementSetHref));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "target",
                 MakeGetter(ctx, "target", ElementGetTarget),
                 MakeSetter(ctx, "target", ElementSetTarget));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "rel",
                 MakeGetter(ctx, "rel", ElementGetRel),
                 MakeSetter(ctx, "rel", ElementSetRel));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "src",
                 MakeGetter(ctx, "src", ElementGetSrc),
                 MakeSetter(ctx, "src", ElementSetSrc));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "currentSrc",
                 MakeGetter(ctx, "currentSrc", ElementGetCurrentSrc),
                 MakeSetter(ctx, "currentSrc", ElementSetSrc));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "alt",
                 MakeGetter(ctx, "alt", ElementGetAlt),
                 MakeSetter(ctx, "alt", ElementSetAlt));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "width",
                 MakeGetter(ctx, "width", ElementGetWidth),
                 MakeSetter(ctx, "width", ElementSetWidth));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "height",
                 MakeGetter(ctx, "height", ElementGetHeight),
                 MakeSetter(ctx, "height", ElementSetHeight));
  DefineGetter(ctx,
               impl.element_proto,
               "naturalWidth",
               MakeGetter(ctx, "naturalWidth", ElementGetNaturalWidth));
  DefineGetter(ctx,
               impl.element_proto,
               "naturalHeight",
               MakeGetter(ctx, "naturalHeight", ElementGetNaturalHeight));
  DefineGetter(
      ctx, impl.element_proto, "complete", MakeGetter(ctx, "complete", ElementGetComplete));
  DefineGetter(
      ctx, impl.element_proto, "duration", MakeGetter(ctx, "duration", ElementGetVideoDuration));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "currentTime",
                 MakeGetter(ctx, "currentTime", ElementGetVideoCurrentTime),
                 MakeSetter(ctx, "currentTime", ElementSetVideoCurrentTime));
  DefineGetter(ctx, impl.element_proto, "paused", MakeGetter(ctx, "paused", ElementGetVideoPaused));
}

void DefineGeneratedElementBindingSvgElementProto(JSContext* ctx, Impl& impl)
{
  DefineGetter(
      ctx, impl.svg_element_proto, "dataset", MakeGetter(ctx, "dataset", ElementGetDataset));
}

void DefineGeneratedElementBindingHtmlTemplateElementProto(JSContext* ctx, Impl& impl)
{
  DefineGetter(ctx,
               impl.html_template_element_proto,
               "content",
               MakeGetter(ctx, "content", ElementGetTemplateContent));
}

void DefineGeneratedElementBindingHtmlDetailsElementProto(JSContext* ctx, Impl& impl)
{
  DefineAccessor(ctx,
                 impl.html_details_element_proto,
                 "open",
                 MakeGetter(ctx, "open", ElementGetOpen),
                 MakeSetter(ctx, "open", ElementSetOpen));
}
