#include "neko/dom/query.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neko::dom {
namespace {

bool IsNameStart(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool IsNameChar(char c)
{
  return IsNameStart(c) || (c >= '0' && c <= '9') || c == '-';
}

bool IsWhitespace(char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string ToLower(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
  }
  return out;
}

struct Compound
{
  std::optional<std::string> tag; // lowercased
  std::optional<std::string> id;
  std::vector<std::string> classes;
  struct Attribute
  {
    std::string name;
    std::string op;
    std::string value;
  };
  std::vector<Attribute> attributes;
};

enum class Combinator
{
  kDescendant,
  kChild
};

struct SelectorChain
{
  std::vector<Compound> compounds;
  std::vector<Combinator> combinators; // size == compounds.size() - 1
};

// Parses a single compound selector like "div#main.a.b".
bool ParseCompound(std::string_view text, Compound& out)
{
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '[') {
      const std::size_t close = text.find(']', i + 1);
      if (close == std::string_view::npos) {
        return false;
      }
      std::string_view inner = text.substr(i + 1, close - i - 1);
      while (!inner.empty() && IsWhitespace(inner.front())) {
        inner.remove_prefix(1);
      }
      while (!inner.empty() && IsWhitespace(inner.back())) {
        inner.remove_suffix(1);
      }
      std::size_t name_end = 0;
      while (name_end < inner.size() && IsNameChar(inner[name_end])) {
        ++name_end;
      }
      if (name_end == 0) {
        return false;
      }
      Compound::Attribute attribute;
      attribute.name = ToLower(inner.substr(0, name_end));
      inner.remove_prefix(name_end);
      while (!inner.empty() && IsWhitespace(inner.front())) {
        inner.remove_prefix(1);
      }
      if (!inner.empty()) {
        constexpr std::string_view operators[] = {"~=", "|=", "^=", "$=", "*=", "="};
        for (const std::string_view candidate : operators) {
          if (inner.starts_with(candidate)) {
            attribute.op = candidate;
            inner.remove_prefix(candidate.size());
            break;
          }
        }
        if (attribute.op.empty()) {
          return false;
        }
        while (!inner.empty() && IsWhitespace(inner.front())) {
          inner.remove_prefix(1);
        }
        if (inner.size() >= 2 && (inner.front() == '\'' || inner.front() == '"')) {
          if (inner.back() != inner.front()) {
            return false;
          }
          inner.remove_prefix(1);
          inner.remove_suffix(1);
        }
        attribute.value = std::string(inner);
      }
      out.attributes.push_back(std::move(attribute));
      i = close + 1;
    } else if (c == '#') {
      std::size_t j = i + 1;
      while (j < text.size() && IsNameChar(text[j])) {
        ++j;
      }
      if (j == i + 1 || out.id.has_value()) {
        return false;
      }
      out.id = std::string(text.substr(i + 1, j - i - 1));
      i = j;
    } else if (c == '.') {
      std::size_t j = i + 1;
      while (j < text.size() && IsNameChar(text[j])) {
        ++j;
      }
      if (j == i + 1) {
        return false;
      }
      out.classes.emplace_back(text.substr(i + 1, j - i - 1));
      i = j;
    } else if (c == '*') {
      ++i;
    } else if (IsNameStart(c)) {
      std::size_t j = i;
      while (j < text.size() && IsNameChar(text[j])) {
        ++j;
      }
      if (out.tag.has_value()) {
        return false;
      }
      out.tag = ToLower(text.substr(i, j - i));
      i = j;
    } else {
      return false;
    }
  }
  return true;
}

// Parses a full selector chain ("div > p .note").
bool ParseSelector(std::string_view selector, SelectorChain& chain)
{
  std::size_t i = 0;
  for (;;) {
    while (i < selector.size() && IsWhitespace(selector[i])) {
      ++i;
    }
    if (i >= selector.size()) {
      break;
    }
    const std::size_t start = i;
    while (i < selector.size() && !IsWhitespace(selector[i]) && selector[i] != '>') {
      ++i;
    }
    Compound compound;
    if (!ParseCompound(selector.substr(start, i - start), compound)) {
      return false;
    }
    chain.compounds.push_back(std::move(compound));

    while (i < selector.size() && IsWhitespace(selector[i])) {
      ++i;
    }
    if (i >= selector.size()) {
      break;
    }
    if (selector[i] == '>') {
      chain.combinators.push_back(Combinator::kChild);
      ++i;
    } else {
      chain.combinators.push_back(Combinator::kDescendant);
    }
  }
  return !chain.compounds.empty() && chain.combinators.size() == chain.compounds.size() - 1;
}

