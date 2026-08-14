#include "neko/css/selector.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neko::css {
namespace {

bool IsNameStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

bool IsNameChar(char c) { return IsNameStart(c) || (c >= '0' && c <= '9') || c == '-'; }

bool IsWhitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string ToLower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
  }
  return out;
}

// Splits a selector list on top-level commas.
std::vector<std::string_view> SplitOnCommas(std::string_view text) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  int depth = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '[' || c == '(') {
      ++depth;
    } else if (c == ']' || c == ')') {
      if (depth > 0) {
        --depth;
      }
    } else if (c == ',' && depth == 0) {
      parts.push_back(text.substr(start, i - start));
      start = i + 1;
    }
  }
  parts.push_back(text.substr(start));
  return parts;
}

// Parses a compound selector (no combinators).
bool ParseCompound(std::string_view text, CompoundSelector& out) {
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '*') {
      ++i;
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
    } else if (c == '[') {
      // attribute selector: [name], [name op "value"]
      std::size_t j = i + 1;
      while (j < text.size() && text[j] != ']') {
        ++j;
      }
      if (j >= text.size()) {
        return false;
      }
      const std::string_view inner = text.substr(i + 1, j - i - 1);
      // parse name [op value]
      std::size_t k = 0;
      while (k < inner.size() && inner[k] != '=' && inner[k] != ' ' && inner[k] != '\t' &&
             inner[k] != '~' && inner[k] != '|' && inner[k] != '^' && inner[k] != '$' &&
             inner[k] != '*') {
        ++k;
      }
      if (k == 0) {
        return false;
      }
      AttributeSelector attr;
      attr.name = ToLower(inner.substr(0, k));
      std::string_view rest = inner.substr(k);
      while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
        rest.remove_prefix(1);
      }
      if (rest.empty()) {
        attr.op = "";
      } else {
        if (rest.rfind("~=", 0) == 0) {
          attr.op = "~=";
        } else if (rest.rfind("|=", 0) == 0) {
          attr.op = "|=";
        } else if (rest.rfind("^=", 0) == 0) {
          attr.op = "^=";
        } else if (rest.rfind("$=", 0) == 0) {
          attr.op = "$=";
        } else if (rest.rfind("*=", 0) == 0) {
          attr.op = "*=";
        } else if (rest.rfind("=", 0) == 0) {
          attr.op = "=";
        } else {
          return false;
        }
        rest.remove_prefix(attr.op.size());
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
          rest.remove_prefix(1);
        }
        if (!rest.empty() && (rest.front() == '"' || rest.front() == '\'')) {
          const char quote = rest.front();
          rest.remove_prefix(1);
          const std::size_t end = rest.find(quote);
          if (end == std::string_view::npos) {
            return false;
          }
          attr.value = std::string(rest.substr(0, end));
        } else {
          attr.value = std::string(rest);
        }
      }
      out.attributes.push_back(std::move(attr));
      i = j + 1;
    } else if (c == ':') {
      std::size_t j = i + 1;
      while (j < text.size() && IsNameChar(text[j])) {
        ++j;
      }
      if (j == i + 1) {
        return false;
      }
      std::string pseudo(text.substr(i + 1, j - i - 1));
      if (j < text.size() && text[j] == '(') {
        std::size_t k = j + 1;
        while (k < text.size() && text[k] != ')') {
          ++k;
        }
        if (k >= text.size()) {
          return false;
        }
        pseudo += std::string(text.substr(j, k - j + 1));
        i = k + 1;
      } else {
        i = j;
      }
      out.pseudo_classes.push_back(std::move(pseudo));
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

// Parses a complex selector: compounds separated by combinators.
bool ParseComplex(std::string_view text, ComplexSelector& out) {
  std::size_t i = 0;
  for (;;) {
    while (i < text.size() && IsWhitespace(text[i])) {
      ++i;
    }
    if (i >= text.size()) {
      break;
    }
    const std::size_t start = i;
    int depth = 0;
    while (i < text.size()) {
      const char ch = text[i];
      if (ch == '[') {
        ++depth;
      } else if (ch == ']' && depth > 0) {
        --depth;
      } else if (ch == '(') {
        ++depth;
      } else if (ch == ')' && depth > 0) {
        --depth;
      }
      // Combinator characters only act as combinators outside brackets and
      // parentheses (e.g. the '+' inside :nth-child(2n+1)).
      if (depth == 0 && (IsWhitespace(ch) || ch == '>' || ch == '+' || ch == '~')) {
        break;
      }
      ++i;
    }
    CompoundSelector compound;
    if (!ParseCompound(text.substr(start, i - start), compound)) {
      return false;
    }
    out.compounds.push_back(std::move(compound));

    while (i < text.size() && IsWhitespace(text[i])) {
      ++i;
    }
    if (i >= text.size()) {
      break;
    }
    if (text[i] == '>') {
      out.combinators.push_back(Combinator::kChild);
      ++i;
    } else if (text[i] == '+') {
      out.combinators.push_back(Combinator::kNextSibling);
      ++i;
    } else if (text[i] == '~') {
      out.combinators.push_back(Combinator::kSubsequentSibling);
      ++i;
    } else {
      out.combinators.push_back(Combinator::kDescendant);
    }
  }
  return !out.compounds.empty() && out.combinators.size() == out.compounds.size() - 1;
}

const dom::Element* PreviousElementSibling(const dom::Node* node) {
  // Walk the parent's children, remembering the last element seen before
  // |node|.
  const dom::Node* parent = node->parent();
  if (parent == nullptr) {
    return nullptr;
  }
  const dom::Element* prev = nullptr;
  for (dom::Node* child : parent->ChildNodes()) {
    if (child == node) {
      return prev;
    }
    if (child->node_type() == dom::NodeType::kElement) {
      prev = static_cast<const dom::Element*>(child);
    }
  }
  return nullptr;
}

const dom::Element* NextElementSibling(const dom::Node* node) {
  const dom::Node* parent = node->parent();
  if (parent == nullptr) {
    return nullptr;
  }
  bool seen = false;
  for (dom::Node* child : parent->ChildNodes()) {
    if (seen && child->node_type() == dom::NodeType::kElement) {
      return static_cast<const dom::Element*>(child);
    }
    if (child == node) {
      seen = true;
    }
  }
  return nullptr;
}

// 1-based index among element siblings; 0 when there are no element siblings.
int ElementIndex(const dom::Node* node) {
  const dom::Node* parent = node->parent();
  if (parent == nullptr) {
    return 0;
  }
  int index = 0;
  for (dom::Node* child : parent->ChildNodes()) {
    if (child->node_type() != dom::NodeType::kElement) {
      continue;
    }
    ++index;
    if (child == node) {
      return index;
    }
  }
  return 0;
}

// Parses an+b (e.g. "2n+1", "-n+3", "odd", "even") into (a, b) with b as the
// 1-based matching positions offset.  Returns false for "n" alone which needs
// special handling.
bool ParseAnB(std::string_view text, int& a, int& b) {
  std::string_view t = text;
  while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) {
    t.remove_prefix(1);
  }
  while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) {
    t.remove_suffix(1);
  }
  if (t == "odd") {
    a = 2;
    b = 1;
    return true;
  }
  if (t == "even") {
    a = 2;
    b = 0;
    return true;
  }
  std::size_t i = 0;
  int sign = 1;
  if (i < t.size() && (t[i] == '+' || t[i] == '-')) {
    sign = (t[i] == '-') ? -1 : 1;
    ++i;
  }
  int number = 0;
  bool has_number = false;
  while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
    number = number * 10 + (t[i] - '0');
    has_number = true;
    ++i;
  }
  if (i < t.size() && (t[i] == 'n' || t[i] == 'N')) {
    ++i;
    a = sign * (has_number ? number : 1);
    int b_sign = 1;
    bool has_b = false;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) {
      b_sign = (t[i] == '-') ? -1 : 1;
      ++i;
      int b_number = 0;
      while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
        b_number = b_number * 10 + (t[i] - '0');
        has_b = true;
        ++i;
      }
      if (!has_b) {
        return false;
      }
      b = b_sign * b_number;
    } else {
      b = 0;
    }
    return i == t.size();
  }
  if (i == t.size()) {
    a = 0;
    b = sign * number;
    return has_number;
  }
  return false;
}

