#pragma once

#include "neko/css/selector.h"

#include <string>
#include <string_view>
#include <vector>

namespace neko::css {

// A single declaration: property: value [!important].
struct Declaration
{
  std::string property; // lowercased
  std::string value;    // raw value text (trimmed)
  bool important = false;
};

// A qualified rule: selector list + declarations.
struct StyleRule
{
  std::vector<ComplexSelector> selectors;
  std::vector<Declaration> declarations;
};

// An at-rule.  @media rules carry nested qualified rules; other at-rules
// (e.g. @import, @font-face) are stored but not interpreted yet.
struct AtRule
{
  std::string name;             // lowercased, without '@'
  std::string prelude;          // text up to '{' or ';'
  std::vector<StyleRule> rules; // nested rules (for @media)
};

struct StyleSheet
{
  std::vector<StyleRule> rules;
  std::vector<AtRule> at_rules;
};

} // namespace neko::css
