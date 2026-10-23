#pragma once

#include "neko/dom/element.h"

#include <string>
#include <string_view>

namespace neko::html {

// <input> value semantics (WHATWG HTML §4.10.5.1 "States of the type
// attribute" and §4.10.5.5 "Common input element APIs").
//
// Each input type has its own "value sanitization algorithm"; the algorithms
// are implemented once, here, and every consumer shares them: the JavaScript
// value/defaultValue bindings, the keyboard editing path in the browser, the
// layout text of a control, constraint validation and the form entry-list
// builder.
//
// The model has two values:
//   * the `value` content attribute -- the raw "default value", exposed by the
//     defaultValue IDL attribute and never rewritten by the value setter;
//   * the current value -- stored on dom::Element::TextControlState when a
//     script or the user changed it (the "dirty value flag"), or when a type
//     change initialized it.  Without a stored current value it is derived
//     from the content attribute.
// Both are always read through the per-state sanitization algorithm, so an
// <input type=number value=abc> reads as "" while getAttribute('value') stays
// "abc".
//
// NOT IMPLEMENTED (deliberate, documented gaps):
//   * raw user input / `badInput`: a rejected keystroke is not stored, so a
//     number control cannot report badInput; typing a character that makes the
//     whole candidate invalid empties the value instead of rejecting the key.
//   * the File API: a file input's value is always the empty string, and the
//     value setter throws InvalidStateError for a non-empty value.
//   * date/time *arithmetic*: the sanitization algorithms validate and (for
//     datetime-local) normalize the string form only.  `valueAsDate` is absent
//     and no calendar conversion exists.
//   * `valueAsNumber`: not implemented.
//   * `textarea.defaultValue` (its text content editing model is separate and
//     unchanged by this module).
//   * a range default derived without a `value` attribute is recomputed when
//     min/max change while nothing is stored (browsers keep the previously
//     computed default and clamp it).  An explicit value, or a value set by
//     script/user, behaves exactly like a browser.

// The `type` attribute's state keyword: the missing value default and the
// invalid value default are both the Text state (HTML §4.10.5.3).
std::string InputTypeKeyword(const dom::Element& input);

// How the value IDL attribute behaves for a state keyword (HTML §4.10.5.5).
enum class InputValueMode
{
  kValue,     // Text/Search/.../Number/Range/Color/Hidden: current value
  kDefault,   // Submit/Reset/Button/Image: reflects the content attribute
  kDefaultOn, // Checkbox/Radio: reflects it, "on" when absent
  kFilename,  // File: always the empty string, setter throws on non-empty
};

InputValueMode InputValueModeFor(std::string_view type_keyword);

// The per-state value sanitization algorithm applied to |value| for |input|'s
// current type and attributes (min/max/step/multiple).  Idempotent for every
// state; returns the value the element should have.
std::string SanitizeInputValue(const dom::Element& input, std::string_view value);

// The input's current value (the value IDL getter): the sanitized stored
// current value, or the sanitization of the `value` content attribute.  For a
// non-input element this is dom::TextControlValue (textarea text content etc.).
std::string InputValue(const dom::Element& input);

// The value IDL setter for a value-mode input: sanitizes |value|, stores it as
// the current value and sets the dirty value flag.  It never writes the
// `value` content attribute.  For the default/default-on modes it reflects
// into the content attribute, as the standard specifies.
void SetInputValue(dom::Element& input, std::string_view value);

// The defaultValue IDL setter: writes the `value` content attribute and, when
// the control's value is not dirty, updates the current value too.
void SetInputDefaultValue(dom::Element& input, std::string_view value);

// Called after the `value` content attribute changed through setAttribute /
// removeAttribute / toggleAttribute.  When the value is not dirty the current
// value follows the attribute; a dirty value is left alone.
void InputValueAttributeChanged(dom::Element& input);

// Called after a type change with the previous state keyword and current
// value (both captured before the attribute changed), implementing the
// "when an input element's type attribute changes state" steps
// (HTML §4.10.5.3), then re-running the new state's sanitization algorithm.
void InputTypeChanged(dom::Element& input, std::string_view old_type, std::string_view old_value);

} // namespace neko::html
