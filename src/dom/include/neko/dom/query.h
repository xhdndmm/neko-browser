#pragma once

#include "neko/dom/element.h"

#include <string_view>
#include <vector>

namespace neko::dom {

// Query helpers supporting a practical subset of CSS selectors:
//
//   * <type>  #id  .class  .a.b  <compound>  <descendant>  <child>
//
// Full CSS selector matching (attribute selectors, pseudo-classes, ...) lives
// in neko::css; this subset covers the common cases used by scripts and tests.
// Malformed selectors yield no matches (never throw).

// First matching element in pre-order, or nullptr.
Element* QuerySelector(Node& root, std::string_view selector);

// All matching elements in document order.
std::vector<Element*> QuerySelectorAll(Node& root, std::string_view selector);

// Matches a single compound selector (no combinators) against an element.
bool MatchesCompoundSelector(const Element& element, std::string_view selector);

} // namespace neko::dom
