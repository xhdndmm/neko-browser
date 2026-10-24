// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/impl.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

void DefineGeneratedImplNodeListProto(JSContext* ctx, Impl& impl)
{
  DefineGetter(ctx, impl.node_list_proto, "length", MakeGetter(ctx, "length", NodeListLength));
}

void DefineGeneratedImplHtmlCollectionProto(JSContext* ctx, Impl& impl)
{
  DefineGetter(
      ctx, impl.html_collection_proto, "length", MakeGetter(ctx, "length", NodeListLength));
}

namespace {

constexpr JSCFunctionListEntry kImplWindowMethods[] = {
    JS_CFUNC_DEF("addEventListener", 2, WindowAddEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, WindowRemoveEventListener),
    JS_CFUNC_DEF("dispatchEvent", 1, WindowDispatchEvent),
    JS_CFUNC_DEF("requestAnimationFrame", 1, WindowRequestAnimationFrame),
    JS_CFUNC_DEF("cancelAnimationFrame", 1, WindowCancelAnimationFrame),
    JS_CFUNC_DEF("scrollTo", 0, WindowScrollTo),
    JS_CFUNC_DEF("scroll", 0, WindowScrollTo),
    JS_CFUNC_DEF("scrollBy", 0, WindowScrollBy),
    JS_CFUNC_DEF("getComputedStyle", 1, WindowGetComputedStyle),
    JS_CFUNC_DEF("requestIdleCallback", 1, WindowRequestAnimationFrame),
    JS_CFUNC_DEF("cancelIdleCallback", 1, WindowCancelAnimationFrame),
    JS_CFUNC_DEF("matchMedia", 1, WindowMatchMedia),
};

} // namespace

void DefineGeneratedImplWindow(JSContext* ctx, Impl& impl)
{
  JS_SetPropertyFunctionList(
      ctx, impl.window, kImplWindowMethods, static_cast<int>(std::size(kImplWindowMethods)));
  DefineGetter(ctx, impl.window, "closed", MakeGetter(ctx, "closed", WindowGetClosed));
  DefineGetter(
      ctx, impl.window, "innerWidth", MakeGetterMagic(ctx, "innerWidth", WindowViewportGetter, 0));
  DefineGetter(ctx,
               impl.window,
               "innerHeight",
               MakeGetterMagic(ctx, "innerHeight", WindowViewportGetter, 1));
  DefineGetter(
      ctx, impl.window, "outerWidth", MakeGetterMagic(ctx, "outerWidth", WindowViewportGetter, 0));
  DefineGetter(ctx,
               impl.window,
               "outerHeight",
               MakeGetterMagic(ctx, "outerHeight", WindowViewportGetter, 2));
  DefineGetter(ctx,
               impl.window,
               "devicePixelRatio",
               MakeGetterMagic(ctx, "devicePixelRatio", WindowViewportGetter, 3));
  DefineGetter(ctx,
               impl.window,
               "pageXOffset",
               MakeGetterMagic(ctx, "pageXOffset", WindowScrollOffsetGetter, 0));
  DefineGetter(ctx,
               impl.window,
               "pageYOffset",
               MakeGetterMagic(ctx, "pageYOffset", WindowScrollOffsetGetter, 1));
  DefineGetter(
      ctx, impl.window, "scrollX", MakeGetterMagic(ctx, "scrollX", WindowScrollOffsetGetter, 2));
  DefineGetter(
      ctx, impl.window, "scrollY", MakeGetterMagic(ctx, "scrollY", WindowScrollOffsetGetter, 3));
}

void DefineGeneratedImplCharacterDataProto(JSContext* ctx, Impl& impl)
{
  DefineAccessor(ctx,
                 impl.character_data_proto,
                 "data",
                 MakeGetter(ctx, "data", CharacterDataGetData),
                 MakeSetter(ctx, "data", CharacterDataSetData));
}

void DefineGeneratedImplFragmentProto(JSContext* ctx, Impl& impl)
{
  DefineGetter(
      ctx, impl.fragment_proto, "children", MakeGetter(ctx, "children", ElementGetChildren));
}

