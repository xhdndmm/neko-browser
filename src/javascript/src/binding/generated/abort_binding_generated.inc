// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/abort_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr JSCFunctionListEntry kAbortBindingAbortSignalProtoMethods[] = {
    JS_CFUNC_DEF("addEventListener", 2, SignalAddEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, SignalRemoveEventListener),
    JS_CFUNC_DEF("throwIfAborted", 0, SignalThrowIfAborted),
};

} // namespace

void DefineGeneratedAbortBindingAbortSignalProto(JSContext* ctx, Impl& impl)
{
  JS_SetPropertyFunctionList(ctx,
                             impl.abort_signal_proto,
                             kAbortBindingAbortSignalProtoMethods,
                             static_cast<int>(std::size(kAbortBindingAbortSignalProtoMethods)));
  DefineGetter(
      ctx, impl.abort_signal_proto, "aborted", MakeGetter(ctx, "aborted", SignalGetAborted));
  DefineGetter(ctx, impl.abort_signal_proto, "reason", MakeGetter(ctx, "reason", SignalGetReason));
}

namespace {

constexpr JSCFunctionListEntry kAbortBindingAbortControllerProtoMethods[] = {
    JS_CFUNC_DEF("abort", 0, ControllerAbort),
};

} // namespace

void DefineGeneratedAbortBindingAbortControllerProto(JSContext* ctx, Impl& impl)
{
  JS_SetPropertyFunctionList(ctx,
                             impl.abort_controller_proto,
                             kAbortBindingAbortControllerProtoMethods,
                             static_cast<int>(std::size(kAbortBindingAbortControllerProtoMethods)));
  DefineGetter(
      ctx, impl.abort_controller_proto, "signal", MakeGetter(ctx, "signal", ControllerGetSignal));
}
