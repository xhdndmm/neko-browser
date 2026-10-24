// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/event_target_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr JSCFunctionListEntry kEventTargetBindingEventTargetProtoMethods[] = {
    JS_CFUNC_DEF("addEventListener", 2, EventTargetAddEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, EventTargetRemoveEventListener),
    JS_CFUNC_DEF("dispatchEvent", 1, EventTargetDispatchEvent),
};

} // namespace

void DefineGeneratedEventTargetBindingEventTargetProto(JSContext* ctx, Impl& impl)
{
  JS_SetPropertyFunctionList(
      ctx,
      impl.event_target_proto,
      kEventTargetBindingEventTargetProtoMethods,
      static_cast<int>(std::size(kEventTargetBindingEventTargetProtoMethods)));
}
