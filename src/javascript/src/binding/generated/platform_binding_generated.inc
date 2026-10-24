// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/platform_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

void DefineGeneratedPlatformBindingEncoderProto(JSContext* ctx, JSValue encoder_proto)
{
  DefineGetter(ctx, encoder_proto, "encoding", MakeGetter(ctx, "encoding", EncodingGetterImpl));
}

void DefineGeneratedPlatformBindingDecoderProto(JSContext* ctx, JSValue decoder_proto)
{
  DefineGetter(ctx, decoder_proto, "encoding", MakeGetter(ctx, "encoding", EncodingGetterImpl));
}
