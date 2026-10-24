#pragma once

#include "neko/dom/element.h"

#include <string_view>
#include <vector>

namespace neko::dom {

// Query helpers over the shared CSS selector engine (neko/dom/selector.h).
//
// querySelector() is defined by the DOM standard, and the style engine must
// agree with it, so both use the same parser/matcher: type/universal, #id,
// .class, attribute selectors, pseudo-classes (`:first-child`, `:nth-child`,
// `:checked`, ...) and the descendant/child/sibling combinators.
// Malformed selectors yield no matches (never throw).  `:checked` is defined
// by MatchesCheckedPseudo, which the matcher uses -- one source of truth for
// checkedness.

// First matching element in pre-order, or nullptr.
Element* QuerySelector(Node& root, std::string_view selector);

// All matching elements in document order.
std::vector<Element*> QuerySelectorAll(Node& root, std::string_view selector);

// Matches a single compound selector (no combinators) against an element.
bool MatchesCompoundSelector(const Element& element, std::string_view selector);

// True for the `:checked` pseudo-class (HTML §4.10.5.3 + Selectors 4): a
// checkable input's checkedness or a selected <option>.  This engine models
// checkedness as the `checked` content attribute -- the same source the JS
// `checked` binding and the widget paint read -- unlike the spec where the
// attribute only initialises defaultChecked.  The input `type` is an ASCII
// case-insensitive enumerated attribute (§15.5.1).
bool MatchesCheckedPseudo(const Element& element);

} // namespace neko::dom
