// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/canvas_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

void DefineGeneratedCanvasBindingCanvas2dProto(JSContext* ctx, Impl& impl)
{
  DefineAccessor(ctx,
                 impl.canvas_2d_proto,
                 "fillStyle",
                 MakeGetter(ctx, "fillStyle", CanvasGetFillStyle),
                 MakeSetter(ctx, "fillStyle", CanvasSetFillStyle));
}
