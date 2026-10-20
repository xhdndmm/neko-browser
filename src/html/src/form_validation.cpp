#include "neko/html/form_validation.h"

#include "neko/base/string_util.h"
#include "neko/dom/node.h"
#include "neko/url/url.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neko::html {
namespace {

using dom::Element;
using dom::Node;

const Element* AsElement(const Node* node)
{
  return node != nullptr && node->node_type() == dom::NodeType::kElement
             ? static_cast<const Element*>(node)
             : nullptr;
}

std::string RawAttr(const Element& element, std::string_view name)
{
  return std::string(element.GetAttribute(name).value_or(""));
}

// The `type` attribute is an ASCII case-insensitive enumerated attribute; a
// missing value and an unknown keyword both mean the Text state.
std::string InputType(const Element& element)
{
  return base::ToLower(RawAttr(element, "type"));
}

enum class Kind
{
  kOther,
  kText,
  kEmail,
  kUrl,
  kNumber,
  kRange,
  kCheckbox,
  kRadio,
  kFile,
  kColor,
  kHidden,
  kButton,
  kTextArea,
  kSelect,
};

Kind KindOf(const Element& element)
{
  const std::string_view tag = element.tag_name();
  if (tag == "textarea") {
    return Kind::kTextArea;
  }
  if (tag == "select") {
    return Kind::kSelect;
  }
  if (tag != "input") {
    return Kind::kOther;
  }
  const std::string type = InputType(element);
  if (type.empty() || type == "text" || type == "search" || type == "tel" || type == "password" ||
      type == "date" || type == "month" || type == "week" || type == "time" ||
      type == "datetime-local") {
    return Kind::kText;
  }
  if (type == "email") {
    return Kind::kEmail;
  }
  if (type == "url") {
    return Kind::kUrl;
  }
  if (type == "number") {
    return Kind::kNumber;
  }
  if (type == "range") {
    return Kind::kRange;
  }
  if (type == "checkbox") {
    return Kind::kCheckbox;
  }
  if (type == "radio") {
    return Kind::kRadio;
  }
  if (type == "file") {
    return Kind::kFile;
  }
  if (type == "color") {
    return Kind::kColor;
  }
  if (type == "hidden") {
    return Kind::kHidden;
  }
  if (type == "submit" || type == "reset" || type == "button" || type == "image") {
    return Kind::kButton;
  }
  // The invalid value default is the Text state (§4.10.5.3).
  return Kind::kText;
}

bool SupportsMaxlength(Kind kind)
{
  return kind == Kind::kText || kind == Kind::kEmail || kind == Kind::kUrl ||
         kind == Kind::kTextArea;
}

bool SupportsReadonly(Kind kind)
{
  return kind == Kind::kText || kind == Kind::kEmail || kind == Kind::kUrl ||
         kind == Kind::kNumber || kind == Kind::kTextArea;
}

// True when |node| is |ancestor| or lies in its subtree.
bool IsInclusiveDescendant(const Node& node, const Node& ancestor)
{
  for (const Node* current = &node; current != nullptr; current = current->parent()) {
    if (current == &ancestor) {
      return true;
    }
  }
  return false;
}

// The nearest <form> ancestor, or nullptr (the default form-owner association).
const Element* NearestAncestorForm(const Element& element)
{
  for (const Node* current = element.parent(); current != nullptr; current = current->parent()) {
    const Element* candidate = AsElement(current);
    if (candidate != nullptr && candidate->tag_name() == "form") {
      return candidate;
    }
  }
  return nullptr;
}

const Element* FirstElementChild(const Element& element, std::string_view tag)
{
  for (Node* child : element.ChildNodes()) {
    const Element* child_element = AsElement(child);
    if (child_element != nullptr && child_element->tag_name() == tag) {
      return child_element;
    }
  }
  return nullptr;
}

// The first <legend> child of a disabled fieldset is not disabled by it
// (HTML §4.10.18.5); every other descendant is.
bool DisabledByFieldset(const Element& element)
{
  for (const Node* current = element.parent(); current != nullptr; current = current->parent()) {
    const Element* fieldset = AsElement(current);
    if (fieldset == nullptr || fieldset->tag_name() != "fieldset" ||
        !fieldset->HasAttribute("disabled")) {
      continue;
    }
    const Element* first_legend = FirstElementChild(*fieldset, "legend");
    if (first_legend != nullptr && IsInclusiveDescendant(element, *first_legend)) {
      continue;
    }
    return true;
  }
  return false;
}

bool HasAncestorNamed(const Element& element, std::string_view tag)
{
  for (const Node* current = element.parent(); current != nullptr; current = current->parent()) {
    const Element* ancestor = AsElement(current);
    if (ancestor != nullptr && ancestor->tag_name() == tag) {
      return true;
    }
  }
  return false;
}

bool IsDisabled(const Element& element)
{
  return element.HasAttribute("disabled") || DisabledByFieldset(element);
}

// ---------------------------------------------------------------------------
// Value access (the engine's live value model).
// ---------------------------------------------------------------------------

// The select's selected option: the first option carrying `selected`, else the
// first option in a single-select (browser default selection).  A multiple
// select only has a selected option when an option is explicitly selected.
const Element* SelectedOption(const Element& select)
{
  std::vector<Node*> stack;
  for (Node* child : select.ChildNodes()) {
    stack.push_back(child);
  }
  const Element* first = nullptr;
  bool first_found = false;
  while (!stack.empty()) {
    Node* node = stack.back();
    stack.pop_back();
    const Element* element = AsElement(node);
    if (element == nullptr) {
      continue;
    }
    if (element->tag_name() == "option") {
      if (!first_found) {
        first = element;
        first_found = true;
      }
      if (element->HasAttribute("selected")) {
        return element;
      }
      continue; // options do not nest
    }
    std::vector<Node*> children;
    for (Node* child : node->ChildNodes()) {
      children.push_back(child);
    }
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back(*it);
    }
  }
  return select.HasAttribute("multiple") ? nullptr : first;
}

