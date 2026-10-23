// neko::javascript DOM bindings — user-facing element surface.
//
// Part of the dom_binding split (see binding/binding_internal.h): element
// layout geometry (getBoundingClientRect / offset* / client*), form control
// properties (input/textarea/select value/checked/type/...), link and image
// URL properties, and the HTMLMediaElement (video) subset.

#include "neko/dom/element.h"
#include "neko/dom/node.h"
#include "neko/dom/query.h"
#include "neko/html/form_validation.h"
#include "neko/html/input_value.h"
#include "neko/url/url.h"

#include "binding_internal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
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
  if (AsElement(UnwrapNode(ctx, this_val)) == nullptr) {
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

// The document scrolling element (documentElement or <body>) drives the page's
// scroll offset; any other element has no scrollable overflow (documented) and
// reports 0 / no-ops.
bool IsDocumentScrollingElement(const dom::Element* element)
{
  if (element == nullptr) {
    return false;
  }
  const std::string_view tag = element->tag_name();
  return tag == "html" || tag == "body";
}

JSValue ElementGetScrollTop(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (!IsDocumentScrollingElement(element) || !impl->apis.scroll_offset) {
    return JS_NewInt32(ctx, 0);
  }
  return JS_NewFloat64(ctx, impl->apis.scroll_offset().second);
}

JSValue ElementSetScrollTop(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (!IsDocumentScrollingElement(element) || !impl->apis.scroll_to) {
    return JS_UNDEFINED;
  }
  double top = 0;
  if (JS_ToFloat64(ctx, &top, value) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }
  const auto current = impl->apis.scroll_offset();
  impl->apis.scroll_to(current.first, top);
  return JS_UNDEFINED;
}

JSValue ElementGetScrollLeft(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (!IsDocumentScrollingElement(element) || !impl->apis.scroll_offset) {
    return JS_NewInt32(ctx, 0);
  }
  return JS_NewFloat64(ctx, impl->apis.scroll_offset().first);
}

JSValue ElementSetScrollLeft(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (!IsDocumentScrollingElement(element) || !impl->apis.scroll_to) {
    return JS_UNDEFINED;
  }
  double left = 0;
  if (JS_ToFloat64(ctx, &left, value) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }
  const auto current = impl->apis.scroll_offset();
  impl->apis.scroll_to(left, current.second);
  return JS_UNDEFINED;
}

JSValue ElementGetScrollWidth(JSContext* ctx, JSValueConst this_val)
{
  const std::optional<ElementGeometry> g = ElementGeometryOf(ctx, this_val);
  return JS_NewFloat64(ctx, g ? static_cast<double>(g->client_width) : 0);
}

JSValue ElementGetScrollHeight(JSContext* ctx, JSValueConst this_val)
{
  const std::optional<ElementGeometry> g = ElementGeometryOf(ctx, this_val);
  return JS_NewFloat64(ctx, g ? static_cast<double>(g->client_height) : 0);
}

JSValue
ElementGetOffsetParent(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
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

namespace {

// Writes a text control's value.  <textarea> stores its text child; <input>
// goes through neko::html::SetInputValue, which applies the per-state value
// sanitization algorithm and sets the dirty value flag (the `value` content
// attribute -- the default value -- is left untouched).  Removed textarea
// children are retained by the binder so live JS references to them do not
// dangle.
void StoreTextControlValue(JSContext* ctx,
                           JSValueConst this_val,
                           dom::Element& element,
                           std::string_view text)
{
  if (element.tag_name() == "textarea") {
    Impl* impl = ImplFor(ctx, this_val);
    while (element.first_child() != nullptr) {
      dom::Node* child = element.first_child();
      std::unique_ptr<dom::Node> removed = element.RemoveChild(child);
      if (impl != nullptr) {
        impl->TakeOwnership(child, std::move(removed));
      }
    }
    if (!text.empty()) {
      element.AppendChild(std::make_unique<dom::Text>(std::string(text)));
    }
  } else if (element.tag_name() == "input") {
    html::SetInputValue(element, text);
  } else {
    element.SetAttribute("value", text);
  }
  if (Impl* impl = ImplFor(ctx, this_val)) {
    impl->MarkDomDirty(&element);
  }
}

// Clamps an existing selection after the relevant value shrank (WHATWG HTML
// §4.10.20 "whenever the relevant value changes").
void ClampTextControlSelection(dom::Element& element)
{
  dom::TextControlState* state = element.text_control_state();
  if (state == nullptr) {
    return;
  }
  const std::size_t length = dom::Utf16Length(html::InputValue(element));
  state->selection_start = std::min(state->selection_start, length);
  state->selection_end = std::min(state->selection_end, length);
}

} // namespace

JSValue ElementGetValue(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string_view tag = element->tag_name();
  if (tag == "textarea") {
    // value == the text content (textContent; <br> not meaningful here).
    const std::string text = dom::TextControlValue(*element);
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
  if (tag == "input") {
    // The value IDL attribute: the sanitized current value (per-state value
    // sanitization algorithm), not the raw `value` content attribute.
    const std::string value = html::InputValue(*element);
    return JS_NewStringLen(ctx, value.data(), value.size());
  }
  const std::string value = RawAttr(*element, "value");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementGetDefaultValue(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // defaultValue is the raw `value` content attribute ("" when absent), as
  // for every input type.
  const std::string value = RawAttr(*element, "value");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetDefaultValue(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (element->tag_name() == "input") {
    // Sets the value content attribute; when the dirty value flag is false the
    // current value follows it (HTML §4.10.5.5).
    html::SetInputDefaultValue(*element, text);
  } else {
    element->SetAttribute("value", text);
  }
  if (Impl* impl = ImplFor(ctx, this_val)) {
    impl->MarkDomDirty(element);
  }
  return JS_UNDEFINED;
}

JSValue ElementSetValue(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (element->tag_name() == "input" &&
      html::InputValueModeFor(html::InputTypeKeyword(*element)) ==
          html::InputValueMode::kFilename &&
      !text.empty()) {
    // HTML §4.10.5.1.7 File state: "On setting, if the new value is the empty
    // string, empty the list of selected files; otherwise, throw an
    // InvalidStateError exception."  The engine has no File API, so only the
    // empty value is ever accepted.
    return ThrowDomException(
        ctx, "InvalidStateError", "a file input's value can only be set to the empty string");
  }
  StoreTextControlValue(ctx, this_val, *element, text);
  if (dom::TextControlState* state = element->text_control_state()) {
    state->dirty_value = true; // the script value setter sets the dirty flag
  }
  ClampTextControlSelection(*element);
  return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Text control selection APIs (WHATWG HTML §4.10.20)
// ---------------------------------------------------------------------------
//
// The state lives on dom::Element (TextControlState), so a selection created
// here is the same selection the browser's keyboard editing uses.  Getters
// return null for input types to which the APIs do not apply, exactly as the
// standard specifies; setters, setSelectionRange() and setRangeText() throw
// InvalidStateError for those types.  select() is a no-op there (the spec
// returns early rather than throwing).

namespace {

dom::Element* TextControlReceiver(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    JS_ThrowTypeError(ctx, "not an element");
  }
  return element;
}

JSValue SelectionDoesNotApply(JSContext* ctx)
{
  return ThrowDomException(
      ctx, "InvalidStateError", "the selection APIs do not apply to this input type");
}

bool ToOffset(JSContext* ctx, JSValueConst value, double* out)
{
  double number = 0;
  if (JS_ToFloat64(ctx, &number, value) != 0) {
    return false;
  }
  if (std::isnan(number) || number < 0) {
    number = 0;
  }
  *out = number;
  return true;
}

std::size_t ClampToLength(double value, std::size_t length)
{
  if (!(value > 0)) {
    return 0;
  }
  if (value >= static_cast<double>(length)) {
    return length;
  }
  return static_cast<std::size_t>(value);
}

dom::SelectionDirection DirectionFromString(std::string_view text)
{
  if (text == "forward") {
    return dom::SelectionDirection::kForward;
  }
  if (text == "backward") {
    return dom::SelectionDirection::kBackward;
  }
  return dom::SelectionDirection::kNone;
}

const char* DirectionName(dom::SelectionDirection direction)
{
  switch (direction) {
  case dom::SelectionDirection::kForward:
    return "forward";
  case dom::SelectionDirection::kBackward:
    return "backward";
  case dom::SelectionDirection::kNone:
  default:
    return "none";
  }
}

std::size_t CurrentSelectionStart(const dom::Element& element, std::size_t length)
{
  const dom::TextControlState* state = element.text_control_state();
  return state != nullptr ? std::min(state->selection_start, length) : 0;
}

std::size_t CurrentSelectionEnd(const dom::Element& element, std::size_t length)
{
  const dom::TextControlState* state = element.text_control_state();
  return state != nullptr ? std::min(state->selection_end, length) : 0;
}

dom::SelectionDirection CurrentSelectionDirection(const dom::Element& element)
{
  const dom::TextControlState* state = element.text_control_state();
  return state != nullptr ? state->selection_direction : dom::SelectionDirection::kNone;
}

// The spec's "set the selection range" algorithm: clamp both ends, collapse an
// inverted/empty range immediately before the character at |end|, and record
// the direction.  Returns whether extent or direction changed (the trigger for
// the select event).
bool ApplySelection(dom::Element& element,
                    std::size_t start,
                    std::size_t end,
                    dom::SelectionDirection direction)
{
  const std::size_t length = dom::Utf16Length(html::InputValue(element));
  start = std::min(start, length);
  end = std::min(end, length);
  if (end <= start) {
    start = end;
  }
  dom::TextControlState& state = element.EnsureTextControlState();
  const bool changed = state.selection_start != start || state.selection_end != end ||
                       state.selection_direction != direction;
  state.selection_start = start;
  state.selection_end = end;
  state.selection_direction = direction;
  return changed;
}

// Queues (here: synchronously dispatches) the bubbling select event the spec
// requires when setSelectionRange()/select() changes the selection.  The
// task-queue delay is not modelled.
void FireSelectEvent(JSContext* ctx, JSValueConst this_val, dom::Element& element)
{
  if (Impl* impl = ImplFor(ctx, this_val)) {
    impl->DispatchEvent(element, "select");
  }
}

} // namespace

JSValue ElementGetSelectionStart(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = TextControlReceiver(ctx, this_val);
  if (element == nullptr) {
    return JS_EXCEPTION;
  }
  if (!dom::SupportsTextControlSelection(*element)) {
    return JS_NULL;
  }
  const std::size_t length = dom::Utf16Length(html::InputValue(*element));
  return JS_NewInt64(ctx, static_cast<std::int64_t>(CurrentSelectionStart(*element, length)));
}

JSValue ElementSetSelectionStart(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = TextControlReceiver(ctx, this_val);
  if (element == nullptr) {
    return JS_EXCEPTION;
  }
  if (!dom::SupportsTextControlSelection(*element)) {
    return SelectionDoesNotApply(ctx);
  }
  double start = 0;
  if (!ToOffset(ctx, value, &start)) {
    return JS_EXCEPTION;
  }
  const std::size_t length = dom::Utf16Length(html::InputValue(*element));
  const std::size_t current_end = CurrentSelectionEnd(*element, length);
  // "If end is less than the given value, set end to the given value."
  const double end =
      static_cast<double>(current_end) < start ? start : static_cast<double>(current_end);
  const bool changed = ApplySelection(*element,
                                      ClampToLength(start, length),
                                      ClampToLength(end, length),
                                      CurrentSelectionDirection(*element));
  if (changed) {
    FireSelectEvent(ctx, this_val, *element);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetSelectionEnd(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = TextControlReceiver(ctx, this_val);
  if (element == nullptr) {
    return JS_EXCEPTION;
  }
  if (!dom::SupportsTextControlSelection(*element)) {
    return JS_NULL;
  }
  const std::size_t length = dom::Utf16Length(html::InputValue(*element));
  return JS_NewInt64(ctx, static_cast<std::int64_t>(CurrentSelectionEnd(*element, length)));
}

JSValue ElementSetSelectionEnd(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = TextControlReceiver(ctx, this_val);
  if (element == nullptr) {
    return JS_EXCEPTION;
  }
  if (!dom::SupportsTextControlSelection(*element)) {
    return SelectionDoesNotApply(ctx);
  }
  double end = 0;
  if (!ToOffset(ctx, value, &end)) {
    return JS_EXCEPTION;
  }
  const std::size_t length = dom::Utf16Length(html::InputValue(*element));
  const bool changed = ApplySelection(*element,
                                      CurrentSelectionStart(*element, length),
                                      ClampToLength(end, length),
                                      CurrentSelectionDirection(*element));
  if (changed) {
    FireSelectEvent(ctx, this_val, *element);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetSelectionDirection(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = TextControlReceiver(ctx, this_val);
  if (element == nullptr) {
    return JS_EXCEPTION;
  }
  if (!dom::SupportsTextControlSelection(*element)) {
    return JS_NULL;
  }
  return JS_NewString(ctx, DirectionName(CurrentSelectionDirection(*element)));
}

JSValue ElementSetSelectionDirection(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = TextControlReceiver(ctx, this_val);
  if (element == nullptr) {
    return JS_EXCEPTION;
  }
  if (!dom::SupportsTextControlSelection(*element)) {
    return SelectionDoesNotApply(ctx);
  }
  bool ok = false;
  const std::string direction = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::size_t length = dom::Utf16Length(html::InputValue(*element));
  const bool changed = ApplySelection(*element,
                                      CurrentSelectionStart(*element, length),
                                      CurrentSelectionEnd(*element, length),
                                      DirectionFromString(direction));
  if (changed) {
    FireSelectEvent(ctx, this_val, *element);
  }
  return JS_UNDEFINED;
}

JSValue
ElementSetSelectionRange(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = TextControlReceiver(ctx, this_val);
  if (element == nullptr) {
    return JS_EXCEPTION;
  }
  if (!dom::SupportsTextControlSelection(*element)) {
    return SelectionDoesNotApply(ctx);
  }
  double start = 0;
  double end = 0;
  dom::SelectionDirection direction = dom::SelectionDirection::kNone;
  if (argc > 0 && !ToOffset(ctx, argv[0], &start)) {
    return JS_EXCEPTION;
  }
  if (argc > 1 && !ToOffset(ctx, argv[1], &end)) {
    return JS_EXCEPTION;
  }
  if (argc > 2) {
    bool ok = false;
    const std::string text = ArgString(ctx, argv[2], &ok);
    if (!ok) {
      return JS_EXCEPTION;
    }
    // Any value other than "forward"/"backward" (including an omitted
    // argument) means "none", per the spec's set-the-selection-range steps.
    direction = DirectionFromString(text);
  }
  const std::size_t length = dom::Utf16Length(html::InputValue(*element));
  const bool changed =
      ApplySelection(*element, ClampToLength(start, length), ClampToLength(end, length), direction);
  if (changed) {
    FireSelectEvent(ctx, this_val, *element);
  }
  return JS_UNDEFINED;
}

JSValue ElementSetRangeText(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = TextControlReceiver(ctx, this_val);
  if (element == nullptr) {
    return JS_EXCEPTION;
  }
  if (!dom::SupportsTextControlSelection(*element)) {
    return SelectionDoesNotApply(ctx);
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "setRangeText requires a replacement argument");
  }
  bool ok = false;
  const std::string replacement = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  // The three-argument form is replacement, start, end; passing start without
  // end is a WebIDL arity error.
  const bool one_argument = argc < 2;
  if (!one_argument && argc < 3) {
    return JS_ThrowTypeError(ctx, "setRangeText requires both start and end");
  }
  std::string mode = "preserve";
  if (argc > 3) {
    mode = ArgString(ctx, argv[3], &ok);
    if (!ok) {
      return JS_EXCEPTION;
    }
    if (mode != "select" && mode != "start" && mode != "end" && mode != "preserve") {
      return JS_ThrowTypeError(ctx, "invalid selectionMode for setRangeText");
    }
  }

  std::string value = html::InputValue(*element);
  const std::size_t length = dom::Utf16Length(value);
  const std::size_t selection_start = CurrentSelectionStart(*element, length);
  const std::size_t selection_end = CurrentSelectionEnd(*element, length);
  std::size_t start = selection_start;
  std::size_t end = selection_end;
  if (!one_argument) {
    double start_number = 0;
    double end_number = 0;
    if (!ToOffset(ctx, argv[1], &start_number) || !ToOffset(ctx, argv[2], &end_number)) {
      return JS_EXCEPTION;
    }
    if (start_number > end_number) {
      return ThrowDomException(ctx, "IndexSizeError", "setRangeText: start is greater than end");
    }
    start = ClampToLength(start_number, length);
    end = ClampToLength(end_number, length);
  }

  dom::TextControlState& state = element->EnsureTextControlState();
  state.dirty_value = true; // setRangeText always sets the dirty value flag

  const std::size_t new_length = dom::Utf16Length(replacement);
  const std::string updated = dom::ReplaceUtf16Range(value, start, end, replacement);
  std::size_t new_selection_start = selection_start;
  std::size_t new_selection_end = selection_end;
  if (mode == "select") {
    new_selection_start = start;
    new_selection_end = start + new_length;
  } else if (mode == "start") {
    new_selection_start = start;
    new_selection_end = start;
  } else if (mode == "end") {
    new_selection_start = start + new_length;
    new_selection_end = start + new_length;
  } else if (mode == "preserve" && one_argument) {
    // Shift the current selection when it lies after the replaced range; snap
    // it into the new text when it overlapped the range (spec subtitle).
    const std::ptrdiff_t delta =
        static_cast<std::ptrdiff_t>(new_length) - static_cast<std::ptrdiff_t>(end - start);
    if (new_selection_start > end) {
      new_selection_start =
          static_cast<std::size_t>(static_cast<std::ptrdiff_t>(new_selection_start) + delta);
    } else if (new_selection_start > start) {
      new_selection_start = start;
    }
    if (new_selection_end > end) {
      new_selection_end =
          static_cast<std::size_t>(static_cast<std::ptrdiff_t>(new_selection_end) + delta);
    } else if (new_selection_end > start) {
      new_selection_end = start + new_length;
    }
  }

  StoreTextControlValue(ctx, this_val, *element, updated);
  const bool changed = ApplySelection(
      *element, new_selection_start, new_selection_end, dom::SelectionDirection::kNone);
  if (changed) {
    FireSelectEvent(ctx, this_val, *element);
  }
  return JS_UNDEFINED;
}

JSValue ElementSelect(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  dom::Element* element = TextControlReceiver(ctx, this_val);
  if (element == nullptr) {
    return JS_EXCEPTION;
  }
  // select() returns early (without throwing) when it does not apply.
  if (!dom::SupportsTextControlSelection(*element)) {
    return JS_UNDEFINED;
  }
  const std::size_t length = dom::Utf16Length(html::InputValue(*element));
  const bool changed = ApplySelection(*element, 0, length, dom::SelectionDirection::kNone);
  if (changed) {
    FireSelectEvent(ctx, this_val, *element);
  }
  // Browsers focus an <input> when select() is called; textarea focus is left
  // to the caller (the standard's select() steps only set the range).
  if (element->tag_name() == "input") {
    const JSValue result = ElementFocus(ctx, this_val, 0, nullptr);
    if (JS_IsException(result)) {
      return result;
    }
  }
  return JS_UNDEFINED;
}

namespace {

JSValue MakeValidityStateObject(JSContext* ctx, Impl& impl, dom::Element& element)
{
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue constructor = JS_GetPropertyStr(ctx, global, "ValidityState");
  JS_FreeValue(ctx, global);
  JSValue prototype = JS_GetPropertyStr(ctx, constructor, "prototype");
  JS_FreeValue(ctx, constructor);
  JSValue validity = JS_NewObjectProto(ctx, prototype);
  JS_FreeValue(ctx, prototype);
  // The state is computed on every getter access from the live DOM (a stored
  // `input.validity` object stays live, as in browsers), so the object only
  // needs to remember which element it describes.  The link is non-enumerable.
  JS_DefinePropertyValueStr(
      ctx, validity, "__nekoValidityElement", impl.WrapNode(&element), JS_PROP_CONFIGURABLE);
  return validity;
}

// Returns the element whose validity is exposed by |this_val|, or null when
// the receiver is not a ValidityState created by ElementGetValidity.
dom::Element* ValidityElementOf(JSContext* ctx, JSValueConst this_val)
{
  JSValue element_value = JS_GetPropertyStr(ctx, this_val, "__nekoValidityElement");
  dom::Element* element = AsElement(UnwrapNode(ctx, element_value));
  JS_FreeValue(ctx, element_value);
  return element;
}

bool ValidityFlagValue(const html::ValidityState& state, int magic)
{
  switch (magic) {
  case kValidityValueMissing:
    return state.value_missing;
  case kValidityTypeMismatch:
    return state.type_mismatch;
  case kValidityPatternMismatch:
    return state.pattern_mismatch;
  case kValidityTooLong:
    return state.too_long;
  case kValidityTooShort:
    return state.too_short;
  case kValidityRangeUnderflow:
    return state.range_underflow;
  case kValidityRangeOverflow:
    return state.range_overflow;
  case kValidityStepMismatch:
    return state.step_mismatch;
  case kValidityBadInput:
    return state.bad_input;
  case kValidityCustomError:
    return state.custom_error;
  case kValidityValid:
    return state.valid;
  default:
    return false;
  }
}

} // namespace

JSValue ElementGetValidity(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  Impl* impl = ImplFor(ctx, this_val);
  if (element == nullptr || impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (!html::IsSubmittableControl(*element)) {
    return JS_ThrowTypeError(ctx, "not a form control");
  }
  return MakeValidityStateObject(ctx, *impl, *element);
}

JSValue ValidityStateGetFlag(JSContext* ctx, JSValueConst this_val, int magic)
{
  if (magic < 0 || magic >= kValidityFlagCount) {
    return JS_ThrowInternalError(ctx, "bad ValidityState flag");
  }
  dom::Element* element = ValidityElementOf(ctx, this_val);
  Impl* impl = ImplFor(ctx, this_val);
  if (element == nullptr || impl == nullptr) {
    return JS_NewBool(ctx, false);
  }
  html::ValidityState state;
  if (element->tag_name() == "form") {
    // HTMLFormElement has no standard `validity`; the engine exposes an
    // aggregate (all constraint flags false, valid = no invalid controls).
    state.valid = impl->InvalidFormControls(*element).empty();
  } else {
    state = html::ValidateControl(*element, impl->CustomValidityMessage(*element)).state;
  }
  return JS_NewBool(ctx, ValidityFlagValue(state, magic));
}

JSValue ElementGetWillValidate(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr || !html::IsSubmittableControl(*element)) {
    return JS_ThrowTypeError(ctx, "not a form control");
  }
  return JS_NewBool(ctx, html::IsCandidateForConstraintValidation(*element));
}

JSValue ElementGetValidationMessage(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  Impl* impl = ImplFor(ctx, this_val);
  if (element == nullptr || impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (element->tag_name() == "form") {
    // Non-standard aggregate: the first invalid control's message.
    for (dom::Element* control : impl->InvalidFormControls(*element)) {
      const html::ControlValidity validity =
          html::ValidateControl(*control, impl->CustomValidityMessage(*control));
      if (!validity.message.empty()) {
        return JS_NewStringLen(ctx, validity.message.data(), validity.message.size());
      }
    }
    return JS_NewStringLen(ctx, "", 0);
  }
  if (!html::IsSubmittableControl(*element)) {
    return JS_ThrowTypeError(ctx, "not a form control");
  }
  const html::ControlValidity validity =
      html::ValidateControl(*element, impl->CustomValidityMessage(*element));
  return JS_NewStringLen(ctx, validity.message.data(), validity.message.size());
}

JSValue
ElementCheckValidity(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  Impl* impl = ImplFor(ctx, this_val);
  if (element == nullptr || impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (element->tag_name() == "form") {
    return JS_NewBool(ctx, impl->CheckFormValidity(*element));
  }
  if (!html::IsSubmittableControl(*element)) {
    return JS_ThrowTypeError(ctx, "not a form control");
  }
  return JS_NewBool(ctx, impl->CheckControlValidity(*element));
}

JSValue ElementReportValidity(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  // PARTIALLY IMPLEMENTED: reportValidity() runs the same constraint check
  // (and fires the invalid events) but does not move focus to the first
  // invalid control, as interactive validation requires (HTML §4.10.21.3).
  // The binder has Element.focus() now, but this path still leaves focus to
  // the browser layer; the browser's form-submission path does focus the
  // first invalid control.
  return ElementCheckValidity(ctx, this_val, argc, argv);
}

JSValue
ElementSetCustomValidity(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  Impl* impl = ImplFor(ctx, this_val);
  if (element == nullptr || impl == nullptr || !html::IsSubmittableControl(*element)) {
    return JS_ThrowTypeError(ctx, "not a form control");
  }
  bool ok = true;
  const std::string message = argc > 0 ? ArgString(ctx, argv[0], &ok) : std::string();
  if (!ok) {
    return JS_EXCEPTION;
  }
  impl->SetCustomValidity(*element, message);
  return JS_UNDEFINED;
}

JSValue ElementGetForm(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  Impl* impl = ImplFor(ctx, this_val);
  if (element == nullptr || impl == nullptr || !html::IsSubmittableControl(*element)) {
    return JS_ThrowTypeError(ctx, "not a form control");
  }
  dom::Element* form = html::OwnerForm(*element);
  return form != nullptr ? impl->WrapNode(form) : JS_NULL;
}

JSValue
FormCheckValidity(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  Impl* impl = ImplFor(ctx, this_val);
  if (element == nullptr || impl == nullptr || element->tag_name() != "form") {
    return JS_ThrowTypeError(ctx, "not a form element");
  }
  return JS_NewBool(ctx, impl->CheckFormValidity(*element));
}

JSValue FormReportValidity(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  // Same focus limitation as ElementReportValidity above: the invalid events
  // fire, but focus does not move to the first invalid control.
  return FormCheckValidity(ctx, this_val, argc, argv);
}

JSValue FormGetNoValidate(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr || element->tag_name() != "form") {
    return JS_ThrowTypeError(ctx, "not a form element");
  }
  return JS_NewBool(ctx, element->HasAttribute("novalidate"));
}

JSValue FormSetNoValidate(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr || element->tag_name() != "form") {
    return JS_ThrowTypeError(ctx, "not a form element");
  }
  const int enabled = JS_ToBool(ctx, value);
  if (enabled < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }
  if (enabled != 0) {
    element->SetAttribute("novalidate", "novalidate");
  } else {
    element->RemoveAttribute("novalidate");
  }
  // novalidate has no cascade/layout effect, so the dirty flags stay clear.
  return JS_UNDEFINED;
}

std::string Impl::CustomValidityMessage(const dom::Element& element) const
{
  const auto it = custom_validity.find(&element);
  return it != custom_validity.end() ? it->second : std::string();
}

void Impl::SetCustomValidity(dom::Element& element, std::string message)
{
  if (message.empty()) {
    custom_validity.erase(&element);
  } else {
    custom_validity[&element] = std::move(message);
  }
}

void Impl::DispatchInvalidToNode(dom::Node* node)
{
  if (node == nullptr) {
    return;
  }
  // The `invalid` event is cancelable but does not bubble (WHATWG HTML
  // §4.10.21.3): listeners on the control run, ancestors do not.
  JSValue event = MakeEvent("invalid", /*bubbles=*/false, /*cancelable=*/true);
  (void)DispatchPropagated(node, event);
  JS_FreeValue(ctx, event);
  engine.RunPendingJobs();
}

std::vector<dom::Element*> Impl::InvalidFormControls(dom::Element& form) const
{
  return html::InvalidControls(
      form, [this](const dom::Element& element) { return CustomValidityMessage(element); });
}

bool Impl::CheckControlValidity(dom::Element& element)
{
  const html::ControlValidity validity =
      html::ValidateControl(element, CustomValidityMessage(element));
  if (validity.state.valid) {
    return true;
  }
  DispatchInvalidToNode(&element);
  return false;
}

bool Impl::CheckFormValidity(dom::Element& form)
{
  const std::vector<dom::Element*> invalid = InvalidFormControls(form);
  for (dom::Element* control : invalid) {
    DispatchInvalidToNode(control);
  }
  return invalid.empty();
}

// DomBinder forwarding surface used by the browser's form-submission path.
std::string DomBinder::CustomValidityMessage(const dom::Element& element) const
{
  return impl_->CustomValidityMessage(element);
}

void DomBinder::SetCustomValidity(dom::Element& element, std::string message)
{
  impl_->SetCustomValidity(element, std::move(message));
}

void DomBinder::DispatchInvalidEvent(dom::Element& element)
{
  impl_->DispatchInvalidToNode(&element);
}

std::vector<dom::Element*> DomBinder::InvalidFormControls(dom::Element& form) const
{
  return impl_->InvalidFormControls(form);
}

JSValue ElementGetChecked(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr || !IsCheckableInput(*element)) {
    return JS_NewBool(ctx, false);
  }
  return JS_NewBool(ctx, element->HasAttribute("checked"));
}

JSValue ElementSetChecked(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
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
  // Reflector setter: behaves like setAttribute (cascade may change).
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

JSValue ElementGetType(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (element->tag_name() != "input") {
    return JS_NewStringLen(ctx, "", 0);
  }
  // The type IDL attribute reflects the content attribute limited to known
  // keywords; the missing and invalid value defaults are the Text state.
  const std::string type = html::InputTypeKeyword(*element);
  return JS_NewStringLen(ctx, type.data(), type.size());
}

JSValue ElementSetType(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string type = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (element->tag_name() == "input") {
    // Capture the previous state and value before the attribute changes: the
    // type-change steps (HTML §4.10.5.3) compare the old and new value modes,
    // and a value-mode state must be sanitized under the old type first.
    const std::string old_type = html::InputTypeKeyword(*element);
    const std::string old_value = html::InputValue(*element);
    element->SetAttribute("type", type);
    html::InputTypeChanged(*element, old_type, old_value);
  }
  // Reflector setter: behaves like setAttribute (cascade may change).
  if (Impl* impl = ImplFor(ctx, this_val)) {
    impl->MarkDomDirty(element);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetPlaceholder(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string value = RawAttr(*element, "placeholder");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetPlaceholder(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("placeholder", text);
  // Reflector setter: behaves like setAttribute (cascade may change).
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

JSValue ElementGetDisabled(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewBool(ctx, element->HasAttribute("disabled"));
}

// input.multiple (HTML §4.10.5.3): a boolean content attribute.  It changes the
// email value sanitization algorithm (single address vs address list), which
// the shared implementation applies on every read.
JSValue ElementGetMultiple(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewBool(ctx, element->HasAttribute("multiple"));
}

JSValue ElementSetMultiple(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const int enabled = JS_ToBool(ctx, value);
  if (enabled < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }
  if (enabled != 0) {
    element->SetAttribute("multiple", "");
  } else {
    element->RemoveAttribute("multiple");
  }
  if (Impl* impl = ImplFor(ctx, this_val)) {
    impl->MarkDomDirty(element);
  }
  return JS_UNDEFINED;
}

// input.min / input.max / input.step: string attribute reflectors.  Their
// values feed the number/range/date value sanitization algorithms directly, so
// the getters return the raw attribute ("" when absent) and the setters behave
// like setAttribute.
JSValue ElementGetMin(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string value = RawAttr(*element, "min");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetMin(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("min", text);
  if (Impl* impl = ImplFor(ctx, this_val)) {
    impl->MarkDomDirty(element);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetMax(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string value = RawAttr(*element, "max");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetMax(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("max", text);
  if (Impl* impl = ImplFor(ctx, this_val)) {
    impl->MarkDomDirty(element);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetStep(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string value = RawAttr(*element, "step");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetStep(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("step", text);
  if (Impl* impl = ImplFor(ctx, this_val)) {
    impl->MarkDomDirty(element);
  }
  return JS_UNDEFINED;
}

JSValue ElementSetDisabled(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
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
  // Reflector setter: behaves like setAttribute (cascade may change).
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

JSValue ElementGetName(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string value = RawAttr(*element, "name");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetName(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("name", text);
  // Reflector setter: behaves like setAttribute (cascade may change).
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

JSValue ElementGetFormAction(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string form_action = RawAttr(*element, "formaction");
  return JS_NewStringLen(ctx, form_action.data(), form_action.size());
}

JSValue ElementSetFormAction(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string form_action = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("formaction", form_action);
  // Reflector setter: behaves like setAttribute (cascade may change).
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

JSValue FormGetAction(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr || element->tag_name() != "form") {
    return JS_ThrowTypeError(ctx, "not a form element");
  }
  const std::string action = ResolvedUrl(*impl, RawAttr(*element, "action"));
  return JS_NewStringLen(ctx, action.data(), action.size());
}

JSValue FormSetAction(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr || element->tag_name() != "form") {
    return JS_ThrowTypeError(ctx, "not a form element");
  }
  bool ok = false;
  const std::string action = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("action", action);
  // Reflector setter: behaves like setAttribute (cascade may change).
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

JSValue FormGetEnctype(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr || element->tag_name() != "form") {
    return JS_ThrowTypeError(ctx, "not a form element");
  }
  const std::string enctype = RawAttr(*element, "enctype");
  return JS_NewStringLen(ctx, enctype.data(), enctype.size());
}

JSValue FormSetEnctype(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr || element->tag_name() != "form") {
    return JS_ThrowTypeError(ctx, "not a form element");
  }
  bool ok = false;
  const std::string enctype = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("enctype", enctype);
  // Reflector setter: behaves like setAttribute (cascade may change).
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

JSValue FormGetMethod(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr || element->tag_name() != "form") {
    return JS_ThrowTypeError(ctx, "not a form element");
  }
  const std::string method = RawAttr(*element, "method");
  return JS_NewStringLen(ctx, method.data(), method.size());
}

JSValue FormSetMethod(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr || element->tag_name() != "form") {
    return JS_ThrowTypeError(ctx, "not a form element");
  }
  bool ok = false;
  const std::string method = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("method", method);
  // Reflector setter: behaves like setAttribute (cascade may change).
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

JSValue FormSubmit(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr || element->tag_name() != "form") {
    return JS_ThrowTypeError(ctx, "not a form element");
  }
  // HTMLFormElement.submit(): no submit event, no validation, no submitter.
  // The browser layer performs the submission after this script returns.
  if (impl->apis.form_submit) {
    impl->apis.form_submit(*element, nullptr, /*request_submit=*/false);
  }
  return JS_UNDEFINED;
}

JSValue FormRequestSubmit(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr || element->tag_name() != "form") {
    return JS_ThrowTypeError(ctx, "not a form element");
  }
  dom::Element* submitter = nullptr;
  if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
    submitter = AsElement(UnwrapNode(ctx, argv[0]));
    if (submitter == nullptr) {
      return JS_ThrowTypeError(ctx, "requestSubmit: submitter is not an element");
    }
    // HTMLFormElement.requestSubmit() steps: a submit button that belongs to
    // this form, or TypeError / NotFoundError.
    if (!html::IsSubmitButton(*submitter)) {
      return JS_ThrowTypeError(ctx, "requestSubmit: submitter is not a submit button");
    }
    if (html::OwnerForm(*submitter) != element) {
      return ThrowDomException(
          ctx, "NotFoundError", "requestSubmit: submitter is not owned by this form");
    }
  }
  if (impl->apis.form_submit) {
    // The callback records the request; the browser runs the submission
    // algorithm after this script run.  This means the submit event and
    // interactive validation happen after requestSubmit() returns, not
    // synchronously inside it (documented approximation: navigation must not
    // re-enter the page mid-script).
    impl->apis.form_submit(*element, submitter, /*request_submit=*/true);
    return JS_UNDEFINED;
  }
  // Standalone binder (no browser submission callback): run the observable
  // part of the sequence locally -- interactive validation, then the
  // cancelable submit event.
  const std::vector<dom::Element*> invalid = impl->InvalidFormControls(*element);
  for (dom::Element* control : invalid) {
    impl->DispatchInvalidToNode(control);
  }
  if (!invalid.empty()) {
    return JS_UNDEFINED;
  }
  (void)impl->DispatchCancelableToNode(element, "submit");
  return JS_UNDEFINED;
}

JSValue ElementGetHref(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
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
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
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
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string href = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("href", href);
  // Reflector setter: behaves like setAttribute (cascade may change).
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

JSValue ElementGetDownload(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string download = RawAttr(*element, "download");
  return JS_NewStringLen(ctx, download.data(), download.size());
}

JSValue ElementSetDownload(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string download = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("download", download);
  // Reflector setter: behaves like setAttribute (cascade may change).
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

JSValue ElementGetPing(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string ping = RawAttr(*element, "ping");
  return JS_NewStringLen(ctx, ping.data(), ping.size());
}

JSValue ElementSetPing(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string ping = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("ping", ping);
  // Reflector setter: behaves like setAttribute (cascade may change).
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

JSValue ElementGetTarget(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string value = RawAttr(*element, "target");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetTarget(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("target", text);
  // Reflector setter: behaves like setAttribute (cascade may change).
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

JSValue ElementGetRel(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string value = RawAttr(*element, "rel");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetRel(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("rel", text);
  // Reflector setter: behaves like setAttribute (cascade may change).
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

JSValue ElementGetSrc(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string raw = RawAttr(*element, "src");
  const std::string resolved = ResolvedUrl(*impl, raw);
  return JS_NewStringLen(ctx, resolved.data(), resolved.size());
}

JSValue ElementSetSrc(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string src = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("src", src);
  // Attribute mutations from a property setter behave exactly like
  // setAttribute: pages assign img.src from IntersectionObserver callbacks
  // (lazy loading) outside any timer/event dispatch, and the browser layer's
  // late-fetch pass and style reapplication hang off the DOM-dirty flag.
  if (Impl* impl = ImplFor(ctx, this_val); impl != nullptr) {
    impl->RecordAttributeMutation(element, "src");
    impl->MarkDomMediaDirty(element);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetSrcSet(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string srcset = RawAttr(*element, "srcset");
  return JS_NewStringLen(ctx, srcset.data(), srcset.size());
}

JSValue ElementSetSrcSet(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string srcset = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("srcset", srcset);
  // See ElementSetSrc: property-driven attribute mutation must mark the DOM
  // dirty like setAttribute("srcset", ...) does.
  if (Impl* impl = ImplFor(ctx, this_val); impl != nullptr) {
    impl->RecordAttributeMutation(element, "srcset");
    impl->MarkDomMediaDirty(element);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetSrcDoc(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string srcdoc = RawAttr(*element, "srcdoc");
  return JS_NewStringLen(ctx, srcdoc.data(), srcdoc.size());
}

JSValue ElementSetSrcDoc(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string srcdoc = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("srcdoc", srcdoc);
  // Reflector setter: behaves like setAttribute (cascade may change).
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

JSValue ElementGetCredentialless(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewBool(ctx, element->HasAttribute("credentialless"));
}

JSValue ElementSetCredentialless(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const int credentialless = JS_ToBool(ctx, value);
  if (credentialless < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }
  if (credentialless != 0) {
    element->SetAttribute("credentialless", "");
  } else {
    element->RemoveAttribute("credentialless");
  }
  // Reflector setter: behaves like setAttribute (cascade may change).
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

JSValue ElementGetAlt(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string value = RawAttr(*element, "alt");
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue ElementSetAlt(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("alt", text);
  // Reflector setter: behaves like setAttribute (cascade may change).
  ImplFor(ctx, this_val)->MarkDomDirty(element);
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
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewInt64(ctx, PositiveIntAttr(*element, "width"));
}

JSValue ElementSetWidth(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
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
  // Reflector setter: behaves like setAttribute (cascade may change).
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

JSValue ElementGetHeight(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewInt64(ctx, PositiveIntAttr(*element, "height"));
}

JSValue ElementSetHeight(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
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
  // Reflector setter: behaves like setAttribute (cascade may change).
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

JSValue ElementGetNaturalWidth(JSContext* ctx, JSValueConst this_val)
{
  if (AsElement(UnwrapNode(ctx, this_val)) == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // The binder does not track decoded image dimensions; 0 (a documented
  // approximation — scripts that check naturalWidth before drawing a layout
  // still branch correctly).
  return JS_NewInt32(ctx, 0);
}

JSValue ElementGetNaturalHeight(JSContext* ctx, JSValueConst this_val)
{
  if (AsElement(UnwrapNode(ctx, this_val)) == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewInt32(ctx, 0);
}

JSValue ElementGetComplete(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // complete: true once a src is present (loading is synchronous here).
  return JS_NewBool(ctx, element->HasAttribute("src"));
}

JSValue ElementGetCurrentSrc(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string raw = RawAttr(*element, "src");
  const std::string resolved = ResolvedUrl(*impl, raw);
  return JS_NewStringLen(ctx, resolved.data(), resolved.size());
}

// ---- HTMLMediaElement (video/audio) ----------------------------------------

// <video> and <audio> share the HTMLMediaElement JS surface implemented here
// (play/pause/currentTime/...); the page's media pipeline keys decoded state
// by element, so an element with no decoded state simply no-ops.
bool IsMediaElement(const dom::Element* element)
{
  return element != nullptr && (element->tag_name() == "video" || element->tag_name() == "audio");
}

JSValue
ElementPlayVideo(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (!IsMediaElement(element)) {
    return JS_ThrowTypeError(ctx, "play() is only defined on <video>/<audio>");
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
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (!IsMediaElement(element)) {
    return JS_ThrowTypeError(ctx, "pause() is only defined on <video>/<audio>");
  }
  if (impl->apis.video_pause) {
    impl->apis.video_pause(*element);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetVideoDuration(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr || !IsMediaElement(element) ||
      !impl->apis.video_duration) {
    return JS_NewFloat64(ctx, std::nan(""));
  }
  const std::optional<double> duration = impl->apis.video_duration(*element);
  return JS_NewFloat64(ctx, duration.value_or(std::nan("")));
}

JSValue ElementGetVideoCurrentTime(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr || !IsMediaElement(element) ||
      !impl->apis.video_current_time) {
    return JS_NewFloat64(ctx, 0);
  }
  return JS_NewFloat64(ctx, impl->apis.video_current_time(*element).value_or(0));
}

JSValue ElementSetVideoCurrentTime(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr || !IsMediaElement(element) || !impl->apis.video_seek) {
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
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr || !IsMediaElement(element) ||
      !impl->apis.video_paused) {
    return JS_NewBool(ctx, 1);
  }
  return JS_NewBool(ctx, impl->apis.video_paused(*element) ? 1 : 0);
}

// HTML §4.8.12: Audio(src) returns a new <audio> element (an ordinary
// HTMLAudioElement through the element prototype) and, when given, sets its
// src attribute.  jd.com's accessibility script starts with `f = new Audio`.
JSValue AudioConstructor(JSContext* ctx, JSValueConst /*new_target*/, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  auto element = std::make_unique<dom::Element>("audio");
  dom::Element* raw = element.get();
  impl->created[raw] = std::move(element);
  if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
    bool ok = false;
    const std::string src = ArgString(ctx, argv[0], &ok);
    if (!ok) {
      return JS_EXCEPTION;
    }
    if (!src.empty()) {
      raw->SetAttribute("src", src);
      impl->MarkDomMediaDirty(raw);
    }
  }
  return impl->WrapNode(raw);
}

void InstallAudioGlobal(JSContext* ctx, Impl& /*impl*/)
{
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue ctor = JS_NewCFunction2(ctx, AudioConstructor, "Audio", 0, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(ctx, global, "Audio", ctor); // steals
  JS_FreeValue(ctx, global);
}

[[maybe_unused]] JSValue ElementGetText(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
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