bool AttributeMatches(const dom::Element& element, const AttributeSelector& attr) {
  const std::optional<std::string_view> actual = element.GetAttribute(attr.name);
  if (!actual.has_value()) {
    return false;
  }
  if (attr.op.empty()) {
    return true;
  }
  const std::string& value = attr.value.value();
  const std::string_view text = actual.value();
  // With an empty value, only "=" can match (an attribute whose value is the
  // empty string).  The other operators (^= $= *= |= ~=) treat "" as a
  // substring/prefix/suffix of every string, which would make them match any
  // element carrying the attribute; the spec requires them to never match.
  if (value.empty() && attr.op != "=") {
    return false;
  }
  if (attr.op == "=") {
    return text == value;
  }
  if (attr.op == "~=") {
    std::size_t pos = 0;
    for (;;) {
      while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
        ++pos;
      }
      const std::size_t start = pos;
      while (pos < text.size() && text[pos] != ' ' && text[pos] != '\t') {
        ++pos;
      }
      if (pos > start && text.substr(start, pos - start) == value) {
        return true;
      }
      if (pos >= text.size()) {
        break;
      }
    }
    return false;
  }
  if (attr.op == "|=") {
    return text == value || (text.size() > value.size() && text.substr(0, value.size() + 1) ==
                                                                 value + "-");
  }
  if (attr.op == "^=") {
    return text.rfind(value, 0) == 0;
  }
  if (attr.op == "$=") {
    return text.size() >= value.size() &&
           text.substr(text.size() - value.size()) == value;
  }
  if (attr.op == "*=") {
    return text.find(value) != std::string_view::npos;
  }
  return false;
}

