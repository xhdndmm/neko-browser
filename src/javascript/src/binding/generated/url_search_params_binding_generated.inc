// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/url_search_params_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr JSCFunctionListEntry kUrlSearchParamsBindingProtoMethods[] = {
    JS_CFUNC_DEF("append", 2, Append),
    JS_CFUNC_DEF("set", 2, Set),
    JS_CFUNC_DEF("delete", 1, Delete),
    JS_CFUNC_DEF("get", 1, Get),
    JS_CFUNC_DEF("getAll", 1, GetAll),
    JS_CFUNC_DEF("has", 1, Has),
    JS_CFUNC_DEF("sort", 0, Sort),
    JS_CFUNC_DEF("toString", 0, ToString),
};

} // namespace

void DefineGeneratedUrlSearchParamsBindingProto(JSContext* ctx, JSValue proto)
{
  JS_SetPropertyFunctionList(ctx,
                             proto,
                             kUrlSearchParamsBindingProtoMethods,
                             static_cast<int>(std::size(kUrlSearchParamsBindingProtoMethods)));
  DefineGetter(ctx, proto, "size", MakeGetter(ctx, "size", Size));
}
