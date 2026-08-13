#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "neko/dom/element.h"

namespace neko::css {

// Selector model and matching (Phase 4 scope).
//
// Supported: type, universal, #id, .class (multiple), attribute selectors
// ([attr], [attr=op value], ~= |= ^= $= *=), pseudo-classes :first-child,
// :last-child, :nth-child(An+B), and combinators descendant / child / next
// sibling / subsequent sibling.

struct AttributeSelector {
  std::string name;             // lowercased
  std::string op;               // "" (exists), "=", "~=", "|=", "^=", "$=", "*="
  std::optional<std::string> value;
};

struct CompoundSelector {
  std::optional<std::string> tag;  // nullopt = universal
  std::optional<std::string> id;
  std::vector<std::string> classes;
  std::vector<AttributeSelector> attributes;
  std::vector<std::string> pseudo_classes;  // e.g. "first-child", "nth-child(2n+1)"
};

enum class Combinator { kDescendant, kChild, kNextSibling, kSubsequentSibling };

struct ComplexSelector {
  std::vector<CompoundSelector> compounds;
  std::vector<Combinator> combinators;  // size == compounds.size() - 1
};

// Specificity as (a, b, c): ids, classes/attributes/pseudo-classes, types.
struct Specificity {
  unsigned a = 0;
  unsigned b = 0;
  unsigned c = 0;

  bool operator<(const Specificity& other) const {
    return a != other.a ? a < other.a : (b != other.b ? b < other.b : c < other.c);
  }
  bool operator==(const Specificity& other) const {
    return a == other.a && b == other.b && c == other.c;
  }
};

// Parses a comma-separated selector list.  Returns empty on malformed input.
std::vector<ComplexSelector> ParseSelectorList(std::string_view text);

// True when |element| matches |selector| (any of its complex selectors).
bool MatchesSelector(const dom::Element& element, const ComplexSelector& selector);

// Specificity of the highest-specificity matching complex selector, or
// Specificity{0,0,0} when nothing matches.
Specificity MatchingSpecificity(const dom::Element& element, std::string_view selector_list);

}  // namespace neko::css
