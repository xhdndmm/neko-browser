#pragma once

#include "neko/dom/element.h"

namespace neko::layout {

// WHATWG HTML §15.5 Widgets: which kind of form control an element is.
//
// The kind is what drives the widget geometry in the layout engine
// (LayoutBox::form_control) and the widget drawing in the paint layer; every
// element that is not a form control is kNone.
enum class FormControlKind
{
  kNone,     // not a form control
  kHidden,   // input[type=hidden]: no box at all (§15.3.10 display:none)
  kText,     // the text-entry states (§15.5.6) and the domain-specific widgets
             // (§15.5.7) -- and the Text state that an unknown `type` falls
             // back to
  kButton,   // <button>, input[type=submit|reset|button|image] (§15.5.4,
             // §15.5.12)
  kCheckbox, // input[type=checkbox] (§15.5.10)
  kRadio,    // input[type=radio] (§15.5.10)
  kRange,    // input[type=range] (§15.5.8)
  kColor,    // input[type=color] (§15.5.9)
  kFile,     // input[type=file] (§15.5.11)
  kSelect,   // <select> (§15.5.16)
  kTextArea, // <textarea> (§15.5.17)
};

// Classifies |element| from its tag name and its `type` attribute.  The `type`
// attribute is an ASCII case-insensitive enumerated attribute -- the standard
// spells its own selectors `input[type=checkbox i]` -- so `TYPE="CHECKBOX"`
// classifies as kCheckbox.  A missing attribute is the Text state (§4.10.5.3:
// the missing value default and the invalid value default are both the Text
// state), and so is an unknown keyword.
//
// The kind is what the paint layer dispatches on.  The **drawn** widgets
// (kCheckbox, kRadio, kRange, kColor, kFile) take their native geometry as a
// border-box size -- a checkbox is 13x13 and a colour well 64x32 whatever the
// author's border and padding, as the platform controls are -- so their layout
// content box is that target minus the chrome.  The text-bearing kinds
// (kText, kTextArea, kSelect, kButton) size from their content and add the
// chrome on top, as text fields do.
FormControlKind ClassifyFormControl(const dom::Element& element);

// True when |element| has an inner display type of flow-root.  §15.3.10's final
// rule applies to input elements whose `type` is neither in the Hidden state nor
// in the Image Button state; no other widget gets flow-root from it.  Exposed
// for the widget layers: the engine renders a control as one atomic widget and
// lays no control content out, so layout itself does not read this yet.
bool HasFlowRootInnerDisplay(const dom::Element& element);

} // namespace neko::layout
