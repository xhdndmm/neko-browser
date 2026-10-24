// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/intersection_observer_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr JSCFunctionListEntry kIntersectionObserverBindingIntersectionObserverProtoMethods[] = {
    JS_CFUNC_DEF("observe", 1, IoObserverObserve),
    JS_CFUNC_DEF("unobserve", 1, IoObserverUnobserve),
    JS_CFUNC_DEF("disconnect", 0, IoObserverDisconnect),
    JS_CFUNC_DEF("takeRecords", 0, IoObserverTakeRecords),
};

} // namespace

void DefineGeneratedIntersectionObserverBindingIntersectionObserverProto(JSContext* ctx, Impl& impl)
{
  JS_SetPropertyFunctionList(
      ctx,
      impl.intersection_observer_proto,
      kIntersectionObserverBindingIntersectionObserverProtoMethods,
      static_cast<int>(std::size(kIntersectionObserverBindingIntersectionObserverProtoMethods)));
}
