// neko::javascript DOM bindings — user-facing element surface.
//
// Part of the dom_binding split (see binding/binding_internal.h): element
// layout geometry (getBoundingClientRect / offset* / client*), form control
// properties (input/textarea/select value/checked/type/...), link and image
// URL properties, and the HTMLMediaElement (video) subset.

#include "neko/dom/element.h"
#include "neko/dom/node.h"
#include "neko/dom/query.h"
#include "neko/url/url.h"

#include "binding_internal.h"

#include <quickjs.h>
#include <string>
#include <string_view>

namespace neko::javascript {

JSValue RectToJson(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  JSValue out = JS_NewObject(ctx);
  for (const char* name : {"x", "y", "left", "top", "right", "bottom", "width", "height"}) {
    JSValue v = JS_GetPropertyStr(ctx, this_val, name);
    JS_SetPropertyStr(ctx, out, name, v); // steals v
  }
  return out;
}

// Returns the laid-out geometry of the element backing |this_val|, or nullopt
// when it is not an element, no layout exists, or the browser layer did not
// wire geometry.
std::optional<ElementGeometry> ElementGeometryOf(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return std::nullopt;
  }
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.element_geometry) {
    return std::nullopt;
  }
  return impl->apis.element_geometry(*element);
}

JSValue ElementGetBoundingClientRect(JSContext* ctx,
                                     JSValueConst this_val,
                                     int /*argc*/,
                                     JSValueConst* /*argv*/)
{
  if (AsElement(UnwrapNode(this_val)) == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // The laid-out border box in document coordinates (client coordinates when
  // the page is scrolled to the top; scroll offsets are not yet modelled).
  const std::optional<ElementGeometry> g = ElementGeometryOf(ctx, this_val);
  const double x = g ? g->x : 0;
  const double y = g ? g->y : 0;
  const double width = g ? g->width : 0;
  const double height = g ? g->height : 0;
  JSValue rect = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, rect, "x", JS_NewFloat64(ctx, x));
  JS_SetPropertyStr(ctx, rect, "y", JS_NewFloat64(ctx, y));
  JS_SetPropertyStr(ctx, rect, "left", JS_NewFloat64(ctx, x));
  JS_SetPropertyStr(ctx, rect, "top", JS_NewFloat64(ctx, y));
  JS_SetPropertyStr(ctx, rect, "right", JS_NewFloat64(ctx, x + width));
  JS_SetPropertyStr(ctx, rect, "bottom", JS_NewFloat64(ctx, y + height));
  JS_SetPropertyStr(ctx, rect, "width", JS_NewFloat64(ctx, width));
  JS_SetPropertyStr(ctx, rect, "height", JS_NewFloat64(ctx, height));
  JS_SetPropertyStr(ctx, rect, "toJSON", JS_NewCFunction(ctx, RectToJson, "toJSON", 0)); // steals
  return rect;
}

#define DEFINE_GEOMETRY_GETTER(Name, Member)                                                       \
  JSValue Name(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)        \
  {                                                                                                \
    const std::optional<ElementGeometry> g = ElementGeometryOf(ctx, this_val);                     \
    return JS_NewFloat64(ctx, g ? static_cast<double>(g->Member) : 0);                             \
  }

DEFINE_GEOMETRY_GETTER(ElementGetOffsetWidth, width)
DEFINE_GEOMETRY_GETTER(ElementGetOffsetHeight, height)
DEFINE_GEOMETRY_GETTER(ElementGetClientWidth, client_width)
DEFINE_GEOMETRY_GETTER(ElementGetClientHeight, client_height)
DEFINE_GEOMETRY_GETTER(ElementGetClientTop, border_top)
DEFINE_GEOMETRY_GETTER(ElementGetClientLeft, border_left)
DEFINE_GEOMETRY_GETTER(ElementGetOffsetTop, y)
DEFINE_GEOMETRY_GETTER(ElementGetOffsetLeft, x)

