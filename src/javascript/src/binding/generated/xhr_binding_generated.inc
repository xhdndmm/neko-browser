// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/xhr_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr JSCFunctionListEntry kXhrBindingProtoMethods[] = {
    JS_CFUNC_DEF("open", 3, XhrOpen),
    JS_CFUNC_DEF("setRequestHeader", 2, XhrSetRequestHeader),
    JS_CFUNC_DEF("send", 1, XhrSend),
    JS_CFUNC_DEF("abort", 0, XhrAbort),
    JS_CFUNC_DEF("getResponseHeader", 1, XhrGetResponseHeader),
    JS_CFUNC_DEF("getAllResponseHeaders", 0, XhrGetAllResponseHeaders),
    JS_CFUNC_DEF("addEventListener", 2, XhrAddEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, XhrRemoveEventListener),
};

} // namespace

void DefineGeneratedXhrBindingProto(JSContext* ctx, JSValue proto)
{
  JS_SetPropertyFunctionList(
      ctx, proto, kXhrBindingProtoMethods, static_cast<int>(std::size(kXhrBindingProtoMethods)));
  DefineGetter(ctx, proto, "readyState", MakeGetter(ctx, "readyState", XhrGetReadyState));
  DefineGetter(ctx, proto, "status", MakeGetter(ctx, "status", XhrGetStatus));
  DefineGetter(ctx, proto, "statusText", MakeGetter(ctx, "statusText", XhrGetStatusText));
  DefineGetter(ctx, proto, "responseText", MakeGetter(ctx, "responseText", XhrGetResponseText));
  DefineGetter(ctx, proto, "response", MakeGetter(ctx, "response", XhrGetResponseText));
  DefineGetter(ctx, proto, "responseURL", MakeGetter(ctx, "responseURL", XhrGetResponseUrl));
  DefineAccessor(ctx,
                 proto,
                 "responseType",
                 MakeGetter(ctx, "responseType", XhrGetResponseType),
                 MakeSetter(ctx, "responseType", XhrSetResponseType));
  DefineAccessor(ctx,
                 proto,
                 "withCredentials",
                 MakeGetter(ctx, "withCredentials", XhrGetWithCredentials),
                 MakeSetter(ctx, "withCredentials", XhrSetWithCredentials));
  DefineAccessor(ctx,
                 proto,
                 "timeout",
                 MakeGetter(ctx, "timeout", XhrGetTimeout),
                 MakeSetter(ctx, "timeout", XhrSetTimeout));
}