bool CompoundMatches(const Element& element, const Compound& compound)
{
  if (compound.tag.has_value() && *compound.tag != element.tag_name()) {
    return false;
  }
  if (compound.id.has_value()) {
    const std::optional<std::string_view> id = element.Id();
    if (!id.has_value() || *id != *compound.id) {
      return false;
    }
  }
  for (const Compound::Attribute& attribute : compound.attributes) {
    const std::optional<std::string_view> value = element.GetAttribute(attribute.name);
    if (!value.has_value()) {
      return false;
    }
    if (attribute.op.empty()) {
      continue;
    }
    if ((attribute.op == "^=" || attribute.op == "$=" || attribute.op == "*=" ||
         attribute.op == "~=" || attribute.op == "|=") &&
        attribute.value.empty()) {
      return false;
    }
    if (attribute.op == "=" && *value != attribute.value) {
      return false;
    }
    if (attribute.op == "^=" && !value->starts_with(attribute.value)) {
      return false;
    }
    if (attribute.op == "$=" && !value->ends_with(attribute.value)) {
      return false;
    }
    if (attribute.op == "*=" && value->find(attribute.value) == std::string_view::npos) {
      return false;
    }
    if (attribute.op == "|=" && *value != attribute.value &&
        !value->starts_with(attribute.value + "-")) {
      return false;
    }
    if (attribute.op == "~=") {
      bool found = false;
      std::size_t start = 0;
      while (start < value->size()) {
        while (start < value->size() && IsWhitespace((*value)[start])) {
          ++start;
        }
        std::size_t end = start;
        while (end < value->size() && !IsWhitespace((*value)[end])) {
          ++end;
        }
        found = found || value->substr(start, end - start) == attribute.value;
        start = end;
      }
      if (!found) {
        return false;
      }
    }
  }
  if (!compound.classes.empty()) {
    const std::vector<std::string_view> classes = element.ClassList();
    for (const std::string& wanted : compound.classes) {
      if (std::find(classes.begin(), classes.end(), wanted) == classes.end()) {
        return false;
      }
    }
  }
  return true;
}

// Matches the element against compound |index| (walking backwards through the
// chain for combinators).
bool MatchOnElement(const Element& element, const SelectorChain& chain, std::size_t index)
{
  if (!CompoundMatches(element, chain.compounds[index])) {
    return false;
  }
  if (index == 0) {
    return true;
  }
  const Combinator comb = chain.combinators[index - 1];
  if (comb == Combinator::kChild) {
    const Node* parent = element.parent();
    if (parent == nullptr || parent->node_type() != NodeType::kElement) {
      return false;
    }
    return MatchOnElement(*static_cast<const Element*>(parent), chain, index - 1);
  }
  for (const Node* ancestor = element.parent(); ancestor != nullptr;
       ancestor = ancestor->parent()) {
    if (ancestor->node_type() == NodeType::kElement &&
        MatchOnElement(*static_cast<const Element*>(ancestor), chain, index - 1)) {
      return true;
    }
  }
  return false;
}

void CollectMatches(Node& node, const SelectorChain& chain, std::vector<Element*>& out)
{
  for (Node* child : node.ChildNodes()) {
    if (child->node_type() != NodeType::kElement) {
      continue;
    }
    Element* element = static_cast<Element*>(child);
    if (MatchOnElement(*element, chain, chain.compounds.size() - 1)) {
      out.push_back(element);
    }
    CollectMatches(*element, chain, out);
  }
}

} // namespace

bool MatchesCompoundSelector(const Element& element, std::string_view selector)
{
  Compound compound;
  return ParseCompound(selector, compound) && CompoundMatches(element, compound);
}

Element* QuerySelector(Node& root, std::string_view selector)
{
  SelectorChain chain;
  if (!ParseSelector(selector, chain)) {
    return nullptr;
  }
  // The root itself may match.
  if (root.node_type() == NodeType::kElement) {
    Element* root_element = static_cast<Element*>(&root);
    if (MatchOnElement(*root_element, chain, chain.compounds.size() - 1)) {
      return root_element;
    }
  }
  std::vector<Element*> matches;
  CollectMatches(root, chain, matches);
  return matches.empty() ? nullptr : matches.front();
}

std::vector<Element*> QuerySelectorAll(Node& root, std::string_view selector)
{
  std::vector<Element*> matches;
  SelectorChain chain;
  if (!ParseSelector(selector, chain)) {
    return matches;
  }
  if (root.node_type() == NodeType::kElement) {
    Element* root_element = static_cast<Element*>(&root);
    if (MatchOnElement(*root_element, chain, chain.compounds.size() - 1)) {
      matches.push_back(root_element);
    }
  }
  CollectMatches(root, chain, matches);
  return matches;
}

} // namespace neko::dom