void DefineGeneratedImplDocumentTypeProto(JSContext* ctx, Impl& impl)
{
  DefineGetter(ctx, impl.document_type_proto, "name", MakeGetter(ctx, "name", DocumentTypeGetName));
  DefineGetter(ctx,
               impl.document_type_proto,
               "publicId",
               MakeGetter(ctx, "publicId", DocumentTypeGetPublicId));
  DefineGetter(ctx,
               impl.document_type_proto,
               "systemId",
               MakeGetter(ctx, "systemId", DocumentTypeGetSystemId));
}

void DefineGeneratedImplHtmlScriptElementProto(JSContext* ctx, Impl& impl)
{
  DefineAccessor(ctx,
                 impl.html_script_element_proto,
                 "src",
                 MakeGetter(ctx, "src", ElementGetSrc),
                 MakeSetter(ctx, "src", ElementSetSrc));
}

void DefineGeneratedImplHtmlBaseElementProto(JSContext* ctx, Impl& impl)
{
  DefineAccessor(ctx,
                 impl.html_base_element_proto,
                 "href",
                 MakeGetter(ctx, "href", ElementGetHref),
                 MakeSetter(ctx, "href", ElementSetHref));
}

void DefineGeneratedImplHtmlAnchorElementProto(JSContext* ctx, Impl& impl)
{
  DefineAccessor(ctx,
                 impl.html_anchor_element_proto,
                 "href",
                 MakeGetter(ctx, "href", ElementGetHref),
                 MakeSetter(ctx, "href", ElementSetHref));
  DefineAccessor(ctx,
                 impl.html_anchor_element_proto,
                 "download",
                 MakeGetter(ctx, "download", ElementGetDownload),
                 MakeSetter(ctx, "download", ElementSetDownload));
  DefineAccessor(ctx,
                 impl.html_anchor_element_proto,
                 "ping",
                 MakeGetter(ctx, "ping", ElementGetPing),
                 MakeSetter(ctx, "ping", ElementSetPing));
}

void DefineGeneratedImplHtmlIframeElementProto(JSContext* ctx, Impl& impl)
{
  DefineAccessor(ctx,
                 impl.html_iframe_element_proto,
                 "src",
                 MakeGetter(ctx, "src", ElementGetSrc),
                 MakeSetter(ctx, "src", ElementSetSrc));
  DefineAccessor(ctx,
                 impl.html_iframe_element_proto,
                 "srcdoc",
                 MakeGetter(ctx, "srcdoc", ElementGetSrcDoc),
                 MakeSetter(ctx, "srcdoc", ElementSetSrcDoc));
  DefineAccessor(ctx,
                 impl.html_iframe_element_proto,
                 "credentialless",
                 MakeGetter(ctx, "credentialless", ElementGetCredentialless),
                 MakeSetter(ctx, "credentialless", ElementSetCredentialless));
}

void DefineGeneratedImplHtmlFormElementProto(JSContext* ctx, Impl& impl)
{
  DefineAccessor(ctx,
                 impl.html_form_element_proto,
                 "action",
                 MakeGetter(ctx, "action", FormGetAction),
                 MakeSetter(ctx, "action", FormSetAction));
  DefineAccessor(ctx,
                 impl.html_form_element_proto,
                 "enctype",
                 MakeGetter(ctx, "enctype", FormGetEnctype),
                 MakeSetter(ctx, "enctype", FormSetEnctype));
  DefineAccessor(ctx,
                 impl.html_form_element_proto,
                 "method",
                 MakeGetter(ctx, "method", FormGetMethod),
                 MakeSetter(ctx, "method", FormSetMethod));
  DefineAccessor(ctx,
                 impl.html_form_element_proto,
                 "noValidate",
                 MakeGetter(ctx, "noValidate", FormGetNoValidate),
                 MakeSetter(ctx, "noValidate", FormSetNoValidate));
}

void DefineGeneratedImplHtmlButtonElementProto(JSContext* ctx, Impl& impl)
{
  DefineAccessor(ctx,
                 impl.html_button_element_proto,
                 "formAction",
                 MakeGetter(ctx, "formAction", ElementGetFormAction),
                 MakeSetter(ctx, "formAction", ElementSetFormAction));
}