bool PseudoClassMatches(const dom::Element& element, std::string_view pseudo) {
  if (pseudo == "first-child") {
    return PreviousElementSibling(&element) == nullptr;
  }
  if (pseudo == "last-child") {
    return NextElementSibling(&element) == nullptr;
  }
  if (pseudo.rfind("nth-child", 0) == 0) {
    const std::size_t open = pseudo.find('(');
    const std::size_t close = pseudo.find(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
      return false;
    }
    int a = 0;
    int b = 0;
    if (!ParseAnB(pseudo.substr(open + 1, close - open - 1), a, b)) {
      return false;
    }
    const int index = ElementIndex(&element);
    if (a == 0) {
      return index == b;
    }
    const int diff = index - b;
    return diff % a == 0 && (a > 0 ? diff >= 0 : diff <= 0) && diff / a >= 0;
  }
  // Other pseudo-classes (:hover, :active, ...) are not matched yet; they are
  // treated as never matching to avoid incorrect styling.
  return false;
}

bool CompoundMatches(const dom::Element& element, const CompoundSelector& compound) {
  if (compound.tag.has_value() && *compound.tag != element.tag_name()) {
    return false;
  }
  if (compound.id.has_value()) {
    const std::optional<std::string_view> id = element.Id();
    if (!id.has_value() || *id != *compound.id) {
      return false;
    }
  }
  for (const std::string& cls : compound.classes) {
    const std::vector<std::string_view> classes = element.ClassList();
    bool found = false;
    for (const std::string_view c : classes) {
      if (c == cls) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  for (const AttributeSelector& attr : compound.attributes) {
    if (!AttributeMatches(element, attr)) {
      return false;
    }
  }
  for (const std::string& pseudo : compound.pseudo_classes) {
    if (!PseudoClassMatches(element, pseudo)) {
      return false;
    }
  }
  return true;
}

bool MatchOnElement(const dom::Element& element, const ComplexSelector& selector,
                    std::size_t index) {
  if (!CompoundMatches(element, selector.compounds[index])) {
    return false;
  }
  if (index == 0) {
    return true;
  }
  const Combinator combinator = selector.combinators[index - 1];
  const dom::Node* parent = element.parent();
  if (combinator == Combinator::kChild) {
    if (parent == nullptr || parent->node_type() != dom::NodeType::kElement) {
      return false;
    }
    return MatchOnElement(*static_cast<const dom::Element*>(parent), selector, index - 1);
  }
  if (combinator == Combinator::kNextSibling) {
    const dom::Element* prev = PreviousElementSibling(&element);
    return prev != nullptr && MatchOnElement(*prev, selector, index - 1);
  }
  if (combinator == Combinator::kSubsequentSibling) {
    const dom::Element* prev = PreviousElementSibling(&element);
    while (prev != nullptr) {
      if (MatchOnElement(*prev, selector, index - 1)) {
        return true;
      }
      prev = PreviousElementSibling(prev);
    }
    return false;
  }
  // Descendant.
  for (const dom::Node* ancestor = parent; ancestor != nullptr; ancestor = ancestor->parent()) {
    if (ancestor->node_type() == dom::NodeType::kElement &&
        MatchOnElement(*static_cast<const dom::Element*>(ancestor), selector, index - 1)) {
      return true;
    }
  }
  return false;
}

void CompoundSpecificity(const CompoundSelector& compound, Specificity& out) {
  if (compound.tag.has_value()) {
    ++out.c;
  }
  if (compound.id.has_value()) {
    ++out.a;
  }
  out.b += static_cast<unsigned>(compound.classes.size());
  out.b += static_cast<unsigned>(compound.attributes.size());
  out.b += static_cast<unsigned>(compound.pseudo_classes.size());
}

Specificity SelectorSpecificity(const ComplexSelector& selector) {
  Specificity out;
  for (const CompoundSelector& compound : selector.compounds) {
    CompoundSpecificity(compound, out);
  }
  return out;
}

}  // namespace

std::vector<ComplexSelector> ParseSelectorList(std::string_view text) {
  std::vector<ComplexSelector> result;
  for (const std::string_view part : SplitOnCommas(text)) {
    ComplexSelector selector;
    if (ParseComplex(part, selector)) {
      result.push_back(std::move(selector));
    }
  }
  return result;
}

bool MatchesSelector(const dom::Element& element, const ComplexSelector& selector) {
  return MatchOnElement(element, selector, selector.compounds.size() - 1);
}

Specificity MatchingSpecificity(const dom::Element& element, std::string_view selector_list) {
  Specificity best;
  for (const ComplexSelector& selector : ParseSelectorList(selector_list)) {
    if (MatchesSelector(element, selector)) {
      const Specificity spec = SelectorSpecificity(selector);
      if (best < spec) {
        best = spec;
      }
    }
  }
  return best;
}

}  // namespace neko::css
