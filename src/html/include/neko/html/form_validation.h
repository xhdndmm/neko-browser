#pragma once

#include "neko/dom/element.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace neko::html {

// Constraint validation for the supported form controls (WHATWG HTML
// §4.10.21 "Constraint validation").  This module owns the rules; the
// JavaScript bindings (checkValidity/reportValidity/validity/...) and the
// browser's form-submission path both call into it, so there is exactly one
// implementation of each constraint.
//
// Implemented constraints:
//   * valueMissing (required) for text-like inputs, textarea, checkbox,
//     radio groups (document-scoped, matching the engine's radio toggle) and
//     select (no selected option);
//   * typeMismatch for email (the spec's email grammar, including the
//     `multiple` attribute) and url (absolute URL, checked with the engine's
//     URL parser);
//   * tooLong / tooShort from maxlength / minlength, measured in UTF-16 code
//     units as the HTML standard defines the attributes.  The dirty-value flag
//     and the "last changed by a user edit" condition are not modelled: the
//     length constraints are evaluated whenever the attributes are present;
//   * rangeUnderflow / rangeOverflow / stepMismatch / badInput for number and
//     range, from min / max / step (default step 1, "any" disables it);
//   * customError from setCustomValidity (the message is supplied by the
//     caller; this module does not own per-element script state).
//
// NOT IMPLEMENTED: patternMismatch.  Evaluating the `pattern` attribute
// requires an ECMAScript regular-expression engine, which the shared HTML
// layer does not have.  patternMismatch therefore stays false; it is never
// faked into a match or a mismatch.
//
// NOT IMPLEMENTED: badInput for invalid *user* text.  The current value is the
// sanitized value, so raw user input that the sanitizer rejects (typing "x"
// into a number control) is never stored; a number input cannot report
// badInput from the DOM.  A `value` attribute that is not a valid
// floating-point number sanitizes to the empty string, exactly as browsers
// expose it through `.value`.
//
// The engine's control value model is the shared one from input_value.h: an
// <input> has a current value (sanitized per type) plus the raw `value`
// content attribute (the default value), while a <textarea> uses its text
// content and a <select> its selected option.  This module never reads an
// input's raw attribute as if it were the live value.
//
// A single-select with no explicitly `selected` option has its first option
// selected (the browser default), so `required` on such a select is only
// missing when it has no options at all.  File inputs are candidates but
// required file inputs are not reported missing: the engine has no file
// picker, so treating them as missing would block every submission of a page
// that uses them.
struct ValidityState
{
  bool value_missing = false;
  bool type_mismatch = false;
  bool pattern_mismatch = false; // NOT IMPLEMENTED (always false)
  bool too_long = false;
  bool too_short = false;
  bool range_underflow = false;
  bool range_overflow = false;
  bool step_mismatch = false;
  bool bad_input = false;
  bool custom_error = false;
  bool valid = true;

  // True when any constraint flag is set.  `valid` is true for elements
  // barred from constraint validation even when a custom message is set.
  bool HasError() const;

  bool operator==(const ValidityState&) const = default;
};

// The validity state plus the `validationMessage` text (empty when valid).
struct ControlValidity
{
  ValidityState state;
  std::string message;
};

// True for input/textarea/select/button (the submittable elements this engine
// implements).
bool IsSubmittableControl(const dom::Element& element);

// True when the element cannot be constrained: disabled (including a disabled
// fieldset ancestor, except inside its first legend), a datalist ancestor, a
// button element, or an input in the Hidden/Reset/Button/Submit/Image Button
// states.  readonly bars text-like inputs and textareas.
bool IsBarredFromConstraintValidation(const dom::Element& element);

// The negation of IsBarred..., for elements that are submittable controls.
bool IsCandidateForConstraintValidation(const dom::Element& element);

// Computes the live validity state of |element|.  |custom_error| is the
// element's custom validity error message (empty when none); the caller owns
// that script state.  For an element that is not a submittable control the
// result is valid (all flags false).
ControlValidity ValidateControl(const dom::Element& element, std::string_view custom_error = {});

// The control's owner form: the form named by its `form` attribute when the
// attribute is present, otherwise its nearest ancestor <form>.  The engine has
// no form-owner pointer in the DOM, so this is resolved on demand.
dom::Element* OwnerForm(const dom::Element& element);

// The submittable controls associated with |form| in tree order: descendants
// without a `form` attribute pointing elsewhere, followed by elements outside
// the form whose `form` attribute names it.  Buttons are included for
// completeness but are always barred from validation.
std::vector<dom::Element*> FormControls(const dom::Element& form);

// True when |element| is a submit button (WHATWG HTML §4.10.19.6):
//   * <button> with a missing type attribute or type=submit;
//   * <input type=submit> or <input type=image>.
// The type keywords are ASCII case-insensitive.  A disabled button is still a
// submit button; callers that implement activation check |disabled|
// separately.
bool IsSubmitButton(const dom::Element& element);

// The form's default button: the first submit button whose form owner is
// |form|, in tree order (WHATWG HTML §4.10.22.2 "Implicit submission"), or
// nullptr when the form has none.
dom::Element* DefaultButton(const dom::Element& form);

// One entry of a form's entry list (WHATWG HTML §4.10.22.4).  |is_file| marks
// an entry contributed by a file input; the engine has no File API, so such an
// entry carries no file content and the submission encoders skip it (the
// browser's form-submission path documents this gap).  |value| is empty for a
// file entry.
struct FormEntry
{
  std::string name;
  std::string value;
  bool is_file = false;
};

// Builds the entry list of |form| (WHATWG HTML §4.10.22.4), with the optional
// |submitter| included when it is a submit button.  Controls are visited in
// tree order; disabled controls, controls inside a <datalist>, unchecked
// checkboxes/radios, non-submitter buttons and image-button coordinates
// (NOT IMPLEMENTED) are omitted.  File inputs produce a file entry with no
// content.  |submitter| does not have to be a descendant of |form|: its form
// owner is used by the caller to decide whether it belongs to this submission.
std::vector<FormEntry> ConstructEntryList(const dom::Element& form,
                                          const dom::Element* submitter = nullptr);

// The controls of |form| whose computed validity is false.  |custom_error|
// optionally supplies each element's custom validity message (the JavaScript
// layer passes its per-element store; the browser layer does the same when a
// runtime exists).
using CustomValidityLookup = std::function<std::string(const dom::Element&)>;
std::vector<dom::Element*> InvalidControls(const dom::Element& form,
                                           const CustomValidityLookup& custom_error = {});

// True when every associated control is valid.  Does not fire events and does
// not consult the form's novalidate attribute (that only affects interactive
// validation during submission).
bool IsFormValid(const dom::Element& form, const CustomValidityLookup& custom_error = {});

} // namespace neko::html
