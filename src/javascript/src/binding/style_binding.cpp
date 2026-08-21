// neko::javascript DOM bindings — CSSStyleDeclaration family.
//
// Part of the dom_binding split (see binding/binding_internal.h): the inline
// style attribute helpers (shared with element_binding.cpp through
// binding_internal.h), the CSSStyleDeclaration callbacks, and
// DefineStylePrototype.

#include "neko/css/parser.h"
#include "neko/dom/element.h"

#include "binding_internal.h"

#include <quickjs.h>
#include <string>
#include <string_view>

namespace neko::javascript {

std::vector<InlineDecl> ParseInlineStyle(std::string_view attr)
{
  std::vector<InlineDecl> out;
  for (const css::Declaration& d : css::ParseDeclarationBlock(attr)) {
    out.push_back(InlineDecl{d.property, d.value, d.important});
  }
  return out;
}

std::string SerializeInlineStyle(const std::vector<InlineDecl>& decls)
{
  std::string out;
  for (std::size_t i = 0; i < decls.size(); ++i) {
    if (i > 0) {
      out += "; ";
    }
    out += decls[i].property;
    out += ": ";
    out += decls[i].value;
    if (decls[i].important) {
      out += " !important";
    }
  }
  return out;
}

std::string CurrentStyleAttr(const dom::Element& element)
{
  const std::optional<std::string_view> attr = element.GetAttribute("style");
  return attr.has_value() ? std::string(attr.value()) : std::string();
}

void SetStyleAttr(dom::Element& element, const std::string& value)
{
  if (value.empty()) {
    element.RemoveAttribute("style");
  } else {
    element.SetAttribute("style", value);
  }
}

JSValue StyleSetProperty(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not a style declaration");
  }
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "setProperty requires (property, value)");
  }
  bool ok = false;
  const std::string name = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::string value = ArgString(ctx, argv[1], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  std::vector<InlineDecl> decls = ParseInlineStyle(CurrentStyleAttr(*element));
  decls.erase(std::remove_if(decls.begin(),
                             decls.end(),
                             [&](const InlineDecl& d) { return d.property == name; }),
              decls.end());
  if (!value.empty()) {
    decls.push_back(InlineDecl{name, value, false});
  }
  SetStyleAttr(*element, SerializeInlineStyle(decls));
  ImplFor(ctx, this_val)->MarkDomDirty();
  return JS_UNDEFINED;
}

JSValue StyleGetPropertyValue(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not a style declaration");
  }
  if (argc < 1) {
    return JS_NewStringLen(ctx, "", 0);
  }
  bool ok = false;
  const std::string name = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    return JS_EXCEPTION;
  }
  for (const InlineDecl& d : ParseInlineStyle(CurrentStyleAttr(*element))) {
    if (d.property == name) {
      return JS_NewStringLen(ctx, d.value.data(), d.value.size());
    }
  }
  return JS_NewStringLen(ctx, "", 0);
}

JSValue StyleRemoveProperty(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not a style declaration");
  }
  bool ok = false;
  const std::string name = argc >= 1 ? ToLower(ArgString(ctx, argv[0], &ok)) : std::string();
  if (!ok) {
    return JS_EXCEPTION;
  }
  std::vector<InlineDecl> decls = ParseInlineStyle(CurrentStyleAttr(*element));
  std::string removed;
  decls.erase(std::remove_if(decls.begin(),
                             decls.end(),
                             [&](const InlineDecl& d) {
                               if (d.property == name) {
                                 removed = d.value;
                                 return true;
                               }
                               return false;
                             }),
              decls.end());
  SetStyleAttr(*element, SerializeInlineStyle(decls));
  return JS_NewStringLen(ctx, removed.data(), removed.size());
}

JSValue StyleGetProperty(JSContext* ctx, JSValueConst this_val, int magic)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not a style declaration");
  }
  if (magic < 0 || magic >= static_cast<int>(kStyleProps.size())) {
    return JS_NewStringLen(ctx, "", 0);
  }
  const std::string_view prop = kStyleProps[static_cast<std::size_t>(magic)];
  for (const InlineDecl& d : ParseInlineStyle(CurrentStyleAttr(*element))) {
    if (d.property == prop) {
      return JS_NewStringLen(ctx, d.value.data(), d.value.size());
    }
  }
  return JS_NewStringLen(ctx, "", 0);
}

JSValue StyleSetPropertyDirect(JSContext* ctx, JSValueConst this_val, JSValueConst value, int magic)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not a style declaration");
  }
  bool ok = false;
  const std::string prop_value = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (magic < 0 || magic >= static_cast<int>(kStyleProps.size())) {
    return JS_UNDEFINED;
  }
  const std::string_view prop = kStyleProps[static_cast<std::size_t>(magic)];
  std::vector<InlineDecl> decls = ParseInlineStyle(CurrentStyleAttr(*element));
  decls.erase(std::remove_if(decls.begin(),
                             decls.end(),
                             [&](const InlineDecl& d) { return d.property == prop; }),
              decls.end());
  if (!prop_value.empty()) {
    decls.push_back(InlineDecl{std::string(prop), prop_value, false});
  }
  SetStyleAttr(*element, SerializeInlineStyle(decls));
  return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Global timer functions (setTimeout/setInterval/clearTimeout/clearInterval).
// ---------------------------------------------------------------------------

void DefineStylePrototype(JSContext* ctx, Impl& impl)
{
  static const std::array<JSCFunctionListEntry, 3> kMethods = {{
      JS_CFUNC_DEF("setProperty", 2, StyleSetProperty),
      JS_CFUNC_DEF("getPropertyValue", 1, StyleGetPropertyValue),
      JS_CFUNC_DEF("removeProperty", 1, StyleRemoveProperty),
  }};
  JS_SetPropertyFunctionList(
      ctx, impl.style_proto, kMethods.data(), static_cast<int>(kMethods.size()));

  // Direct camelCase accessors for the documented property subset.
  for (std::size_t i = 0; i < kStyleProps.size(); ++i) {
    const std::string_view prop = kStyleProps[i];
    std::string camel;
    bool upper = false;
    for (const char c : prop) {
      if (c == '-') {
        upper = true;
        continue;
      }
      camel.push_back(upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
      upper = false;
    }
    const int magic = static_cast<int>(i);
    DefineAccessor(ctx,
                   impl.style_proto,
                   camel.c_str(),
                   MakeGetterMagic(ctx, camel.c_str(), StyleGetProperty, magic),
                   MakeSetterMagic(ctx, camel.c_str(), StyleSetPropertyDirect, magic));
  }
}

} // namespace neko::javascript
