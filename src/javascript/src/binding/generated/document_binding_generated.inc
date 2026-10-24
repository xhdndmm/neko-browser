// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/document_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

void DefineGeneratedDocumentBindingCssStyleSheetProto(JSContext* ctx, Impl& impl)
{
  DefineGetter(ctx, impl.css_style_sheet_proto, "href", MakeGetter(ctx, "href", StyleSheetGetHref));
  DefineGetter(ctx,
               impl.css_style_sheet_proto,
               "ownerNode",
               MakeGetter(ctx, "ownerNode", StyleSheetGetOwnerNode));
  DefineGetter(ctx,
               impl.css_style_sheet_proto,
               "cssRules",
               MakeGetter(ctx, "cssRules", StyleSheetGetCssRules));
}

namespace {

constexpr JSCFunctionListEntry kDocumentBindingDocumentProtoMethods[] = {
    JS_CFUNC_DEF("getElementById", 1, DocGetElementById),
    JS_CFUNC_DEF("createElement", 1, DocCreateElement),
    JS_CFUNC_DEF("createElementNS", 2, DocCreateElementNS),
    JS_CFUNC_DEF("createTextNode", 1, DocCreateTextNode),
    JS_CFUNC_DEF("createDocumentFragment", 0, DocCreateDocumentFragment),
    JS_CFUNC_DEF("createTreeWalker", 1, DocCreateTreeWalker),
    JS_CFUNC_DEF("createRange", 0, DocCreateRange),
    JS_CFUNC_DEF("createComment", 1, DocCreateComment),
    JS_CFUNC_DEF("createEvent", 1, DocCreateEvent),
    JS_CFUNC_DEF("getSelection", 0, DocGetSelection),
    JS_CFUNC_DEF("querySelector", 1, DocQuerySelector),
    JS_CFUNC_DEF("querySelectorAll", 1, DocQuerySelectorAll),
    JS_CFUNC_DEF("getElementsByTagName", 1, DocGetElementsByTagName),
    JS_CFUNC_DEF("getElementsByClassName", 1, DocGetElementsByClassName),
    JS_CFUNC_DEF("write", 1, DocWrite),
    JS_CFUNC_DEF("writeln", 1, DocWrite),
    JS_CFUNC_DEF("hasFocus", 0, DocHasFocus),
};

} // namespace

void DefineGeneratedDocumentBindingDocumentProto(JSContext* ctx, Impl& impl)
{
  JS_SetPropertyFunctionList(ctx,
                             impl.document_proto,
                             kDocumentBindingDocumentProtoMethods,
                             static_cast<int>(std::size(kDocumentBindingDocumentProtoMethods)));
  DefineGetter(ctx,
               impl.document_proto,
               "implementation",
               MakeGetter(ctx, "implementation", DocGetImplementation));
  DefineGetter(ctx,
               impl.document_proto,
               "documentElement",
               MakeGetter(ctx, "documentElement", DocGetDocumentElement));
  DefineGetter(ctx, impl.document_proto, "doctype", MakeGetter(ctx, "doctype", DocGetDoctype));
  DefineGetter(ctx, impl.document_proto, "body", MakeGetter(ctx, "body", DocGetBody));
  DefineGetter(ctx,
               impl.document_proto,
               "activeElement",
               MakeGetter(ctx, "activeElement", DocGetActiveElement));
  DefineGetter(ctx, impl.document_proto, "head", MakeGetter(ctx, "head", DocGetHead));
  DefineGetter(
      ctx, impl.document_proto, "readyState", MakeGetter(ctx, "readyState", DocGetReadyState));
  DefineAccessor(ctx,
                 impl.document_proto,
                 "title",
                 MakeGetter(ctx, "title", DocGetTitle),
                 MakeSetter(ctx, "title", DocSetTitle));
  DefineGetter(ctx, impl.document_proto, "URL", MakeGetter(ctx, "URL", DocGetURL));
  DefineAccessor(ctx,
                 impl.document_proto,
                 "domain",
                 MakeGetter(ctx, "domain", DocGetDomain),
                 MakeSetter(ctx, "domain", DocSetDomain));
  DefineAccessor(ctx,
                 impl.document_proto,
                 "cookie",
                 MakeGetter(ctx, "cookie", DocGetCookie),
                 MakeSetter(ctx, "cookie", DocSetCookie));
  DefineGetter(
      ctx, impl.document_proto, "defaultView", MakeGetter(ctx, "defaultView", DocGetDefaultView));
  DefineGetter(ctx, impl.document_proto, "baseURI", MakeGetter(ctx, "baseURI", DocGetBaseURI));
  DefineGetter(
      ctx, impl.document_proto, "documentURI", MakeGetter(ctx, "documentURI", DocGetDocumentURI));
  DefineGetter(ctx,
               impl.document_proto,
               "characterSet",
               MakeGetter(ctx, "characterSet", DocGetCharacterSet));
  DefineGetter(
      ctx, impl.document_proto, "contentType", MakeGetter(ctx, "contentType", DocGetContentType));
  DefineGetter(ctx, impl.document_proto, "referrer", MakeGetter(ctx, "referrer", DocGetReferrer));
  DefineGetter(ctx, impl.document_proto, "forms", MakeGetter(ctx, "forms", DocGetForms));
  DefineGetter(ctx, impl.document_proto, "images", MakeGetter(ctx, "images", DocGetImages));
  DefineGetter(ctx, impl.document_proto, "links", MakeGetter(ctx, "links", DocGetLinks));
  DefineGetter(ctx, impl.document_proto, "scripts", MakeGetter(ctx, "scripts", DocGetScripts));
  DefineGetter(
      ctx, impl.document_proto, "styleSheets", MakeGetter(ctx, "styleSheets", DocGetStyleSheets));
  DefineGetter(ctx,
               impl.document_proto,
               "currentScript",
               MakeGetter(ctx, "currentScript", DocGetCurrentScript));
}
