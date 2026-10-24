// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/global_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr JSCFunctionListEntry kGlobalBindingRegistryMethods[] = {
    JS_CFUNC_DEF("define", 2, CustomElementsDefine),
    JS_CFUNC_DEF("get", 1, CustomElementsGet),
    JS_CFUNC_DEF("whenDefined", 1, CustomElementsWhenDefined),
    JS_CFUNC_DEF("upgrade", 1, CustomElementsUpgrade),
};

} // namespace

void DefineGeneratedGlobalBindingRegistry(JSContext* ctx, JSValue registry)
{
  JS_SetPropertyFunctionList(ctx,
                             registry,
                             kGlobalBindingRegistryMethods,
                             static_cast<int>(std::size(kGlobalBindingRegistryMethods)));
}

namespace {

constexpr JSCFunctionListEntry kGlobalBindingListMethods[] = {
    JS_CFUNC_DEF("addEventListener", 2, MatchMediaAddListener),
    JS_CFUNC_DEF("removeEventListener", 2, MatchMediaRemoveListener),
    JS_CFUNC_DEF("addListener", 1, MatchMediaAddListener),
    JS_CFUNC_DEF("removeListener", 1, MatchMediaRemoveListener),
};

} // namespace

void DefineGeneratedGlobalBindingList(JSContext* ctx, JSValue list)
{
  JS_SetPropertyFunctionList(
      ctx, list, kGlobalBindingListMethods, static_cast<int>(std::size(kGlobalBindingListMethods)));
}
