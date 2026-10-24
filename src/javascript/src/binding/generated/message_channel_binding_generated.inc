// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/message_channel_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr JSCFunctionListEntry kMessageChannelBindingMessagePortProtoMethods[] = {
    JS_CFUNC_DEF("postMessage", 1, PostMessage),
    JS_CFUNC_DEF("start", 0, Start),
    JS_CFUNC_DEF("close", 0, Close),
    JS_CFUNC_DEF("addEventListener", 2, AddEventListener),
};

} // namespace

void DefineGeneratedMessageChannelBindingMessagePortProto(JSContext* ctx, Impl& impl)
{
  JS_SetPropertyFunctionList(
      ctx,
      impl.message_port_proto,
      kMessageChannelBindingMessagePortProtoMethods,
      static_cast<int>(std::size(kMessageChannelBindingMessagePortProtoMethods)));
  DefineAccessor(ctx,
                 impl.message_port_proto,
                 "onmessage",
                 MakeGetter(ctx, "onmessage", GetOnMessage),
                 MakeSetter(ctx, "onmessage", SetOnMessage));
}
