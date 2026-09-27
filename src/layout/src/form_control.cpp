#include "neko/layout/form_control.h"

#include "neko/base/string_util.h"

#include <string_view>

namespace neko::layout {
namespace {

// The `type` attribute value, defaulted to the Text state when the attribute is
// absent (§4.10.5.3: the missing value default is the Text state).
std::string_view TypeAttribute(const dom::Element& element)
{
  return element.GetAttribute("type").value_or("text");
}

} // namespace

FormControlKind ClassifyFormControl(const dom::Element& element)
{
  const std::string_view tag = element.tag_name();
  if (tag == "button") {
    return FormControlKind::kButton;
  }
  if (tag == "select") {
    return FormControlKind::kSelect;
  }
  if (tag == "textarea") {
    return FormControlKind::kTextArea;
  }
  if (tag != "input") {
    return FormControlKind::kNone;
  }

  const std::string_view type = TypeAttribute(element);
  if (base::AsciiEqualsIgnoreCase(type, "hidden")) {
    return FormControlKind::kHidden;
  }
  if (base::AsciiEqualsIgnoreCase(type, "checkbox")) {
    return FormControlKind::kCheckbox;
  }
  if (base::AsciiEqualsIgnoreCase(type, "radio")) {
    return FormControlKind::kRadio;
  }
  if (base::AsciiEqualsIgnoreCase(type, "range")) {
    return FormControlKind::kRange;
  }
  if (base::AsciiEqualsIgnoreCase(type, "color")) {
    return FormControlKind::kColor;
  }
  if (base::AsciiEqualsIgnoreCase(type, "file")) {
    return FormControlKind::kFile;
  }
  // The Image Button state is a submit button (§4.10.5.1.20); it is classified
  // as a button because the widget kinds of the form-control contract have no
  // separate image-button case (its `src` image is not loaded -- see the
  // PARTIALLY IMPLEMENTED note in layout.cpp).
  if (base::AsciiEqualsIgnoreCase(type, "submit") || base::AsciiEqualsIgnoreCase(type, "reset") ||
      base::AsciiEqualsIgnoreCase(type, "button") || base::AsciiEqualsIgnoreCase(type, "image")) {
    return FormControlKind::kButton;
  }
  // The text-entry states (text, search, telephone, URL, email, password), the
  // domain-specific widgets (number and the date/time family) and any unknown
  // keyword (the invalid value default is the Text state).
  return FormControlKind::kText;
}

bool HasFlowRootInnerDisplay(const dom::Element& element)
{
  if (element.tag_name() != "input") {
    return false;
  }
  const std::string_view type = TypeAttribute(element);
  return !base::AsciiEqualsIgnoreCase(type, "hidden") &&
         !base::AsciiEqualsIgnoreCase(type, "image");
}

} // namespace neko::layout
