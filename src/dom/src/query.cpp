#include "neko/dom/query.h"

#include "neko/base/string_util.h"
#include "neko/dom/selector.h"

#include <string>
#include <string_view>
#include <vector>

namespace neko::dom {

bool MatchesCheckedPseudo(const Element& element)
{
  const std::string_view tag = element.tag_name();
  if (tag == "option") {
    return element.HasAttribute("selected");
  }
  if (tag != "input") {
    return false;
  }
  const std::string type = base::ToLower(std::string(element.GetAttribute("type").value_or("")));
  return (type == "checkbox" || type == "radio") && element.HasAttribute("checked");
}

namespace {

bool MatchesAny(const Element& element, const std::vector<ComplexSelector>& selectors)
{
  for (const ComplexSelector& selector : selectors) {
    if (MatchesSelector(element, selector, nullptr)) {
      return true;
    }
  }
  return false;
}

// Pre-order walk with the CSS tree-order contract (document order).  The root
// itself is handled by the callers: Node::ChildNodes() never yields the root.
void CollectMatches(Node& node,
                    const std::vector<ComplexSelector>& selectors,
                    std::vector<Element*>& out)
{
  for (Node* child : node.ChildNodes()) {
    if (child->node_type() != NodeType::kElement) {
      continue;
    }
    Element* element = static_cast<Element*>(child);
    if (MatchesAny(*element, selectors)) {
      out.push_back(element);
    }
    CollectMatches(*element, selectors, out);
  }
}

} // namespace

bool MatchesCompoundSelector(const Element& element, std::string_view selector)
{
  const std::vector<ComplexSelector> selectors = ParseSelectorList(selector);
  if (selectors.size() != 1 || selectors.front().compounds.size() != 1 ||
      !selectors.front().combinators.empty()) {
    return false;
  }
  return MatchesSelector(element, selectors.front(), nullptr);
}

Element* QuerySelector(Node& root, std::string_view selector)
{
  const std::vector<ComplexSelector> selectors = ParseSelectorList(selector);
  if (selectors.empty()) {
    return nullptr;
  }
  if (root.node_type() == NodeType::kElement) {
    Element* root_element = static_cast<Element*>(&root);
    if (MatchesAny(*root_element, selectors)) {
      return root_element;
    }
  }
  std::vector<Element*> matches;
  CollectMatches(root, selectors, matches);
  return matches.empty() ? nullptr : matches.front();
}

std::vector<Element*> QuerySelectorAll(Node& root, std::string_view selector)
{
  std::vector<Element*> matches;
  const std::vector<ComplexSelector> selectors = ParseSelectorList(selector);
  if (selectors.empty()) {
    return matches;
  }
  if (root.node_type() == NodeType::kElement) {
    Element* root_element = static_cast<Element*>(&root);
    if (MatchesAny(*root_element, selectors)) {
      matches.push_back(root_element);
    }
  }
  CollectMatches(root, selectors, matches);
  return matches;
}

} // namespace neko::dom