void DefineGeneratedImplHtmlInputElementProto(JSContext* ctx, Impl& impl)
{
  DefineAccessor(ctx,
                 impl.html_input_element_proto,
                 "formAction",
                 MakeGetter(ctx, "formAction", ElementGetFormAction),
                 MakeSetter(ctx, "formAction", ElementSetFormAction));
  DefineAccessor(ctx,
                 impl.html_input_element_proto,
                 "value",
                 MakeGetter(ctx, "value", ElementGetValue),
                 MakeSetter(ctx, "value", ElementSetValue));
  DefineAccessor(ctx,
                 impl.html_input_element_proto,
                 "defaultValue",
                 MakeGetter(ctx, "defaultValue", ElementGetDefaultValue),
                 MakeSetter(ctx, "defaultValue", ElementSetDefaultValue));
  DefineAccessor(ctx,
                 impl.html_input_element_proto,
                 "multiple",
                 MakeGetter(ctx, "multiple", ElementGetMultiple),
                 MakeSetter(ctx, "multiple", ElementSetMultiple));
  DefineAccessor(ctx,
                 impl.html_input_element_proto,
                 "min",
                 MakeGetter(ctx, "min", ElementGetMin),
                 MakeSetter(ctx, "min", ElementSetMin));
  DefineAccessor(ctx,
                 impl.html_input_element_proto,
                 "max",
                 MakeGetter(ctx, "max", ElementGetMax),
                 MakeSetter(ctx, "max", ElementSetMax));
  DefineAccessor(ctx,
                 impl.html_input_element_proto,
                 "step",
                 MakeGetter(ctx, "step", ElementGetStep),
                 MakeSetter(ctx, "step", ElementSetStep));
}

void DefineGeneratedImplHtmlImageElementProto(JSContext* ctx, Impl& impl)
{
  DefineAccessor(ctx,
                 impl.html_image_element_proto,
                 "src",
                 MakeGetter(ctx, "src", ElementGetSrc),
                 MakeSetter(ctx, "src", ElementSetSrc));
  DefineAccessor(ctx,
                 impl.html_image_element_proto,
                 "srcset",
                 MakeGetter(ctx, "srcset", ElementGetSrcSet),
                 MakeSetter(ctx, "srcset", ElementSetSrcSet));
}

void DefineGeneratedImplHtmlMediaElementProto(JSContext* ctx, Impl& impl)
{
  DefineAccessor(ctx,
                 impl.html_media_element_proto,
                 "src",
                 MakeGetter(ctx, "src", ElementGetSrc),
                 MakeSetter(ctx, "src", ElementSetSrc));
  DefineGetter(ctx,
               impl.html_media_element_proto,
               "currentSrc",
               MakeGetter(ctx, "currentSrc", ElementGetCurrentSrc));
}

void DefineGeneratedImplAttrProto(JSContext* ctx, Impl& impl)
{
  DefineAccessor(ctx,
                 impl.attr_proto,
                 "value",
                 MakeGetter(ctx, "value", AttrGetValue),
                 MakeSetter(ctx, "value", AttrSetValue));
  DefineAccessor(ctx,
                 impl.attr_proto,
                 "nodeValue",
                 MakeGetter(ctx, "nodeValue", AttrGetNodeValue),
                 MakeSetter(ctx, "nodeValue", AttrSetNodeValue));
  DefineGetter(
      ctx, impl.attr_proto, "ownerElement", MakeGetter(ctx, "ownerElement", AttrGetOwnerElement));
}

void DefineGeneratedImplNavigatorProto(JSContext* ctx, JSValue navigator_proto)
{
  DefineGetter(
      ctx, navigator_proto, "languages", MakeGetter(ctx, "languages", NavigatorGetLanguages));
}

namespace {

constexpr JSCFunctionListEntry kImplHistoryProtoMethods[] = {
    JS_CFUNC_DEF("back", 0, HistoryBack),
    JS_CFUNC_DEF("forward", 0, HistoryForward),
    JS_CFUNC_DEF("go", 1, HistoryGo),
    JS_CFUNC_DEF("pushState", 3, HistoryPushState),
    JS_CFUNC_DEF("replaceState", 3, HistoryReplaceState),
};

} // namespace

void DefineGeneratedImplHistoryProto(JSContext* ctx, JSValue history_proto)
{
  JS_SetPropertyFunctionList(ctx,
                             history_proto,
                             kImplHistoryProtoMethods,
                             static_cast<int>(std::size(kImplHistoryProtoMethods)));
}

