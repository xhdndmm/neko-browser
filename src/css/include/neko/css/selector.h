#pragma once

// CSS selector matching now lives in neko::dom (see neko/dom/selector.h).
//
// Why: querySelector() is defined by the DOM standard and must share one
// selector parser/matcher with the style engine.  Keeping a second, smaller
// implementation in neko::dom made the two drift (querySelector did not
// understand :nth-child while the cascade did).  neko::css already depends on
// neko::dom, so the engine moved down instead of duplicating.
//
// This header keeps the historical neko::css spellings working; new code
// should include neko/dom/selector.h directly.

#include "neko/dom/selector.h"

namespace neko::css {

using dom::AttributeSelector;
using dom::Combinator;
using dom::ComplexSelector;
using dom::CompoundSelector;
using dom::MatchesSelector;
using dom::MatchingSpecificity;
using dom::MatchState;
using dom::ParseSelectorList;
using dom::SelectorSpecificity;
using dom::Specificity;

} // namespace neko::css
