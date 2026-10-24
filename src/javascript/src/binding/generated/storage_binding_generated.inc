// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/storage_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr JSCFunctionListEntry kStorageBindingPrototypeMethods[] = {
    JS_CFUNC_DEF("text", 0, ResponseText),
    JS_CFUNC_DEF("json", 0, ResponseJson),
};

} // namespace

void DefineGeneratedStorageBindingPrototype(JSContext* ctx, JSValue prototype)
{
  JS_SetPropertyFunctionList(ctx,
                             prototype,
                             kStorageBindingPrototypeMethods,
                             static_cast<int>(std::size(kStorageBindingPrototypeMethods)));
}