namespace {

constexpr JSCFunctionListEntry kImplPerformanceProtoMethods[] = {
    JS_CFUNC_DEF("now", 0, PerformanceNow),
    JS_CFUNC_DEF("getEntries", 0, PerformanceGetEntries),
    JS_CFUNC_DEF("getEntriesByType", 1, PerformanceGetEntriesByType),
    JS_CFUNC_DEF("setResourceTimingBufferSize", 1, PerformanceSetResourceTimingBufferSize),
    JS_CFUNC_DEF("clearResourceTimings", 0, PerformanceClearResourceTimings),
};

} // namespace

void DefineGeneratedImplPerformanceProto(JSContext* ctx, JSValue performance_proto)
{
  JS_SetPropertyFunctionList(ctx,
                             performance_proto,
                             kImplPerformanceProtoMethods,
                             static_cast<int>(std::size(kImplPerformanceProtoMethods)));
}

namespace {

constexpr JSCFunctionListEntry kImplNamedNodeMapProtoMethods[] = {
    JS_CFUNC_DEF("item", 1, NamedNodeMapItem),
    JS_CFUNC_DEF("getNamedItem", 1, NamedNodeMapGetNamedItem),
};

} // namespace

void DefineGeneratedImplNamedNodeMapProto(JSContext* ctx, Impl& impl)
{
  JS_SetPropertyFunctionList(ctx,
                             impl.named_node_map_proto,
                             kImplNamedNodeMapProtoMethods,
                             static_cast<int>(std::size(kImplNamedNodeMapProtoMethods)));
}

namespace {

constexpr JSCFunctionListEntry kImplTreeWalkerProtoMethods[] = {
    JS_CFUNC_DEF("nextNode", 0, TreeWalkerNextNode),
};

} // namespace

void DefineGeneratedImplTreeWalkerProto(JSContext* ctx, Impl& impl)
{
  JS_SetPropertyFunctionList(ctx,
                             impl.tree_walker_proto,
                             kImplTreeWalkerProtoMethods,
                             static_cast<int>(std::size(kImplTreeWalkerProtoMethods)));
}

namespace {

constexpr JSCFunctionListEntry kImplMutationObserverProtoMethods[] = {
    JS_CFUNC_DEF("observe", 2, MutationObserverObserve),
    JS_CFUNC_DEF("disconnect", 0, MutationObserverDisconnect),
    JS_CFUNC_DEF("takeRecords", 0, MutationObserverTakeRecords),
};

} // namespace

void DefineGeneratedImplMutationObserverProto(JSContext* ctx, Impl& impl)
{
  JS_SetPropertyFunctionList(ctx,
                             impl.mutation_observer_proto,
                             kImplMutationObserverProtoMethods,
                             static_cast<int>(std::size(kImplMutationObserverProtoMethods)));
}

namespace {

constexpr JSCFunctionListEntry kImplDomImplementationProtoMethods[] = {
    JS_CFUNC_DEF("hasFeature", 2, DocumentImplementationHasFeature),
    JS_CFUNC_DEF("createHTMLDocument", 1, DocumentImplementationCreateHTMLDocument),
};

} // namespace

void DefineGeneratedImplDomImplementationProto(JSContext* ctx, Impl& impl)
{
  JS_SetPropertyFunctionList(ctx,
                             impl.dom_implementation_proto,
                             kImplDomImplementationProtoMethods,
                             static_cast<int>(std::size(kImplDomImplementationProtoMethods)));
}

namespace {

constexpr JSCFunctionListEntry kImplStorageProtoMethods[] = {
    JS_CFUNC_DEF("getItem", 1, LocalStorageGetItem),
    JS_CFUNC_DEF("setItem", 2, LocalStorageSetItem),
    JS_CFUNC_DEF("removeItem", 1, LocalStorageRemoveItem),
    JS_CFUNC_DEF("clear", 0, LocalStorageClear),
    JS_CFUNC_DEF("key", 1, LocalStorageKey),
};

} // namespace

void DefineGeneratedImplStorageProto(JSContext* ctx, JSValue storage_proto)
{
  JS_SetPropertyFunctionList(ctx,
                             storage_proto,
                             kImplStorageProtoMethods,
                             static_cast<int>(std::size(kImplStorageProtoMethods)));
}
