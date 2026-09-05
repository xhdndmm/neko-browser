#pragma once

#include "neko/css/selector.h"

#include <optional>
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

// One @font-face rule: a downloadable font registered under |family| with an
// optional weight/style restriction.  |src_url| is the first usable url()
// from the src descriptor (format hints woff2 > woff > ttf/otf preferred).
struct FontFaceRule
{
  std::string family;
  std::string src_url;
  int weight = 400;
  bool italic = false;
};

// An at-rule.  @media rules carry nested qualified rules; declaration-block
// at-rules (e.g. @font-face) keep their raw block text in |block| and, for
// @font-face, the parsed rule in |font_face|.
struct AtRule
{
  std::string name;                      // lowercased, without '@'
  std::string prelude;                   // text up to '{' or ';'
  std::string block;                     // raw declaration-block text (@font-face etc.)
  std::optional<FontFaceRule> font_face; // set for @font-face when valid
  std::vector<StyleRule> rules;          // nested rules (for @media)
};

struct StyleSheet
{
  std::vector<StyleRule> rules;
  std::vector<AtRule> at_rules;
  std::vector<FontFaceRule> font_faces;
};

// Serializes a complex selector (the compound chain, with combinators) back
// to CSS text.  Used by the CSSOM cssRules selectorText.  Supported features
// degrade gracefully: anything the parser could not represent is omitted
// rather than throwing.  A universal (empty) compound serializes as "*".
std::string ToString(const ComplexSelector& selector);

// Serializes one qualified rule as "selector { prop: value [!important]; ... }".
std::string SerializeStyleRule(const StyleRule& rule);

// Serializes a whole stylesheet back to CSS text (rules then at-rules).
std::string SerializeStyleSheet(const StyleSheet& sheet);

} // namespace neko::css