#undef DEFINE_GEOMETRY_GETTER

JSValue
ElementGetOffsetParent(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // Simplified: the <body> is the offset parent of in-flow content; positioned
  // ancestors and table cells are not yet modelled.  offsetTop/offsetLeft are
  // therefore reported in document coordinates.
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_NULL;
  }
  dom::Element* body = nullptr;
  if (impl->document.document_element() != nullptr) {
    body = dom::QuerySelector(*impl->document.document_element(), "body");
  }
  if (body == nullptr || body == element) {
    return JS_NULL;
  }
  return impl->WrapNode(body);
}

std::string RawAttr(const dom::Element& element, std::string_view name)
{
  const std::optional<std::string_view> value = element.GetAttribute(name);
  return value.has_value() ? std::string(value.value()) : std::string();
}

// The absolute URL for an href/src attribute (resolved against the page base
// via the PageApis callback when wired; otherwise the raw attribute).
std::string ResolvedUrl(const Impl& impl, const std::string& raw)
{
  if (impl.apis.resolve_url && !raw.empty()) {
    const std::string resolved = impl.apis.resolve_url(raw);
    if (!resolved.empty()) {
      return resolved;
    }
  }
  return raw;
}

[[maybe_unused]] bool IsFormControl(const dom::Element& element)
{
  const std::string_view tag = element.tag_name();
  return tag == "input" || tag == "textarea" || tag == "select" || tag == "option" ||
         tag == "button";
}

bool IsCheckableInput(const dom::Element& element)
{
  if (element.tag_name() != "input") {
    return false;
  }
  const std::string type = ToLower(RawAttr(element, "type"));
  return type == "checkbox" || type == "radio";
}

