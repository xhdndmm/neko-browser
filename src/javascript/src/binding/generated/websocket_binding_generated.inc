// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/websocket_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

void DefineGeneratedWebsocketBindingProto(JSContext* ctx, JSValue proto)
{
  DefineGetter(ctx, proto, "url", MakeGetter(ctx, "url", WebSocketGetUrl));
  DefineGetter(ctx, proto, "readyState", MakeGetter(ctx, "readyState", WebSocketGetReadyState));
  DefineGetter(ctx, proto, "protocol", MakeGetter(ctx, "protocol", WebSocketGetProtocol));
}
