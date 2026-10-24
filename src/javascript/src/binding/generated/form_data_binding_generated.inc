// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/form_data_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr JSCFunctionListEntry kFormDataBindingProtoMethods[] = {
    JS_CFUNC_DEF("append", 2, FormDataAppend),
    JS_CFUNC_DEF("set", 2, FormDataSet),
    JS_CFUNC_DEF("delete", 1, FormDataDelete),
    JS_CFUNC_DEF("get", 1, FormDataGet),
    JS_CFUNC_DEF("has", 1, FormDataHas),
    JS_CFUNC_DEF("entries", 0, FormDataEntries),
};

} // namespace

void DefineGeneratedFormDataBindingProto(JSContext* ctx, JSValue proto)
{
  JS_SetPropertyFunctionList(ctx,
                             proto,
                             kFormDataBindingProtoMethods,
                             static_cast<int>(std::size(kFormDataBindingProtoMethods)));
}
