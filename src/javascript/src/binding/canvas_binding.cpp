// neko::javascript DOM bindings — minimal HTML Canvas 2D support.

#include "neko/css/color.h"

#include "binding_internal.h"

#include <array>
#include <cmath>
#include <quickjs.h>
#include <string>

namespace neko::javascript {

namespace {

JSClassID g_canvas_class_id = 0;

struct CanvasContextWrapper
{
  Impl* impl = nullptr;
  dom::Element* canvas = nullptr;
  css::Color fill_style{0, 0, 0, 255};
  std::string fill_style_text = "#000000";
};

void CanvasFinalizer(JSRuntime*, JSValue object)
{
  delete static_cast<CanvasContextWrapper*>(JS_GetOpaque(object, g_canvas_class_id));
}

CanvasContextWrapper* UnwrapCanvasContext(JSValueConst value)
{
  return static_cast<CanvasContextWrapper*>(JS_GetOpaque(value, g_canvas_class_id));
}

JSValue CanvasGetContext(JSContext* ctx, JSValueConst this_value, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_value);
  dom::Element* canvas = AsElement(UnwrapNode(this_value));
  if (impl == nullptr || canvas == nullptr || canvas->tag_name() != "canvas") {
    return JS_ThrowTypeError(ctx, "getContext called on incompatible receiver");
  }
  if (argc < 1) {
    return JS_NULL;
  }
  bool ok = false;
  const std::string type = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (type != "2d") {
    return JS_NULL;
  }
  const auto existing = impl->canvas_contexts.find(canvas);
  if (existing != impl->canvas_contexts.end()) {
    return JS_DupValue(ctx, existing->second);
  }
  auto* wrapper = new CanvasContextWrapper{impl, canvas};
  JSValue object = JS_NewObjectProtoClass(ctx, impl->canvas_2d_proto, g_canvas_class_id);
  if (JS_IsException(object)) {
    delete wrapper;
    return object;
  }
  JS_SetOpaque(object, wrapper);
  impl->canvas_contexts.emplace(canvas, JS_DupValue(ctx, object));
  return object;
}

JSValue CanvasGetFillStyle(JSContext* ctx, JSValueConst this_value)
{
  CanvasContextWrapper* wrapper = UnwrapCanvasContext(this_value);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "invalid CanvasRenderingContext2D");
  }
  return JS_NewString(ctx, wrapper->fill_style_text.c_str());
}

JSValue CanvasSetFillStyle(JSContext* ctx, JSValueConst this_value, JSValueConst value)
{
  CanvasContextWrapper* wrapper = UnwrapCanvasContext(this_value);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "invalid CanvasRenderingContext2D");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::optional<css::Color> color = css::ParseColor(text);
  if (color.has_value()) {
    wrapper->fill_style = *color;
    wrapper->fill_style_text = text;
  }
  return JS_UNDEFINED;
}

JSValue CanvasFillRect(JSContext* ctx, JSValueConst this_value, int argc, JSValueConst* argv)
{
  CanvasContextWrapper* wrapper = UnwrapCanvasContext(this_value);
  if (wrapper == nullptr || wrapper->impl == nullptr || wrapper->canvas == nullptr) {
    return JS_ThrowTypeError(ctx, "invalid CanvasRenderingContext2D");
  }
  if (argc < 4) {
    return JS_UNDEFINED;
  }
  std::array<double, 4> values{};
  for (int index = 0; index < 4; ++index) {
    if (JS_ToFloat64(ctx, &values[static_cast<std::size_t>(index)], argv[index]) < 0) {
      return JS_EXCEPTION;
    }
    if (!std::isfinite(values[static_cast<std::size_t>(index)])) {
      return JS_UNDEFINED;
    }
  }
  if (wrapper->impl->apis.canvas_fill_rect) {
    const css::Color color = wrapper->fill_style;
    wrapper->impl->apis.canvas_fill_rect(*wrapper->canvas,
                                         values[0],
                                         values[1],
                                         values[2],
                                         values[3],
                                         {color.r, color.g, color.b, color.a});
  }
  return JS_UNDEFINED;
}

} // namespace

std::mutex g_canvas_class_mutex;
std::unordered_set<JSRuntime*> g_canvas_class_registered;

void EnsureCanvasClassRegistered(JSRuntime* runtime)
{
  std::lock_guard<std::mutex> lock(g_canvas_class_mutex);
  if (g_canvas_class_id == 0) {
    JS_NewClassID(runtime, &g_canvas_class_id);
  }
  if (!g_canvas_class_registered.insert(runtime).second) {
    return;
  }
  JSClassDef definition{};
  definition.class_name = "CanvasRenderingContext2D";
  definition.finalizer = CanvasFinalizer;
  JS_NewClass(runtime, g_canvas_class_id, &definition);
}

void DefineCanvasPrototype(JSContext* ctx, Impl& impl)
{
  impl.canvas_2d_proto = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx,
                    impl.html_canvas_element_proto,
                    "getContext",
                    JS_NewCFunction(ctx, CanvasGetContext, "getContext", 1));
  JS_SetPropertyStr(
      ctx, impl.canvas_2d_proto, "fillRect", JS_NewCFunction(ctx, CanvasFillRect, "fillRect", 4));
  DefineAccessor(ctx,
                 impl.canvas_2d_proto,
                 "fillStyle",
                 MakeGetter(ctx, "fillStyle", CanvasGetFillStyle),
                 MakeSetter(ctx, "fillStyle", CanvasSetFillStyle));
}

} // namespace neko::javascript