std::string OptionValue(const Element& option)
{
  const std::optional<std::string_view> value = option.GetAttribute("value");
  return value.has_value() ? std::string(*value) : option.TextContent();
}

// True when |option| is disabled itself or by a disabled <optgroup> ancestor
// inside |select| (HTML §4.10.10/§4.10.11).
bool OptionIsDisabled(const Element& option, const Element& select)
{
  if (option.HasAttribute("disabled")) {
    return true;
  }
  for (const Node* current = option.parent(); current != nullptr; current = current->parent()) {
    if (current == &select) {
      break;
    }
    const Element* element = AsElement(current);
    if (element != nullptr && element->tag_name() == "optgroup" &&
        element->HasAttribute("disabled")) {
      return true;
    }
  }
  return false;
}

// Every enabled, selected option of |select| in tree order.  A single-select
// with no explicitly selected option selects its first option (the browser
// default, matching SelectedOption).
std::vector<const Element*> SelectedOptions(const Element& select)
{
  std::vector<const Element*> options;
  std::vector<Node*> stack;
  for (Node* child : select.ChildNodes()) {
    stack.push_back(child);
  }
  while (!stack.empty()) {
    Node* node = stack.back();
    stack.pop_back();
    const Element* element = AsElement(node);
    if (element != nullptr && element->tag_name() == "option") {
      options.push_back(element);
      continue; // options do not nest
    }
    std::vector<Node*> children;
    for (Node* child : node->ChildNodes()) {
      children.push_back(child);
    }
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back(*it);
    }
  }
  std::vector<const Element*> selected;
  for (const Element* option : options) {
    if (option->HasAttribute("selected") && !OptionIsDisabled(*option, select)) {
      selected.push_back(option);
    }
  }
  if (!selected.empty()) {
    return selected; // a multiple select may have several; single keeps all selected (first wins
                     // below)
  }
  if (select.HasAttribute("multiple")) {
    return {};
  }
  for (const Element* option : options) {
    if (!OptionIsDisabled(*option, select)) {
      return {option};
    }
  }
  return {};
}

std::string RawValue(const Element& element)
{
  const Kind kind = KindOf(element);
  if (kind == Kind::kSelect) {
    const Element* option = SelectedOption(element);
    return option != nullptr ? OptionValue(*option) : std::string();
  }
  if (kind == Kind::kTextArea) {
    // The parser does not strip the newline immediately after <textarea>
    // (the HTML tokenizer's RCDATA handling keeps it), so strip it here to
    // match the textarea raw value semantics.
    std::string text = element.TextContent();
    if (!text.empty() && text.front() == '\n') {
      text.erase(text.begin());
    }
    return text;
  }
  return RawAttr(element, "value");
}

