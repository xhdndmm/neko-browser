// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/style_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr JSCFunctionListEntry kStyleBindingStyleProtoMethods[] = {
    JS_CFUNC_DEF("setProperty", 2, StyleSetProperty),
    JS_CFUNC_DEF("getPropertyValue", 1, StyleGetPropertyValue),
    JS_CFUNC_DEF("removeProperty", 1, StyleRemoveProperty),
};

} // namespace

void DefineGeneratedStyleBindingStyleProto(JSContext* ctx, Impl& impl)
{
  JS_SetPropertyFunctionList(ctx,
                             impl.style_proto,
                             kStyleBindingStyleProtoMethods,
                             static_cast<int>(std::size(kStyleBindingStyleProtoMethods)));
}
