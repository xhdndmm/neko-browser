#include "neko/css/stylesheet.h"

#include <string>

namespace neko::css {
namespace {

// Serializes one compound selector (no combinators).
std::string ToString(const CompoundSelector& compound)
{
  std::string out;
  if (compound.tag.has_value()) {
    out += *compound.tag;
  } else if (!compound.id.has_value() && compound.classes.empty() && compound.attributes.empty() &&
             compound.pseudo_classes.empty()) {
    // A compound with no components at all is the universal selector.
    out += '*';
  }
  if (compound.id.has_value()) {
    out += '#';
    out += *compound.id;
  }
  for (const std::string& cls : compound.classes) {
    out += '.';
    out += cls;
  }
  for (const AttributeSelector& attr : compound.attributes) {
    out += '[';
    out += attr.name;
    if (!attr.op.empty()) {
      out += attr.op;
      if (attr.value.has_value()) {
        out += '"';
        out += *attr.value;
        out += '"';
      }
    }
    out += ']';
  }
  for (const std::string& pseudo : compound.pseudo_classes) {
    out += ':';
    out += pseudo;
  }
  return out;
}

std::string CombinatorText(Combinator combinator)
{
  switch (combinator) {
  case Combinator::kDescendant:
    return " ";
  case Combinator::kChild:
    return " > ";
  case Combinator::kNextSibling:
    return " + ";
  case Combinator::kSubsequentSibling:
    return " ~ ";
  }
  return " ";
}

} // namespace

std::string ToString(const ComplexSelector& selector)
{
  std::string out;
  for (std::size_t i = 0; i < selector.compounds.size(); ++i) {
    if (i > 0) {
      out += CombinatorText(selector.combinators[i - 1]);
    }
    out += ToString(selector.compounds[i]);
  }
  return out;
}

std::string SerializeStyleRule(const StyleRule& rule)
{
  std::string out;
  for (std::size_t i = 0; i < rule.selectors.size(); ++i) {
    if (i > 0) {
      out += ", ";
    }
    out += ToString(rule.selectors[i]);
  }
  out += " { ";
  for (const Declaration& declaration : rule.declarations) {
    out += declaration.property;
    out += ": ";
    out += declaration.value;
    if (declaration.important) {
      out += " !important";
    }
    out += "; ";
  }
  out += "}";
  return out;
}

std::string SerializeStyleSheet(const StyleSheet& sheet)
{
  std::string out;
  for (const StyleRule& rule : sheet.rules) {
    out += SerializeStyleRule(rule);
    out += "\n";
  }
  for (const AtRule& at : sheet.at_rules) {
    out += '@';
    out += at.name;
    if (!at.prelude.empty()) {
      out += ' ';
      out += at.prelude;
    }
    out += " { ";
    for (const StyleRule& rule : at.rules) {
      out += SerializeStyleRule(rule);
    }
    if (!at.block.empty()) {
      out += at.block;
    }
    out += " }\n";
  }
  return out;
}

} // namespace neko::css