// Value sanitization relevant to validation: email/url strip line breaks and
// leading/trailing ASCII whitespace (HTML §4.10.5.1.5/§4.10.5.1.6).
std::string SanitizedValue(const Element& element)
{
  std::string value = RawValue(element);
  const Kind kind = KindOf(element);
  if (kind == Kind::kEmail || kind == Kind::kUrl) {
    value.erase(
        std::remove_if(value.begin(), value.end(), [](char c) { return c == '\r' || c == '\n'; }),
        value.end());
    const std::string_view trimmed = base::Trim(value);
    value.assign(trimmed);
  }
  return value;
}

bool HasValue(const Element& element)
{
  const Kind kind = KindOf(element);
  if (kind == Kind::kCheckbox || kind == Kind::kRadio) {
    return element.HasAttribute("checked");
  }
  if (kind == Kind::kSelect) {
    return SelectedOption(element) != nullptr;
  }
  if (kind == Kind::kFile) {
    // File selection is not implemented; required file inputs must not block
    // every submission of every page that uses them.
    return true;
  }
  return !SanitizedValue(element).empty();
}

// True when this required radio has a checked member in its group.  The group
// is document-scoped, matching the engine's radio toggle approximation.
bool RadioGroupHasChecked(const Element& radio)
{
  const std::string_view name = radio.GetAttribute("name").value_or("");
  if (name.empty()) {
    return radio.HasAttribute("checked");
  }
  const Node* root = &radio;
  while (root->parent() != nullptr) {
    root = root->parent();
  }
  std::vector<const Node*> stack{root};
  while (!stack.empty()) {
    const Node* node = stack.back();
    stack.pop_back();
    const Element* element = AsElement(node);
    if (element != nullptr && element->tag_name() == "input" && KindOf(*element) == Kind::kRadio &&
        element->GetAttribute("name").value_or("") == name && element->HasAttribute("checked")) {
      return true;
    }
    for (const Node* child : node->ChildNodes()) {
      stack.push_back(child);
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Parsing helpers.
// ---------------------------------------------------------------------------

// A valid floating-point number per HTML: no surrounding whitespace, a finite
// value, and an optional leading sign (std::from_chars does not accept '+').
std::optional<double> ParseFloatingPoint(std::string_view text)
{
  if (text.empty()) {
    return std::nullopt;
  }
  if (text.front() == '+') {
    text.remove_prefix(1);
  }
  if (text.empty()) {
    return std::nullopt;
  }
  double value = 0;
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, value, std::chars_format::general);
  if (result.ec != std::errc() || result.ptr != end || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::uint32_t> ParseNonNegativeInteger(std::string_view text)
{
  if (text.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc() || result.ptr != end ||
      value > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(value);
}

// maxlength/minlength are measured in UTF-16 code units (HTML §4.10.5.3).
// The DOM stores UTF-8, so decode code points and count surrogate pairs as 2.
std::size_t Utf16CodeUnitLength(std::string_view text)
{
  std::size_t units = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    std::uint32_t code_point = lead;
    std::size_t length = 1;
    if ((lead & 0xe0U) == 0xc0U) {
      code_point = lead & 0x1fU;
      length = 2;
    } else if ((lead & 0xf0U) == 0xe0U) {
      code_point = lead & 0x0fU;
      length = 3;
    } else if ((lead & 0xf8U) == 0xf0U) {
      code_point = lead & 0x07U;
      length = 4;
    }
    if (length > 1) {
      if (i + length > text.size()) {
        ++units; // truncated sequence: count the byte as one unit
        ++i;
        continue;
      }
      bool valid = true;
      for (std::size_t k = 1; k < length; ++k) {
        const unsigned char continuation = static_cast<unsigned char>(text[i + k]);
        if ((continuation & 0xc0U) != 0x80U) {
          valid = false;
          break;
        }
        code_point = (code_point << 6U) | (continuation & 0x3fU);
      }
      if (!valid) {
        ++units;
        ++i;
        continue;
      }
    }
    units += code_point > 0xffffU ? 2 : 1;
    i += length;
  }
  return units;
}

bool IsAsciiAlphaNumeric(char c)
{
  const unsigned char u = static_cast<unsigned char>(c);
  return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9');
}

bool IsAtext(char c)
{
  if (IsAsciiAlphaNumeric(c) || static_cast<unsigned char>(c) >= 0x80U) {
    return true;
  }
  switch (c) {
  case '!':
  case '#':
  case '$':
  case '%':
  case '&':
  case '\'':
  case '*':
  case '+':
  case '/':
  case '=':
  case '?':
  case '^':
  case '_':
  case '`':
  case '{':
  case '|':
  case '}':
  case '~':
  case '-':
  case '.':
    return true;
  default:
    return false;
  }
}

// The HTML spec's "valid email address" grammar:
//   1*(atext / ".") "@" label *("." label)
//   label = let-dig [*61(ldh-str) let-dig]
bool IsValidEmail(std::string_view address)
{
  const std::size_t at = address.find('@');
  if (at == std::string_view::npos || at == 0 || at + 1 >= address.size()) {
    return false;
  }
  for (char c : address.substr(0, at)) {
    if (!IsAtext(c)) {
      return false;
    }
  }
  if (address.find('@', at + 1) != std::string_view::npos) {
    return false;
  }
  std::string_view domain = address.substr(at + 1);
  if (domain.size() > 253) {
    return false;
  }
  while (!domain.empty()) {
    if (domain.front() == '.') {
      return false;
    }
    const std::size_t dot = domain.find('.');
    const std::string_view label = dot == std::string_view::npos ? domain : domain.substr(0, dot);
    if (label.empty() || label.size() > 63 || !IsAsciiAlphaNumeric(label.front()) ||
        !IsAsciiAlphaNumeric(label.back())) {
      return false;
    }
    for (char c : label) {
      if (!IsAsciiAlphaNumeric(c) && c != '-') {
        return false;
      }
    }
    if (dot == std::string_view::npos) {
      break;
    }
    domain.remove_prefix(dot + 1);
  }
  return true;
}

bool IsValidEmailValue(const Element& element, const std::string& value)
{
  if (!element.HasAttribute("multiple")) {
    return IsValidEmail(value);
  }
  // Split on commas and validate each token; whitespace around tokens is
  // stripped by the email value sanitization algorithm.
  bool any = false;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t comma = value.find(',', start);
    const std::string_view token = std::string_view(value).substr(
        start, comma == std::string::npos ? std::string::npos : comma - start);
    const std::string_view trimmed = base::Trim(token);
    if (trimmed.empty()) {
      // The spec treats empty tokens as invalid when the value is non-empty.
      if (!value.empty()) {
        return false;
      }
    } else if (!IsValidEmail(trimmed)) {
      return false;
    } else {
      any = true;
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return any || value.empty();
}

bool IsValidUrlValue(const std::string& value)
{
  if (value.empty()) {
    return true;
  }
  return url::Url::Parse(value).has_value();
}

std::string FormatNumber(double value)
{
  char buffer[64];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec != std::errc()) {
    return std::string();
  }
  return std::string(buffer, result.ptr);
}

struct RangeInfo
{
  bool has_value = false;
  double value = 0;
  std::optional<double> min;
  std::optional<double> max;
  double step = 1;
  bool step_any = false;
  double step_base = 0;
  bool bad_input = false;
};

RangeInfo ComputeRangeInfo(const Element& element, Kind kind)
{
  RangeInfo info;
  const std::string raw_value = RawAttr(element, "value");
  const std::optional<double> parsed_value = ParseFloatingPoint(raw_value);
  const std::optional<double> parsed_min = ParseFloatingPoint(RawAttr(element, "min"));
  const std::optional<double> parsed_max = ParseFloatingPoint(RawAttr(element, "max"));

  if (kind == Kind::kRange) {
    const double minimum = parsed_min.value_or(0.0);
    double maximum = parsed_max.value_or(100.0);
    if (maximum < minimum) {
      maximum = minimum;
    }
    info.min = minimum;
    info.max = maximum;

    const std::string step_attr = RawAttr(element, "step");
    if (base::AsciiEqualsIgnoreCase(step_attr, "any")) {
      info.step_any = true;
    } else if (const std::optional<double> parsed_step = ParseFloatingPoint(step_attr);
               parsed_step.has_value() && *parsed_step > 0) {
      info.step = *parsed_step;
    }
    info.step_base = parsed_min.value_or(parsed_value.value_or(0.0));

    double value = parsed_value.value_or(minimum + ((maximum - minimum) / 2.0));
    value = std::clamp(value, minimum, maximum);
    if (!info.step_any) {
      // Range values are step-aligned by the value sanitization algorithm.
      const double steps = std::round((value - info.step_base) / info.step);
      value = info.step_base + (steps * info.step);
      value = std::clamp(value, minimum, maximum);
    }
    info.value = value;
    info.has_value = true;
    return info;
  }

  // number
  info.min = parsed_min;
  info.max = parsed_max;
  const std::string step_attr = RawAttr(element, "step");
  if (base::AsciiEqualsIgnoreCase(step_attr, "any")) {
    info.step_any = true;
  } else if (const std::optional<double> parsed_step = ParseFloatingPoint(step_attr);
             parsed_step.has_value() && *parsed_step > 0) {
    info.step = *parsed_step;
  }
  if (parsed_value.has_value()) {
    info.has_value = true;
    info.value = *parsed_value;
    info.step_base = info.min.value_or(*parsed_value);
  } else {
    if (!raw_value.empty()) {
      info.bad_input = true;
    }
    info.step_base = info.min.value_or(0.0);
  }
  return info;
}

std::string DefaultMessage(const Element& element,
                           Kind kind,
                           const ValidityState& state,
                           std::size_t too_long_length,
                           std::size_t too_short_length,
                           const RangeInfo& range)
{
  if (state.value_missing) {
    return "Please fill out this field.";
  }
  if (state.type_mismatch) {
    return kind == Kind::kEmail ? "Please enter an email address." : "Please enter a URL.";
  }
  if (state.too_long) {
    return "Please shorten this text to " + std::to_string(too_long_length) +
           " characters or fewer (you are currently using " +
           std::to_string(Utf16CodeUnitLength(SanitizedValue(element))) + " characters).";
  }
  if (state.too_short) {
    return "Please lengthen this text to " + std::to_string(too_short_length) +
           " characters or more (you are currently using " +
           std::to_string(Utf16CodeUnitLength(SanitizedValue(element))) + " characters).";
  }
  if (state.range_underflow) {
    return "Value must be greater than or equal to " +
           FormatNumber(range.min.value_or(range.value)) + ".";
  }
  if (state.range_overflow) {
    return "Value must be less than or equal to " + FormatNumber(range.max.value_or(range.value)) +
           ".";
  }
  if (state.step_mismatch) {
    return "Please enter a valid value.";
  }
  if (state.bad_input) {
    return kind == Kind::kNumber ? "Please enter a number." : "Please enter a valid value.";
  }
  return std::string();
}

} // namespace

bool ValidityState::HasError() const
{
  return value_missing || type_mismatch || pattern_mismatch || too_long || too_short ||
         range_underflow || range_overflow || step_mismatch || bad_input || custom_error;
}

bool IsSubmittableControl(const Element& element)
{
  const std::string_view tag = element.tag_name();
  return tag == "input" || tag == "textarea" || tag == "select" || tag == "button";
}

bool IsBarredFromConstraintValidation(const Element& element)
{
  if (!IsSubmittableControl(element)) {
    return true;
  }
  const Kind kind = KindOf(element);
  if (kind == Kind::kHidden || kind == Kind::kButton || element.tag_name() == "button") {
    return true;
  }
  if (IsDisabled(element)) {
    return true;
  }
  if (HasAncestorNamed(element, "datalist")) {
    return true;
  }
  if (SupportsReadonly(kind) && element.HasAttribute("readonly")) {
    return true;
  }
  return false;
}

bool IsCandidateForConstraintValidation(const Element& element)
{
  return !IsBarredFromConstraintValidation(element);
}

ControlValidity ValidateControl(const Element& element, std::string_view custom_error)
{
  ControlValidity result;
  const Kind kind = KindOf(element);
  if (!IsSubmittableControl(element)) {
    return result;
  }

  ValidityState& state = result.state;
  state.custom_error = !custom_error.empty();
  if (IsBarredFromConstraintValidation(element)) {
    // Barred elements satisfy their constraints (and report valid), but the
    // flags still reflect the underlying state (including customError).
    state.valid = true;
    return result;
  }

  const std::string value = SanitizedValue(element);
  const bool required = element.HasAttribute("required");

  if (required) {
    switch (kind) {
    case Kind::kCheckbox:
      state.value_missing = !element.HasAttribute("checked");
      break;
    case Kind::kRadio:
      state.value_missing = !RadioGroupHasChecked(element);
      break;
    case Kind::kSelect:
      state.value_missing = SelectedOption(element) == nullptr;
      break;
    case Kind::kFile:
      // No file picker: a required file input is not reported missing (the
      // engine cannot satisfy it, and blocking every submission would be
      // worse than the documented gap).
      break;
    case Kind::kRange:
    case Kind::kColor:
    case Kind::kOther:
      break;
    default:
      state.value_missing = !HasValue(element);
      break;
    }
  }

  if (kind == Kind::kEmail && !value.empty() && !IsValidEmailValue(element, value)) {
    state.type_mismatch = true;
  } else if (kind == Kind::kUrl && !value.empty() && !IsValidUrlValue(value)) {
    state.type_mismatch = true;
  }

  // maxlength/minlength (UTF-16 code units).
  if (SupportsMaxlength(kind)) {
    const std::size_t length = Utf16CodeUnitLength(value);
    if (const std::optional<std::uint32_t> maxlength =
            ParseNonNegativeInteger(RawAttr(element, "maxlength"));
        maxlength.has_value() && length > *maxlength) {
      state.too_long = true;
    }
    if (const std::optional<std::uint32_t> minlength =
            ParseNonNegativeInteger(RawAttr(element, "minlength"));
        minlength.has_value() && !value.empty() && length < *minlength) {
      state.too_short = true;
    }
  }

  RangeInfo range;
  if (kind == Kind::kNumber || kind == Kind::kRange) {
    range = ComputeRangeInfo(element, kind);
    state.bad_input = range.bad_input;
    if (range.has_value) {
      if (range.min.has_value() && range.value < *range.min) {
        state.range_underflow = true;
      }
      if (range.max.has_value() && range.value > *range.max) {
        state.range_overflow = true;
      }
      if (!range.step_any && range.step > 0) {
        const double ratio = (range.value - range.step_base) / range.step;
        const double nearest = std::round(ratio);
        if (std::abs(ratio - nearest) > 1e-7 * std::max(1.0, std::abs(ratio))) {
          state.step_mismatch = true;
        }
      }
    }
  }

  // patternMismatch is NOT IMPLEMENTED: evaluating the pattern attribute needs
  // an ECMAScript RegExp engine, which this shared layer does not have.  The
  // flag stays false (never a fake match or mismatch).

  state.valid = !state.HasError();
  if (!state.valid) {
    if (state.custom_error) {
      result.message = std::string(custom_error);
    } else {
      result.message =
          DefaultMessage(element,
                         kind,
                         state,
                         ParseNonNegativeInteger(RawAttr(element, "maxlength")).value_or(0),
                         ParseNonNegativeInteger(RawAttr(element, "minlength")).value_or(0),
                         range);
    }
  }
  return result;
}

dom::Element* OwnerForm(const dom::Element& element)
{
  if (element.HasAttribute("form")) {
    const std::string id = RawAttr(element, "form");
    if (id.empty()) {
      return nullptr;
    }
    const Node* root = &element;
    while (root->parent() != nullptr) {
      root = root->parent();
    }
    std::vector<const Node*> stack{root};
    while (!stack.empty()) {
      const Node* node = stack.back();
      stack.pop_back();
      const Element* candidate = AsElement(node);
      if (candidate != nullptr) {
        const std::optional<std::string_view> candidate_id = candidate->Id();
        if (candidate_id.has_value() && *candidate_id == id && candidate->tag_name() == "form") {
          return const_cast<Element*>(candidate);
        }
      }
      for (const Node* child : node->ChildNodes()) {
        stack.push_back(child);
      }
    }
    return nullptr;
  }
  return const_cast<Element*>(NearestAncestorForm(element));
}

std::vector<dom::Element*> FormControls(const dom::Element& form)
{
  const std::string form_id = RawAttr(form, "id");
  const Node* root = &form;
  while (root->parent() != nullptr) {
    root = root->parent();
  }

  std::vector<Element*> controls;
  std::vector<const Node*> stack{root};
  while (!stack.empty()) {
    const Node* node = stack.back();
    stack.pop_back();
    const Element* element = AsElement(node);
    if (element != nullptr && IsSubmittableControl(*element)) {
      const std::optional<std::string_view> form_attr = element->GetAttribute("form");
      const bool associated = form_attr.has_value() ? (!form_id.empty() && *form_attr == form_id)
                                                    : (NearestAncestorForm(*element) == &form);
      if (associated) {
        controls.push_back(const_cast<Element*>(element));
      }
    }
    std::vector<const Node*> children;
    for (const Node* child : node->ChildNodes()) {
      children.push_back(child);
    }
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back(*it);
    }
  }
  return controls;
}

bool IsSubmitButton(const dom::Element& element)
{
  const std::string_view tag = element.tag_name();
  if (tag == "button") {
    const std::string type = base::ToLower(RawAttr(element, "type"));
    // <button>'s missing value default is Submit (HTML §4.10.6).
    return type.empty() || type == "submit";
  }
  if (tag == "input") {
    const std::string type = InputType(element);
    return type == "submit" || type == "image";
  }
  return false;
}

dom::Element* DefaultButton(const dom::Element& form)
{
  for (Element* control : FormControls(form)) {
    if (IsSubmitButton(*control)) {
      return control;
    }
  }
  return nullptr;
}

std::vector<FormEntry> ConstructEntryList(const dom::Element& form, const dom::Element* submitter)
{
  std::vector<FormEntry> entries;
  for (Element* field : FormControls(form)) {
    // HTML §4.10.22.4: datalist descendants and disabled controls never
    // contribute, nor do buttons other than the submitter.
    if (HasAncestorNamed(*field, "datalist") || IsDisabled(*field)) {
      continue;
    }
    const std::string tag(field->tag_name());
    const bool is_input = tag == "input";
    const std::string type = is_input ? InputType(*field) : std::string();
    if (tag == "button") {
      if (field != submitter || !IsSubmitButton(*field)) {
        continue;
      }
    } else if (is_input) {
      if (type == "checkbox" || type == "radio") {
        if (!field->HasAttribute("checked")) {
          continue;
        }
      } else if (type == "reset" || type == "button") {
        continue;
      } else if (type == "submit" || type == "image") {
        if (field != submitter) {
          continue;
        }
        // NOT IMPLEMENTED: an active image button contributes its selected
        // coordinate as name.x/name.y.  The engine has no click coordinate at
        // submission time, so it contributes no entry (not even empty ones).
        if (type == "image") {
          continue;
        }
      }
    }
    const std::optional<std::string_view> name_attr = field->GetAttribute("name");
    if (!name_attr.has_value() || name_attr->empty()) {
      continue;
    }
    const std::string name(*name_attr);
    if (is_input && (type == "checkbox" || type == "radio")) {
      const std::optional<std::string_view> value_attr = field->GetAttribute("value");
      entries.push_back({name, value_attr.has_value() ? std::string(*value_attr) : "on", false});
      continue;
    }
    if (is_input && type == "file") {
      // No File API / picker: the control has no selected files.  This is a
      // file entry with no content, which the encoders skip rather than
      // sending a fabricated empty file part.
      entries.push_back({name, std::string(), true});
      continue;
    }
    if (tag == "button") {
      // A submit button's value is its `value` attribute (default empty).
      entries.push_back({name, RawAttr(*field, "value"), false});
      continue;
    }
    if (tag == "select") {
      std::vector<const Element*> selected = SelectedOptions(*field);
      if (!field->HasAttribute("multiple") && selected.size() > 1) {
        selected.resize(1);
      }
      for (const Element* option : selected) {
        entries.push_back({name, OptionValue(*option), false});
      }
      continue;
    }
    entries.push_back({name, SanitizedValue(*field), false});
  }
  return entries;
}

std::vector<dom::Element*> InvalidControls(const dom::Element& form,
                                           const CustomValidityLookup& custom_error)
{
  std::vector<Element*> invalid;
  for (Element* control : FormControls(form)) {
    const std::string custom = custom_error ? custom_error(*control) : std::string();
    if (!ValidateControl(*control, custom).state.valid) {
      invalid.push_back(control);
    }
  }
  return invalid;
}

bool IsFormValid(const dom::Element& form, const CustomValidityLookup& custom_error)
{
  return InvalidControls(form, custom_error).empty();
}

} // namespace neko::html
