// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/headers_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr JSCFunctionListEntry kHeadersBindingPrototypeMethods[] = {
    JS_CFUNC_DEF("append", 2, Append),
    JS_CFUNC_DEF("set", 2, Set),
    JS_CFUNC_DEF("get", 1, Get),
    JS_CFUNC_DEF("has", 1, Has),
    JS_CFUNC_DEF("delete", 1, Delete),
    JS_CFUNC_DEF("entries", 0, Entries),
};

} // namespace

void DefineGeneratedHeadersBindingPrototype(JSContext* ctx, JSValue prototype)
{
  JS_SetPropertyFunctionList(ctx,
                             prototype,
                             kHeadersBindingPrototypeMethods,
                             static_cast<int>(std::size(kHeadersBindingPrototypeMethods)));
}