JSValue ElementGetValue(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string_view tag = element->tag_name();
  if (tag == "textarea") {
    // value == the text content (textContent; <br> not meaningful here).
    const std::string text = element->TextContent();
    return JS_NewStringLen(ctx, text.data(), text.size());
  }
  if (tag == "option") {
    // option.value: the value attribute, else the text content.
    const std::string attr = RawAttr(*element, "value");
    if (!attr.empty()) {
      return JS_NewStringLen(ctx, attr.data(), attr.size());
    }
    const std::string text = element->TextContent();
    return JS_NewStringLen(ctx, text.data(), text.size());
  }
  const std::string value = RawAttr(*element, "value");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetValue(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (element->tag_name() == "textarea") {
    // Setting value on a textarea replaces its text child.  RemoveChild
    // returns ownership; keep the removed subtree alive in the binder's
    // retained set so JS references to the old children do not dangle.
    Impl* impl = ImplFor(ctx, this_val);
    while (element->first_child() != nullptr) {
      dom::Node* child = element->first_child();
      std::unique_ptr<dom::Node> removed = element->RemoveChild(child);
      if (impl != nullptr) {
        impl->TakeOwnership(child, std::move(removed));
      }
    }
    if (!text.empty()) {
      element->AppendChild(std::make_unique<dom::Text>(text));
    }
  } else {
    element->SetAttribute("value", text);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetChecked(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr || !IsCheckableInput(*element)) {
    return JS_NewBool(ctx, false);
  }
  return JS_NewBool(ctx, element->HasAttribute("checked"));
}

JSValue ElementSetChecked(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr || !IsCheckableInput(*element)) {
    return JS_UNDEFINED;
  }
  const int v = JS_ToBool(ctx, value);
  if (v < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }
  if (v != 0) {
    element->SetAttribute("checked", "checked");
  } else {
    element->RemoveAttribute("checked");
  }
  return JS_UNDEFINED;
}

JSValue ElementGetType(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (element->tag_name() != "input") {
    return JS_NewStringLen(ctx, "", 0);
  }
  const std::string type = RawAttr(*element, "type");
  return JS_NewStringLen(ctx, type.data(), type.size());
}

JSValue ElementSetType(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string type = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (element->tag_name() == "input") {
    element->SetAttribute("type", type);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetPlaceholder(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string value = RawAttr(*element, "placeholder");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetPlaceholder(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("placeholder", text);
  return JS_UNDEFINED;
}

JSValue ElementGetDisabled(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewBool(ctx, element->HasAttribute("disabled"));
}

JSValue ElementSetDisabled(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const int v = JS_ToBool(ctx, value);
  if (v < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }
  if (v != 0) {
    element->SetAttribute("disabled", "disabled");
  } else {
    element->RemoveAttribute("disabled");
  }
  return JS_UNDEFINED;
}

JSValue ElementGetName(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string value = RawAttr(*element, "name");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetName(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("name", text);
  return JS_UNDEFINED;
}

JSValue ElementGetHref(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string raw = RawAttr(*element, "href");
  const std::string resolved = ResolvedUrl(*impl, raw);
  return JS_NewStringLen(ctx, resolved.data(), resolved.size());
}

JSValue ElementGetAnchorUrlPart(JSContext* ctx, JSValueConst this_val, int magic)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const base::Result<url::Url> parsed =
      url::Url::Parse(ResolvedUrl(*impl, RawAttr(*element, "href")));
  if (!parsed.has_value()) {
    return JS_NewStringLen(ctx, "", 0);
  }

  const url::Url& parsed_url = parsed.value();
  std::string value;
  switch (static_cast<AnchorUrlPart>(magic)) {
  case AnchorUrlPart::kProtocol:
    value = parsed_url.scheme().empty() ? "" : parsed_url.scheme() + ":";
    break;
  case AnchorUrlPart::kHost:
    value = parsed_url.host();
    if (parsed_url.port().has_value()) {
      value += ":" + std::to_string(parsed_url.port().value());
    }
    break;
  case AnchorUrlPart::kHostname:
    value = parsed_url.host();
    break;
  case AnchorUrlPart::kPort:
    value = parsed_url.port().has_value() ? std::to_string(parsed_url.port().value()) : "";
    break;
  case AnchorUrlPart::kPathname:
    value = parsed_url.path().empty() ? "/" : parsed_url.path();
    break;
  case AnchorUrlPart::kSearch:
    value = parsed_url.has_query() ? "?" + parsed_url.query() : "";
    break;
  case AnchorUrlPart::kHash:
    value = parsed_url.has_fragment() ? "#" + parsed_url.fragment() : "";
    break;
  }
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetHref(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string href = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("href", href);
  return JS_UNDEFINED;
}

JSValue ElementGetTarget(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string value = RawAttr(*element, "target");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetTarget(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("target", text);
  return JS_UNDEFINED;
}

JSValue ElementGetRel(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string value = RawAttr(*element, "rel");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetRel(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("rel", text);
  return JS_UNDEFINED;
}

JSValue ElementGetSrc(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string raw = RawAttr(*element, "src");
  const std::string resolved = ResolvedUrl(*impl, raw);
  return JS_NewStringLen(ctx, resolved.data(), resolved.size());
}

JSValue ElementSetSrc(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string src = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("src", src);
  return JS_UNDEFINED;
}

JSValue ElementGetAlt(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string value = RawAttr(*element, "alt");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetAlt(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("alt", text);
  return JS_UNDEFINED;
}

// Parses a non-negative integer attribute (for img width/height).
int64_t PositiveIntAttr(const dom::Element& element, std::string_view name)
{
  const std::string value = RawAttr(element, name);
  if (value.empty()) {
    return 0;
  }
  int64_t out = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') {
      return 0;
    }
    out = out * 10 + (c - '0');
  }
  return out;
}

JSValue ElementGetWidth(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewInt64(ctx, PositiveIntAttr(*element, "width"));
}

JSValue ElementSetWidth(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  int32_t v = 0;
  if (JS_ToInt32(ctx, &v, value) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }
  if (v < 0) {
    v = 0;
  }
  element->SetAttribute("width", std::to_string(v));
  return JS_UNDEFINED;
}

JSValue ElementGetHeight(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewInt64(ctx, PositiveIntAttr(*element, "height"));
}

JSValue ElementSetHeight(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  int32_t v = 0;
  if (JS_ToInt32(ctx, &v, value) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }
  if (v < 0) {
    v = 0;
  }
  element->SetAttribute("height", std::to_string(v));
  return JS_UNDEFINED;
}

JSValue ElementGetNaturalWidth(JSContext* ctx, JSValueConst this_val)
{
  if (AsElement(UnwrapNode(this_val)) == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // The binder does not track decoded image dimensions; 0 (a documented
  // approximation — scripts that check naturalWidth before drawing a layout
  // still branch correctly).
  return JS_NewInt32(ctx, 0);
}

JSValue ElementGetNaturalHeight(JSContext* ctx, JSValueConst this_val)
{
  if (AsElement(UnwrapNode(this_val)) == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewInt32(ctx, 0);
}

JSValue ElementGetComplete(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // complete: true once a src is present (loading is synchronous here).
  return JS_NewBool(ctx, element->HasAttribute("src"));
}

JSValue ElementGetCurrentSrc(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string raw = RawAttr(*element, "src");
  const std::string resolved = ResolvedUrl(*impl, raw);
  return JS_NewStringLen(ctx, resolved.data(), resolved.size());
}

// ---- HTMLMediaElement (video) ----------------------------------------------

bool IsVideoElement(const dom::Element* element)
{
  return element != nullptr && element->tag_name() == "video";
}

JSValue
ElementPlayVideo(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (!IsVideoElement(element)) {
    return JS_ThrowTypeError(ctx, "play() is only defined on <video>");
  }
  if (!impl->apis.video_play) {
    return JS_ThrowTypeError(ctx, "video playback is not available");
  }
  impl->apis.video_play(*element);
  return JS_UNDEFINED;
}

JSValue
ElementPauseVideo(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (!IsVideoElement(element)) {
    return JS_ThrowTypeError(ctx, "pause() is only defined on <video>");
  }
  if (impl->apis.video_pause) {
    impl->apis.video_pause(*element);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetVideoDuration(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr || !IsVideoElement(element) ||
      !impl->apis.video_duration) {
    return JS_NewFloat64(ctx, std::nan(""));
  }
  const std::optional<double> duration = impl->apis.video_duration(*element);
  return JS_NewFloat64(ctx, duration.value_or(std::nan("")));
}

JSValue ElementGetVideoCurrentTime(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr || !IsVideoElement(element) ||
      !impl->apis.video_current_time) {
    return JS_NewFloat64(ctx, 0);
  }
  return JS_NewFloat64(ctx, impl->apis.video_current_time(*element).value_or(0));
}

JSValue ElementSetVideoCurrentTime(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr || !IsVideoElement(element) || !impl->apis.video_seek) {
    return JS_UNDEFINED;
  }
  double seconds = 0;
  if (JS_ToFloat64(ctx, &seconds, value) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_UNDEFINED;
  }
  impl->apis.video_seek(*element, seconds);
  return JS_UNDEFINED;
}

JSValue ElementGetVideoPaused(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr || !IsVideoElement(element) ||
      !impl->apis.video_paused) {
    return JS_NewBool(ctx, 1);
  }
  return JS_NewBool(ctx, impl->apis.video_paused(*element) ? 1 : 0);
}

[[maybe_unused]] JSValue ElementGetText(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (element->tag_name() != "option") {
    return JS_NewStringLen(ctx, "", 0);
  }
  const std::string text = element->TextContent();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

// ---------------------------------------------------------------------------
// classList (DOMTokenList subset).
//
// A classList object is backed by the element's class attribute.  add/remove/
// toggle/contains/replace operate on the whitespace-separated token set and
// persist to the attribute; item/length/toString expose the current tokens.
// ---------------------------------------------------------------------------

} // namespace neko::javascript
