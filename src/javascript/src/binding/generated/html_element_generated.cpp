// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/html_element.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

#include "html_element_generated.h"

#include "../binding_internal.h"

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace neko::javascript {
namespace {

// [Reflect=title] attribute DOMString title;
JSValue GeneratedHTMLElementGetTitle(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::optional<std::string_view> value = element->GetAttribute("title");
  return value.has_value() ? JS_NewStringLen(ctx, value->data(), value->size())
                           : JS_NewStringLen(ctx, "", 0);
}

JSValue GeneratedHTMLElementSetTitle(JSContext* ctx, JSValueConst this_val, JSValueConst value)
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
  if (text.empty()) {
    element->RemoveAttribute("title");
  } else {
    element->SetAttribute("title", text);
  }
  ImplFor(ctx, this_val)->MarkAttrDirty(element, "title");
  return JS_UNDEFINED;
}

// [Reflect=lang] attribute DOMString lang;
JSValue GeneratedHTMLElementGetLang(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::optional<std::string_view> value = element->GetAttribute("lang");
  return value.has_value() ? JS_NewStringLen(ctx, value->data(), value->size())
                           : JS_NewStringLen(ctx, "", 0);
}

JSValue GeneratedHTMLElementSetLang(JSContext* ctx, JSValueConst this_val, JSValueConst value)
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
  if (text.empty()) {
    element->RemoveAttribute("lang");
  } else {
    element->SetAttribute("lang", text);
  }
  ImplFor(ctx, this_val)->MarkAttrDirty(element, "lang");
  return JS_UNDEFINED;
}

// [Reflect=hidden] attribute boolean hidden;
JSValue GeneratedHTMLElementGetHidden(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewBool(ctx, element->HasAttribute("hidden"));
}

JSValue GeneratedHTMLElementSetHidden(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const int flag = JS_ToBool(ctx, value);
  if (flag < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }
  if (flag != 0) {
    element->SetAttribute("hidden", "");
  } else {
    element->RemoveAttribute("hidden");
  }
  ImplFor(ctx, this_val)->MarkAttrDirty(element, "hidden");
  return JS_UNDEFINED;
}

// [Reflect=inert] attribute boolean inert;
JSValue GeneratedHTMLElementGetInert(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewBool(ctx, element->HasAttribute("inert"));
}

JSValue GeneratedHTMLElementSetInert(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const int flag = JS_ToBool(ctx, value);
  if (flag < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }
  if (flag != 0) {
    element->SetAttribute("inert", "");
  } else {
    element->RemoveAttribute("inert");
  }
  ImplFor(ctx, this_val)->MarkAttrDirty(element, "inert");
  return JS_UNDEFINED;
}

// Methods: the body is hand-written; the generator owns the
// registration so the name and argument count cannot drift.
constexpr JSCFunctionListEntry kMethods[] = {
    JS_CFUNC_DEF("focus", 0, ElementFocus),
};

} // namespace

void DefineGeneratedHTMLElement(JSContext* ctx, JSValue proto)
{
  JS_SetPropertyFunctionList(ctx, proto, kMethods, static_cast<int>(std::size(kMethods)));
  DefineAccessor(ctx,
                 proto,
                 "title",
                 MakeGetter(ctx, "title", GeneratedHTMLElementGetTitle),
                 MakeSetter(ctx, "title", GeneratedHTMLElementSetTitle));
  DefineAccessor(ctx,
                 proto,
                 "lang",
                 MakeGetter(ctx, "lang", GeneratedHTMLElementGetLang),
                 MakeSetter(ctx, "lang", GeneratedHTMLElementSetLang));
  DefineAccessor(ctx,
                 proto,
                 "hidden",
                 MakeGetter(ctx, "hidden", GeneratedHTMLElementGetHidden),
                 MakeSetter(ctx, "hidden", GeneratedHTMLElementSetHidden));
  DefineAccessor(ctx,
                 proto,
                 "inert",
                 MakeGetter(ctx, "inert", GeneratedHTMLElementGetInert),
                 MakeSetter(ctx, "inert", GeneratedHTMLElementSetInert));
}

} // namespace neko::javascript
