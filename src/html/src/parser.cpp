#include "neko/html/parser.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace neko::html {
namespace {

bool IsAllWhitespace(std::string_view text)
{
  for (const char c : text) {
    if (!IsAsciiWhitespace(c)) {
      return false;
    }
  }
  return !text.empty();
}

bool IsHeadingTag(std::string_view tag)
{
  return tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" || tag == "h5" || tag == "h6";
}

// Elements in the WHATWG "formatting" category (13.2.4.3).
bool IsFormattingElement(std::string_view tag)
{
  return tag == "a" || tag == "b" || tag == "big" || tag == "code" || tag == "em" ||
         tag == "font" || tag == "i" || tag == "nobr" || tag == "s" || tag == "small" ||
         tag == "strike" || tag == "strong" || tag == "tt" || tag == "u";
}

// Elements in the WHATWG "special" category (used by the adoption agency).
bool IsSpecialElement(std::string_view tag)
{
  return tag == "address" || tag == "applet" || tag == "area" || tag == "article" ||
         tag == "aside" || tag == "base" || tag == "basefont" || tag == "bgsound" ||
         tag == "blockquote" || tag == "body" || tag == "br" || tag == "button" ||
         tag == "caption" || tag == "center" || tag == "col" || tag == "colgroup" || tag == "dd" ||
         tag == "details" || tag == "dir" || tag == "div" || tag == "dl" || tag == "dt" ||
         tag == "embed" || tag == "fieldset" || tag == "figcaption" || tag == "figure" ||
         tag == "footer" || tag == "form" || tag == "frame" || tag == "frameset" || tag == "h1" ||
         tag == "h2" || tag == "h3" || tag == "h4" || tag == "h5" || tag == "h6" || tag == "head" ||
         tag == "header" || tag == "hgroup" || tag == "hr" || tag == "html" || tag == "iframe" ||
         tag == "img" || tag == "input" || tag == "keygen" || tag == "li" || tag == "link" ||
         tag == "listing" || tag == "main" || tag == "marquee" || tag == "menu" || tag == "meta" ||
         tag == "nav" || tag == "noembed" || tag == "noframes" || tag == "noscript" ||
         tag == "object" || tag == "ol" || tag == "p" || tag == "param" || tag == "plaintext" ||
         tag == "pre" || tag == "script" || tag == "section" || tag == "select" ||
         tag == "source" || tag == "style" || tag == "summary" || tag == "table" ||
         tag == "tbody" || tag == "td" || tag == "template" || tag == "textarea" ||
         tag == "tfoot" || tag == "th" || tag == "thead" || tag == "title" || tag == "tr" ||
         tag == "track" || tag == "ul" || tag == "wbr" || tag == "xmp";
}

// True when two elements have the same set of parsed attributes (name + value,
// order-independent).  The namespace is always HTML in this parser, so it is
// not compared.
bool SameAttributes(const dom::Element& a, const dom::Element& b)
{
  const auto& aa = a.attributes();
  const auto& bb = b.attributes();
  if (aa.size() != bb.size()) {
    return false;
  }
  for (const dom::Attribute& attr : aa) {
    bool found = false;
    for (const dom::Attribute& other : bb) {
      if (other.name == attr.name) {
        if (other.value != attr.value) {
          return false;
        }
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

// Builds an element from a start tag token, copying its attributes.
std::unique_ptr<dom::Element> CreateElement(const Token& token)
{
  auto element = std::make_unique<dom::Element>(token.name);
  for (const Attribute& attr : token.attributes) {
    element->SetAttribute(attr.name, attr.value);
  }
  return element;
}

// The start-tag set shared by the "in caption" and "in cell" insertion modes
// (13.2.6.4.11 / 13.2.6.4.15): elements that end the caption / cell.  Note
// that "table" is deliberately absent — a nested table is allowed inside a
// cell.
bool IsTableCaptionColumn(std::string_view tag)
{
  return tag == "caption" || tag == "col" || tag == "colgroup" || tag == "tbody" || tag == "td" ||
         tag == "tfoot" || tag == "th" || tag == "thead" || tag == "tr";
}

} // namespace

Parser::Parser(std::string_view html) : html_(html), tokenizer_(html) {}

std::unique_ptr<dom::Document> Parser::Parse()
{
  document_ = std::make_unique<dom::Document>();
  for (;;) {
    Token token = NextToken();
    if (token.type == TokenType::kEOF) {
      // Ensure the document skeleton (html/head/body) exists at EOF, mirroring
      // the WHATWG 'anything else' rules for each mode.
      while (mode_ != Mode::kInBody) {
        switch (mode_) {
        case Mode::kInitial:
          mode_ = Mode::kBeforeHtml;
          break;
        case Mode::kBeforeHtml:
          InsertElement(new dom::Element("html"));
          mode_ = Mode::kBeforeHead;
          break;
        case Mode::kBeforeHead:
          InsertElement(new dom::Element("head"));
          mode_ = Mode::kInHead;
          break;
        case Mode::kInHead:
          PopElement(); // implicit close of head
          mode_ = Mode::kAfterHead;
          break;
        case Mode::kAfterHead:
          InsertElement(new dom::Element("body"));
          mode_ = Mode::kInBody;
          break;
        case Mode::kText:
          PopElement();
          mode_ = mode_before_text_;
          break;
        case Mode::kAfterBody:
        case Mode::kAfterAfterBody:
          mode_ = Mode::kInBody;
          break;
        case Mode::kInTable:
        case Mode::kInTableText:
        case Mode::kInCaption:
        case Mode::kInColumnGroup:
        case Mode::kInTableBody:
        case Mode::kInRow:
        case Mode::kInCell:
          // EOF in a table insertion mode is processed with the "in body"
          // rules (13.2.6.4.9), which simply stop parsing here.
          mode_ = Mode::kInBody;
          break;
        case Mode::kInBody:
          break;
        }
      }
      break;
    }
    ProcessToken(std::move(token));
  }
  return std::move(document_);
}

Token Parser::NextToken()
{
  if (pending_.has_value()) {
    Token token = std::move(pending_.value());
    pending_.reset();
    return token;
  }
  return tokenizer_.Next();
}

void Parser::Reprocess(Token token)
{
  pending_ = std::move(token);
}

dom::Element* Parser::CurrentNode()
{
  return stack_.empty() ? nullptr : stack_.back();
}

void Parser::AppendNode(std::unique_ptr<dom::Node> node)
{
  auto [parent, before] = AdjustedInsertionLocation();
  InsertNodeAt(parent, before, std::move(node));
}

void Parser::InsertElement(dom::Element* element)
{
  // Depth guard: drop over-deep subtrees instead of building a DOM so deep
  // that the recursive style/layout walks overflow the stack.
  if (stack_.size() >= kMaxDepth) {
    delete element;
    skip_depth_ = 1;
    return;
  }
  auto [parent, before] = AdjustedInsertionLocation();
  InsertNodeAt(parent, before, std::unique_ptr<dom::Node>(element));
  stack_.push_back(element);
}

void Parser::PopElement()
{
  if (skip_depth_ > 0) {
    --skip_depth_; // keep the real stack balanced while discarding
    return;
  }
  stack_.pop_back();
}

void Parser::PopThrough(std::string_view tag)
{
  while (!stack_.empty()) {
    dom::Element* element = stack_.back();
    PopElement();
    if (element->tag_name() == tag) {
      break;
    }
  }
}

void Parser::AppendText(std::string_view text)
{
  auto [parent, before] = AdjustedInsertionLocation();
  AppendTextAt(parent, before, text);
}

// Whether |tag| terminates scope searches.  These are the WHATWG 13.2.6 scope
// boundaries for the default scope (applet, caption, html, table, td, th,
// marquee, object, template) plus the MathML/SVG ones we do not create.  The
// HTML parser never produces SVG/MathML elements, so those boundaries are
// omitted.
bool IsScopeBoundaryTag(std::string_view tag)
{
  return tag == "applet" || tag == "caption" || tag == "html" || tag == "table" || tag == "td" ||
         tag == "th" || tag == "marquee" || tag == "object" || tag == "template";
}

bool Parser::InScope(std::string_view tag) const
{
  for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
    const std::string_view current = (*it)->tag_name();
    if (current == tag) {
      return true;
    }
    if (IsScopeBoundaryTag(current)) {
      return false;
    }
  }
  return false;
}

// Button scope (WHATWG 13.2.6): the default scope boundaries plus `button`.
// The "close a p element" step runs in button scope, so an open <p> nested
// inside a <button> must NOT be closed by a following break-out start tag
// (e.g. <div>), whereas a <p> inside an <ol>/<ul>/<li> still is.
bool Parser::InButtonScope(std::string_view tag) const
{
  for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
    const std::string_view current = (*it)->tag_name();
    if (current == tag) {
      return true;
    }
    if (IsScopeBoundaryTag(current) || current == "button") {
      return false;
    }
  }
  return false;
}

dom::Element* Parser::FindInStack(std::string_view tag) const
{
  for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
    if ((*it)->tag_name() == tag) {
      return *it;
    }
  }
  return nullptr;
}

void Parser::ClosePElement()
{
  // "close a p element": only if the open <p> is in button scope.
  if (InButtonScope("p")) {
    PopThrough("p");
  }
}

bool Parser::IsVoidElement(std::string_view tag) const
{
  return tag == "area" || tag == "base" || tag == "basefont" || tag == "bgsound" || tag == "br" ||
         tag == "col" || tag == "embed" || tag == "hr" || tag == "img" || tag == "input" ||
         tag == "keygen" || tag == "link" || tag == "meta" || tag == "param" || tag == "source" ||
         tag == "track" || tag == "wbr";
}

bool Parser::IsBlockElement(std::string_view tag) const
{
  return tag == "address" || tag == "article" || tag == "aside" || tag == "blockquote" ||
         tag == "body" || tag == "caption" || tag == "dd" || tag == "details" || tag == "dialog" ||
         tag == "div" || tag == "dl" || tag == "dt" || tag == "fieldset" || tag == "figcaption" ||
         tag == "figure" || tag == "footer" || tag == "form" || tag == "h1" || tag == "h2" ||
         tag == "h3" || tag == "h4" || tag == "h5" || tag == "h6" || tag == "header" ||
         tag == "hgroup" || tag == "hr" || tag == "li" || tag == "main" || tag == "menu" ||
         tag == "nav" || tag == "ol" || tag == "p" || tag == "pre" || tag == "section" ||
         tag == "summary" || tag == "table" || tag == "tbody" || tag == "td" || tag == "tfoot" ||
         tag == "th" || tag == "thead" || tag == "tr" || tag == "ul";
}

// "Table scope" boundaries (13.2.6.1): html, table, template.
bool Parser::InTableScope(std::string_view tag) const
{
  for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
    const std::string_view current = (*it)->tag_name();
    if (current == tag) {
      return true;
    }
    if (current == "html" || current == "table" || current == "template") {
      return false;
    }
  }
  return false;
}

// "List item scope" boundaries (13.2.6.1): ol and ul, in addition to the
// default scope boundaries.
bool Parser::InListItemScope(std::string_view tag) const
{
  for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
    const std::string_view current = (*it)->tag_name();
    if (current == tag) {
      return true;
    }
    if (current == "ol" || current == "ul") {
      return false;
    }
    if (IsScopeBoundaryTag(current)) {
      return false;
    }
  }
  return false;
}

// "Generate implied end tags" (13.2.6.3): pop dd, dt, li, optgroup, option,
// p, rb, rp, rt, rtc while the current node is one of those.  The optional
// |except| element is left in place.
void Parser::GenerateImpliedEndTags(std::optional<std::string_view> except)
{
  while (!stack_.empty()) {
    const std::string_view tag = stack_.back()->tag_name();
    const bool implied = tag == "dd" || tag == "dt" || tag == "li" || tag == "optgroup" ||
                         tag == "option" || tag == "p" || tag == "rb" || tag == "rp" ||
                         tag == "rt" || tag == "rtc";
    if (!implied) {
      break;
    }
    if (except.has_value() && tag == except.value()) {
      break;
    }
    PopElement();
  }
}

// "Clear the stack back to a table context" (13.2.6.4.9).
void Parser::ClearStackBackToTableContext()
{
  while (!stack_.empty()) {
    const std::string_view tag = stack_.back()->tag_name();
    if (tag == "table" || tag == "template" || tag == "html") {
      break;
    }
    PopElement();
  }
}

// "Clear the stack back to a table body context" (13.2.6.4.13).
void Parser::ClearStackBackToTableBodyContext()
{
  while (!stack_.empty()) {
    const std::string_view tag = stack_.back()->tag_name();
    if (tag == "tbody" || tag == "tfoot" || tag == "thead" || tag == "template" || tag == "html") {
      break;
    }
    PopElement();
  }
}

// "Clear the stack back to a table row context" (13.2.6.4.14).
void Parser::ClearStackBackToTableRowContext()
{
  while (!stack_.empty()) {
    const std::string_view tag = stack_.back()->tag_name();
    if (tag == "tr" || tag == "template" || tag == "html") {
      break;
    }
    PopElement();
  }
}

// "Close the cell" (13.2.6.4.15).
void Parser::CloseTableCell()
{
  GenerateImpliedEndTags();
  while (!stack_.empty()) {
    const std::string_view tag = stack_.back()->tag_name();
    PopElement();
    if (tag == "td" || tag == "th") {
      break;
    }
  }
  ClearActiveFormattingToMarker();
  mode_ = Mode::kInRow;
}

// "Reset the insertion mode appropriately" (13.2.6.1).  Tables restore the
// insertion mode by walking the stack of open elements.
void Parser::ResetInsertionMode()
{
  bool last = false;
  for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
    dom::Element* node = *it;
    if (it + 1 == stack_.rend()) {
      last = true;
    }
    const std::string_view tag = node->tag_name();
    if ((tag == "td" || tag == "th") && !last) {
      mode_ = Mode::kInCell;
      return;
    }
    if (tag == "tr") {
      mode_ = Mode::kInRow;
      return;
    }
    if (tag == "tbody" || tag == "thead" || tag == "tfoot") {
      mode_ = Mode::kInTableBody;
      return;
    }
    if (tag == "caption") {
      mode_ = Mode::kInCaption;
      return;
    }
    if (tag == "colgroup") {
      mode_ = Mode::kInColumnGroup;
      return;
    }
    if (tag == "table") {
      mode_ = Mode::kInTable;
      return;
    }
    if (tag == "body") {
      mode_ = Mode::kInBody;
      return;
    }
    if (tag == "html") {
      mode_ = head_element_ != nullptr ? Mode::kAfterHead : Mode::kBeforeHead;
      return;
    }
    if (last) {
      mode_ = Mode::kInBody;
      return;
    }
  }
}

void Parser::PushFormattingMarker()
{
  active_formatting_.push_back(nullptr);
}

// "Clear the list of active formatting elements up to the last marker"
// (13.2.6.4.9).
void Parser::ClearActiveFormattingToMarker()
{
  while (!active_formatting_.empty()) {
    dom::Element* entry = active_formatting_.back();
    active_formatting_.pop_back();
    if (entry == nullptr) {
      break;
    }
  }
}

// The adjusted insertion location (13.2.6.1).  When foster parenting is
// enabled and the current node is a table, tbody, tfoot, thead or tr, content
// is inserted before the table instead of inside it.
std::pair<dom::Node*, dom::Node*> Parser::AdjustedInsertionLocation() const
{
  dom::Node* target = stack_.empty() ? nullptr : stack_.back();
  if (target == nullptr) {
    target = document_.get();
  }
  const std::string_view target_tag = target->node_type() == dom::NodeType::kElement
                                          ? static_cast<dom::Element*>(target)->tag_name()
                                          : std::string_view();
  if (foster_parenting_ && (target_tag == "table" || target_tag == "tbody" ||
                            target_tag == "tfoot" || target_tag == "thead" || target_tag == "tr")) {
    // Find the last table element in the stack of open elements.
    dom::Element* last_table = nullptr;
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
      if ((*it)->tag_name() == "table") {
        last_table = *it;
        break;
      }
    }
    if (last_table == nullptr) {
      // Fragment case: insert into the html element.
      return stack_.empty() ? std::make_pair(static_cast<dom::Node*>(document_.get()),
                                             static_cast<dom::Node*>(nullptr))
                            : std::make_pair(static_cast<dom::Node*>(stack_.front()),
                                             static_cast<dom::Node*>(nullptr));
    }
    if (last_table->parent() != nullptr) {
      // Insert immediately before the table in its parent.
      return std::make_pair(last_table->parent(), static_cast<dom::Node*>(last_table));
    }
    // The table is the root; insert into the element above it in the stack.
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
      if (*it == last_table) {
        if (it + 1 != stack_.rend()) {
          return std::make_pair(static_cast<dom::Node*>(*(it + 1)),
                                static_cast<dom::Node*>(nullptr));
        }
        break;
      }
    }
    return std::make_pair(static_cast<dom::Node*>(document_.get()),
                          static_cast<dom::Node*>(nullptr));
  }
  return std::make_pair(target, static_cast<dom::Node*>(nullptr));
}

void Parser::InsertNodeAt(dom::Node* parent, dom::Node* before, std::unique_ptr<dom::Node> node)
{
  if (parent == nullptr) {
    parent = document_.get();
  }
  if (before != nullptr) {
    parent->InsertBefore(std::move(node), before);
  } else {
    parent->AppendChild(std::move(node));
  }
}

// Inserts text at an adjusted insertion location, merging with an adjacent
// text node when possible.
void Parser::AppendTextAt(dom::Node* parent, dom::Node* before, std::string_view text)
{
  std::string clean;
  for (const char c : text) {
    if (c == '\0') {
      clean += "\xEF\xBF\xBD"; // U+FFFD replacement character
    } else {
      clean.push_back(c);
    }
  }
  if (before != nullptr) {
    // Merge with the previous sibling if it is a text node.
    dom::Node* prev = nullptr;
    for (dom::Node* child : parent->ChildNodes()) {
      if (child == before) {
        break;
      }
      prev = child;
    }
    if (prev != nullptr && prev->node_type() == dom::NodeType::kText) {
      static_cast<dom::Text*>(prev)->AppendData(clean);
      return;
    }
    parent->InsertBefore(std::make_unique<dom::Text>(std::move(clean)), before);
    return;
  }
  dom::Node* last = parent->last_child();
  if (last != nullptr && last->node_type() == dom::NodeType::kText) {
    static_cast<dom::Text*>(last)->AppendData(clean);
    return;
  }
  parent->AppendChild(std::make_unique<dom::Text>(std::move(clean)));
}

dom::Element* Parser::CloneElement(const dom::Element& source)
{
  auto element = std::make_unique<dom::Element>(std::string(source.tag_name()));
  for (const dom::Attribute& attr : source.attributes()) {
    element->SetAttribute(attr.name, attr.value);
  }
  return element.release();
}

bool Parser::IsRawTextElement(std::string_view tag) const
{
  return tag == "xmp" || tag == "iframe" || tag == "noembed";
}

void Parser::PushActiveFormatting(dom::Element* element)
{
  // Noah's Ark clause: at most three elements with the same tag name,
  // namespace, and attributes after the last marker.
  int same = 0;
  dom::Element* earliest = nullptr;
  for (dom::Element* existing : active_formatting_) {
    if (existing == nullptr) {
      continue;
    }
    if (existing->tag_name() == element->tag_name() && SameAttributes(*existing, *element)) {
      ++same;
      if (earliest == nullptr) {
        earliest = existing;
      }
    }
  }
  if (same >= 3 && earliest != nullptr) {
    RemoveFromActiveFormatting(earliest);
  }
  active_formatting_.push_back(element);
}

void Parser::RemoveFromActiveFormatting(dom::Element* element)
{
  active_formatting_.erase(
      std::remove(active_formatting_.begin(), active_formatting_.end(), element),
      active_formatting_.end());
}

bool Parser::InActiveFormatting(dom::Element* element) const
{
  return std::find(active_formatting_.begin(), active_formatting_.end(), element) !=
         active_formatting_.end();
}

bool Parser::InStack(dom::Element* element) const
{
  return std::find(stack_.begin(), stack_.end(), element) != stack_.end();
}

bool Parser::ElementInScope(dom::Element* element) const
{
  for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
    if (*it == element) {
      return true;
    }
    if (IsScopeBoundaryTag((*it)->tag_name())) {
      return false;
    }
  }
  return false;
}

void Parser::ReconstructActiveFormatting()
{
  if (active_formatting_.empty()) {
    return;
  }
  dom::Element* last = active_formatting_.back();
  if (last == nullptr || InStack(last)) {
    return;
  }
  // Rewind to the earliest entry (from the end) that is not a marker and not
  // in the stack; that is the first element that needs reconstructing.
  std::size_t entry = active_formatting_.size() - 1;
  while (entry > 0) {
    dom::Element* candidate = active_formatting_[entry - 1];
    if (candidate == nullptr || InStack(candidate)) {
      break;
    }
    --entry;
  }
  // Create a fresh element for every entry from `entry` to the end.
  for (std::size_t i = entry; i < active_formatting_.size(); ++i) {
    dom::Element* old = active_formatting_[i];
    if (old == nullptr) {
      continue;
    }
    dom::Element* fresh = CloneElement(*old);
    InsertElement(fresh);
    active_formatting_[i] = fresh;
  }
}

void Parser::RunAdoptionAgency(std::string_view subject)
{
  dom::Element* current = CurrentNode();
  if (current != nullptr && current->tag_name() == subject && !InActiveFormatting(current)) {
    PopElement();
    return;
  }

  auto stack_index = [&](dom::Element* e) {
    for (std::size_t i = 0; i < stack_.size(); ++i) {
      if (stack_[i] == e) {
        return static_cast<long>(i);
      }
    }
    return -1L;
  };
  auto active_index = [&](dom::Element* e) {
    for (std::size_t i = 0; i < active_formatting_.size(); ++i) {
      if (active_formatting_[i] == e) {
        return static_cast<long>(i);
      }
    }
    return -1L;
  };
  auto move_node = [&](dom::Node* new_parent, dom::Node* n) {
    if (n->parent() == new_parent) {
      return;
    }
    if (n->parent() != nullptr) {
      std::unique_ptr<dom::Node> detached = n->parent()->RemoveChild(n);
      new_parent->AppendChild(std::move(detached));
    } else {
      new_parent->AppendChild(std::unique_ptr<dom::Node>(n));
    }
  };

  // The adoption agency algorithm.  Every path in the body either completes
  // the reconstruction or returns, so it runs a single pass.
  {
    // Find the formatting element: the last element in the list (no markers)
    // whose tag name is `subject`.
    dom::Element* formatting = nullptr;
    for (auto it = active_formatting_.rbegin(); it != active_formatting_.rend(); ++it) {
      if (*it != nullptr && (*it)->tag_name() == subject) {
        formatting = *it;
        break;
      }
    }
    if (formatting == nullptr) {
      if (InScope(subject)) {
        PopThrough(subject);
      }
      return;
    }
    if (!InStack(formatting)) {
      RemoveFromActiveFormatting(formatting);
      return;
    }
    if (!ElementInScope(formatting)) {
      return;
    }

    const long fmt_idx = stack_index(formatting);

    // Furthest block: the first special element below formatting (toward the
    // current node).
    dom::Element* furthest_block = nullptr;
    for (long i = fmt_idx + 1; i < static_cast<long>(stack_.size()); ++i) {
      if (IsSpecialElement(stack_[static_cast<std::size_t>(i)]->tag_name())) {
        furthest_block = stack_[static_cast<std::size_t>(i)];
        break;
      }
    }

    if (furthest_block == nullptr) {
      // Pop nodes from the current node up to and including formatting.
      while (!stack_.empty()) {
        dom::Element* e = stack_.back();
        PopElement();
        if (e == formatting) {
          break;
        }
      }
      RemoveFromActiveFormatting(formatting);
      return;
    }

    dom::Element* common_ancestor =
        (fmt_idx - 1 >= 0) ? stack_[static_cast<std::size_t>(fmt_idx - 1)] : nullptr;
    long bookmark = active_index(formatting);

    // Inner loop (WHATWG 13.2.6.4.7): walk down the stack from the element
    // just below furthest_block toward the formatting element.  |node_pos| is
    // the stack index of the element *below* the one currently being examined,
    // so it stays valid even after the examined node is removed from the stack
    // (removal shifts only elements above it, never below it).  This avoids
    // re-indexing a removed node (which would return -1 and overflow).
    dom::Element* node = furthest_block;
    dom::Element* last_node = furthest_block;
    long node_pos = stack_index(furthest_block);

    int inner = 0;
    for (;;) {
      ++inner;
      if (node_pos <= 0) {
        // Nothing below furthest_block in the stack: nothing left to re-parent.
        node = nullptr;
        break;
      }
      node = stack_[static_cast<std::size_t>(node_pos - 1)];
      --node_pos;
      if (node == formatting) {
        break;
      }
      if (inner > 3 && InActiveFormatting(node)) {
        RemoveFromActiveFormatting(node);
      }
      if (!InActiveFormatting(node)) {
        auto it = std::find(stack_.begin(), stack_.end(), node);
        if (it != stack_.end()) {
          stack_.erase(it);
        }
        continue;
      }
      dom::Element* fresh = CloneElement(*node);
      *std::find(active_formatting_.begin(), active_formatting_.end(), node) = fresh;
      *std::find(stack_.begin(), stack_.end(), node) = fresh;
      node = fresh;
      if (last_node == furthest_block) {
        bookmark = active_index(node) + 1;
      }
      move_node(node, last_node);
      last_node = node;
    }

    if (common_ancestor != nullptr) {
      move_node(common_ancestor, last_node);
    }

    dom::Element* new_formatting = CloneElement(*formatting);
    while (furthest_block->first_child() != nullptr) {
      dom::Node* child = furthest_block->first_child();
      std::unique_ptr<dom::Node> detached = furthest_block->RemoveChild(child);
      new_formatting->AppendChild(std::move(detached));
    }
    furthest_block->AppendChild(std::unique_ptr<dom::Node>(new_formatting));

    {
      auto it = std::find(active_formatting_.begin(), active_formatting_.end(), formatting);
      if (it != active_formatting_.end()) {
        active_formatting_.erase(it);
      }
      std::size_t pos = static_cast<std::size_t>(std::max(0L, bookmark));
      if (pos > active_formatting_.size()) {
        pos = active_formatting_.size();
      }
      active_formatting_.insert(active_formatting_.begin() + static_cast<long>(pos),
                                new_formatting);
    }

    {
      auto it = std::find(stack_.begin(), stack_.end(), formatting);
      if (it != stack_.end()) {
        stack_.erase(it);
      }
      auto fb = std::find(stack_.begin(), stack_.end(), furthest_block);
      if (fb != stack_.end()) {
        stack_.insert(fb, new_formatting); // immediately below furthest block
      } else {
        stack_.push_back(new_formatting);
      }
    }

    return;
  }
}

void Parser::ProcessToken(Token token)
{
  // While an over-deep subtree is being discarded, only track the nesting
  // balance; no DOM nodes are created (see InsertElement).
  if (skip_depth_ > 0) {
    switch (token.type) {
    case TokenType::kStartTag:
      ++skip_depth_;
      return;
    case TokenType::kEndTag:
      --skip_depth_;
      return;
    default:
      return; // characters, comments, doctype, EOF: all dropped
    }
  }
  // The "in table text" insertion mode buffers character tokens; any other
  // token flushes the pending characters first (13.2.6.4.10).
  if (mode_ == Mode::kInTableText && token.type != TokenType::kCharacter) {
    bool has_non_whitespace = false;
    for (const Token& pending : pending_table_chars_) {
      if (!IsAllWhitespace(pending.data)) {
        has_non_whitespace = true;
        break;
      }
    }
    if (has_non_whitespace) {
      // Foster-parent the pending characters via the in-table "anything else"
      // entry (in body rules with foster parenting enabled).
      foster_parenting_ = true;
      mode_ = Mode::kInBody;
      for (Token& pending : pending_table_chars_) {
        ProcessCharacter(std::move(pending));
      }
      foster_parenting_ = false;
    } else {
      // All whitespace: insert into the current (table-ish) node.
      for (Token& pending : pending_table_chars_) {
        AppendText(pending.data);
      }
    }
    pending_table_chars_.clear();
    mode_ = original_mode_;
    Reprocess(std::move(token));
    return;
  }
  switch (token.type) {
  case TokenType::kDoctype:
    // A DOCTYPE token is only processed in the "initial" insertion mode;
    // anywhere else it is a parse error and is ignored (13.2.6.4).
    if (mode_ == Mode::kInitial) {
      mode_ = Mode::kBeforeHtml;
    }
    return;
  case TokenType::kComment:
    ProcessComment(std::move(token));
    return;
  case TokenType::kCharacter:
    ProcessCharacter(std::move(token));
    return;
  case TokenType::kStartTag:
    ProcessStartTag(std::move(token));
    return;
  case TokenType::kEndTag:
    ProcessEndTag(std::move(token));
    return;
  case TokenType::kEOF:
    return;
  }
}

void Parser::ProcessComment(Token token)
{
  switch (mode_) {
  case Mode::kAfterBody:
  case Mode::kAfterAfterBody: {
    dom::Element* html = FindInStack("html");
    if (html != nullptr) {
      html->AppendChild(std::make_unique<dom::Comment>(token.data));
    }
    return;
  }
  case Mode::kInitial:
  case Mode::kBeforeHtml:
  case Mode::kBeforeHead:
  case Mode::kInHead:
  case Mode::kAfterHead:
  case Mode::kInBody:
  case Mode::kText:
  case Mode::kInTable:
  case Mode::kInTableText:
  case Mode::kInCaption:
  case Mode::kInColumnGroup:
  case Mode::kInTableBody:
  case Mode::kInRow:
  case Mode::kInCell:
    AppendNode(std::make_unique<dom::Comment>(token.data));
    return;
  }
}

void Parser::ProcessCharacter(Token token)
{
  switch (mode_) {
  case Mode::kInitial:
    if (IsAllWhitespace(token.data)) {
      return;
    }
    mode_ = Mode::kBeforeHtml;
    Reprocess(std::move(token));
    return;

  case Mode::kBeforeHtml:
    if (IsAllWhitespace(token.data)) {
      return;
    }
    InsertElement(new dom::Element("html"));
    mode_ = Mode::kBeforeHead;
    Reprocess(std::move(token));
    return;

  case Mode::kBeforeHead:
    if (IsAllWhitespace(token.data)) {
      return;
    }
    InsertElement(new dom::Element("head"));
    mode_ = Mode::kInHead;
    Reprocess(std::move(token));
    return;

  case Mode::kInHead:
    if (IsAllWhitespace(token.data)) {
      AppendText(token.data);
      return;
    }
    PopElement(); // implicit close of head
    mode_ = Mode::kAfterHead;
    Reprocess(std::move(token));
    return;

  case Mode::kAfterHead:
    if (IsAllWhitespace(token.data)) {
      AppendText(token.data);
      return;
    }
    InsertElement(new dom::Element("body"));
    mode_ = Mode::kInBody;
    Reprocess(std::move(token));
    return;

  case Mode::kInBody:
    ReconstructActiveFormatting();
    AppendText(token.data);
    return;

  case Mode::kText:
    AppendText(token.data);
    return;

  case Mode::kInTable:
    // If the current node is a table/tbody/template/tfoot/thead/tr, the
    // characters are accumulated in "in table text".  Anything else goes
    // through the foster-parenting path (in body rules).
    if (CurrentNode() != nullptr) {
      const std::string_view tag = CurrentNode()->tag_name();
      if (tag == "table" || tag == "tbody" || tag == "template" || tag == "tfoot" ||
          tag == "thead" || tag == "tr") {
        pending_table_chars_.clear();
        original_mode_ = mode_;
        mode_ = Mode::kInTableText;
        Reprocess(std::move(token));
        return;
      }
    }
    foster_parenting_ = true;
    mode_ = Mode::kInBody;
    ProcessCharacter(std::move(token));
    foster_parenting_ = false;
    return;

  case Mode::kInTableText:
    pending_table_chars_.push_back(std::move(token));
    return;

  case Mode::kInCaption:
  case Mode::kInCell:
    ReconstructActiveFormatting();
    AppendText(token.data);
    return;

  case Mode::kInColumnGroup:
    if (IsAllWhitespace(token.data)) {
      AppendText(token.data);
      return;
    }
    // Anything else: pop colgroup, switch to "in table", reprocess.
    if (CurrentNode() != nullptr && CurrentNode()->tag_name() == "colgroup") {
      PopElement();
    }
    mode_ = Mode::kInTable;
    Reprocess(std::move(token));
    return;

  case Mode::kInTableBody:
  case Mode::kInRow:
    // Anything else: the "in table" rules for a character token buffer it
    // into pending table characters, preserving the current insertion mode
    // as the original mode (13.2.6.4.10).  The current node is always a
    // table-ish element here (tbody/thead/tfoot/tr), so buffering applies.
    pending_table_chars_.clear();
    original_mode_ = mode_;
    mode_ = Mode::kInTableText;
    Reprocess(std::move(token));
    return;

  case Mode::kAfterBody:
    if (IsAllWhitespace(token.data)) {
      AppendText(token.data);
      return;
    }
    mode_ = Mode::kInBody;
    Reprocess(std::move(token));
    return;

  case Mode::kAfterAfterBody:
    if (IsAllWhitespace(token.data)) {
      AppendText(token.data);
      return;
    }
    mode_ = Mode::kInBody;
    Reprocess(std::move(token));
    return;
  }
}

void Parser::ProcessStartTag(Token token)
{
  switch (mode_) {
  case Mode::kInitial:
    mode_ = Mode::kBeforeHtml;
    Reprocess(std::move(token));
    return;

  case Mode::kBeforeHtml:
    if (token.name == "html") {
      dom::Element* element = CreateElement(token).release();
      InsertElement(element);
      mode_ = Mode::kBeforeHead;
      return;
    }
    InsertElement(new dom::Element("html"));
    mode_ = Mode::kBeforeHead;
    Reprocess(std::move(token));
    return;

  case Mode::kBeforeHead:
    if (token.name == "html") {
      mode_ = Mode::kInHead;
      Reprocess(std::move(token));
      return;
    }
    if (token.name == "head") {
      InsertElement(CreateElement(token).release());
      head_element_ = stack_.back();
      mode_ = Mode::kInHead;
      return;
    }
    InsertElement(new dom::Element("head"));
    head_element_ = stack_.back();
    mode_ = Mode::kInHead;
    Reprocess(std::move(token));
    return;

  case Mode::kInHead:
    if (token.name == "html") {
      return; // merge attributes is not needed; html already exists
    }
    if (IsVoidElement(token.name) &&
        (token.name == "base" || token.name == "basefont" || token.name == "bgsound" ||
         token.name == "link" || token.name == "meta")) {
      InsertElement(CreateElement(token).release());
      PopElement();
      return;
    }
    if (token.name == "title" || token.name == "style" || token.name == "script") {
      InsertElement(CreateElement(token).release());
      mode_before_text_ = Mode::kInHead;
      tokenizer_.StartRawText(token.name);
      mode_ = Mode::kText;
      return;
    }
    if (token.name == "head") {
      return;
    }
    PopElement(); // implicit close of head
    mode_ = Mode::kAfterHead;
    Reprocess(std::move(token));
    return;

  case Mode::kAfterHead:
    if (token.name == "html") {
      mode_ = Mode::kInBody;
      Reprocess(std::move(token));
      return;
    }
    if (token.name == "body") {
      InsertElement(CreateElement(token).release());
      mode_ = Mode::kInBody;
      return;
    }
    // base, basefont, bgsound, link, meta, noframes, script, style,
    // template, title: push the head element pointer, process with the
    // "in head" rules, then remove the head element again (13.2.6.4.6).
    if (token.name == "base" || token.name == "basefont" || token.name == "bgsound" ||
        token.name == "link" || token.name == "meta" || token.name == "noframes" ||
        token.name == "script" || token.name == "style" || token.name == "template" ||
        token.name == "title") {
      if (head_element_ != nullptr) {
        stack_.push_back(head_element_);
        ProcessInHead(std::move(token));
        for (auto it = stack_.begin(); it != stack_.end(); ++it) {
          if (*it == head_element_) {
            stack_.erase(it);
            break;
          }
        }
        return;
      }
    }
    InsertElement(new dom::Element("body"));
    mode_ = Mode::kInBody;
    Reprocess(std::move(token));
    return;

  case Mode::kInBody: {
    const std::string& tag = token.name;

    if (tag == "html") {
      return; // attributes of the document element are already set
    }
    if (tag == "head" || tag == "body") {
      return; // parse error; ignored
    }
    if (tag == "image") {
      // "image" is a parse error; treat as "img" (13.2.6.4.7).
      token.name = "img";
      ProcessStartTag(std::move(token));
      return;
    }
    if (IsVoidElement(tag)) {
      if (tag == "hr") {
        ClosePElement();
      }
      InsertElement(CreateElement(token).release());
      PopElement();
      return;
    }
    if (tag == "title" || tag == "style" || tag == "script" || tag == "textarea" ||
        IsRawTextElement(tag)) {
      InsertElement(CreateElement(token).release());
      mode_before_text_ = Mode::kInBody;
      tokenizer_.StartRawText(tag);
      mode_ = Mode::kText;
      return;
    }
    if (tag == "plaintext") {
      ClosePElement();
      InsertElement(CreateElement(token).release());
      tokenizer_.StartPlaintext();
      return;
    }
    if (tag == "p") {
      ClosePElement();
      InsertElement(CreateElement(token).release());
      return;
    }
    if (tag == "li") {
      // WHATWG 13.2.6.4.7: walk the stack toward the root, looking for an
      // open li element, but stop at the first special element that is not
      // address, div or p (a nested <ul>/<ol> is special, so a <li> inside
      // one does not close an outer list item).
      bool done = false;
      for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        const std::string_view t = (*it)->tag_name();
        if (t == "li") {
          GenerateImpliedEndTags("li");
          PopThrough("li");
          done = true;
          break;
        }
        if (IsSpecialElement(t) && t != "address" && t != "div" && t != "p") {
          done = true;
          break;
        }
      }
      (void)done;
      ClosePElement();
      InsertElement(CreateElement(token).release());
      return;
    }
    if (tag == "dd" || tag == "dt") {
      // A dd/dt start tag closes an open dd or dt (13.2.6.4.7).
      if (InScope("dd")) {
        GenerateImpliedEndTags("dd");
        PopThrough("dd");
      }
      if (InScope("dt")) {
        GenerateImpliedEndTags("dt");
        PopThrough("dt");
      }
      ClosePElement();
      InsertElement(CreateElement(token).release());
      return;
    }
    if (IsHeadingTag(tag)) {
      // Close an open heading if one exists.
      for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        if (IsHeadingTag((*it)->tag_name())) {
          // Pop through the found heading.
          while (!stack_.empty()) {
            dom::Element* e = stack_.back();
            PopElement();
            if (IsHeadingTag(e->tag_name())) {
              break;
            }
          }
          break;
        }
      }
      ClosePElement();
      InsertElement(CreateElement(token).release());
      return;
    }
    if (tag == "table") {
      ClosePElement();
      InsertElement(CreateElement(token).release());
      mode_ = Mode::kInTable;
      return;
    }
    if (IsBlockElement(tag) || tag == "center") {
      ClosePElement();
      InsertElement(CreateElement(token).release());
      return;
    }
    if (tag == "button") {
      if (InScope("button")) {
        GenerateImpliedEndTags();
        PopThrough("button");
      }
      ReconstructActiveFormatting();
      InsertElement(CreateElement(token).release());
      return;
    }
    if (tag == "a") {
      ReconstructActiveFormatting();
      if (FindInStack("a") != nullptr) {
        PopThrough("a");
      }
      InsertElement(CreateElement(token).release());
      PushActiveFormatting(stack_.back());
      return;
    }

    // Any other start tag: inline element or unknown element.
    if (IsFormattingElement(tag)) {
      ReconstructActiveFormatting();
    }
    InsertElement(CreateElement(token).release());
    if (IsFormattingElement(tag)) {
      PushActiveFormatting(stack_.back());
    }
    // The self-closing flag on a non-void HTML element is a parse error and
    // is ignored (13.2.6.4.7 "non-void-html-element-start-tag-with-
    // trailing-solidus"); foreign content is not produced here.
    return;
  }

  case Mode::kText:
    // Character data should have arrived as kCharacter; a stray start tag
    // is dropped.
    return;

  case Mode::kInTable:
    ProcessStartTagInTable(std::move(token));
    return;

  case Mode::kInTableText:
    // Character tokens are buffered; a start tag is processed as
    // "anything else" (flushed by ProcessToken) — the token reaches here
    // only after the pending characters have been handled.
    ProcessStartTagInTable(std::move(token));
    return;

  case Mode::kInCaption:
    if (IsTableCaptionColumn(token.name)) {
      // Start tag whose name is one of: caption, col, colgroup, tbody, td,
      // tfoot, th, thead, tr.
      if (!InTableScope("caption")) {
        return; // parse error; ignore (fragment case)
      }
      GenerateImpliedEndTags();
      PopThrough("caption");
      ClearActiveFormattingToMarker();
      mode_ = Mode::kInTable;
      Reprocess(std::move(token));
      return;
    }
    mode_ = Mode::kInBody;
    ProcessStartTag(std::move(token));
    return;

  case Mode::kInColumnGroup:
    if (token.name == "html") {
      mode_ = Mode::kInBody;
      ProcessStartTag(std::move(token));
      return;
    }
    if (token.name == "col") {
      InsertElement(CreateElement(token).release());
      PopElement();
      return;
    }
    if (token.name == "template") {
      mode_ = Mode::kInHead;
      ProcessStartTag(std::move(token));
      return;
    }
    // Anything else.
    if (CurrentNode() != nullptr && CurrentNode()->tag_name() != "colgroup") {
      return; // parse error; ignore
    }
    PopElement();
    mode_ = Mode::kInTable;
    Reprocess(std::move(token));
    return;

  case Mode::kInTableBody:
    if (token.name == "tr") {
      ClearStackBackToTableBodyContext();
      InsertElement(CreateElement(token).release());
      mode_ = Mode::kInRow;
      return;
    }
    if (token.name == "th" || token.name == "td") {
      ClearStackBackToTableBodyContext();
      InsertElement(new dom::Element("tr"));
      mode_ = Mode::kInRow;
      Reprocess(std::move(token));
      return;
    }
    if (token.name == "caption" || token.name == "col" || token.name == "colgroup" ||
        token.name == "tbody" || token.name == "tfoot" || token.name == "thead") {
      if (!InTableScope("tbody") && !InTableScope("thead") && !InTableScope("tfoot")) {
        return; // parse error; ignore
      }
      ClearStackBackToTableBodyContext();
      PopElement();
      mode_ = Mode::kInTable;
      Reprocess(std::move(token));
      return;
    }
    // Anything else.
    mode_ = Mode::kInTable;
    ProcessStartTag(std::move(token));
    return;

  case Mode::kInRow:
    if (token.name == "th" || token.name == "td") {
      ClearStackBackToTableRowContext();
      InsertElement(CreateElement(token).release());
      mode_ = Mode::kInCell;
      PushFormattingMarker();
      return;
    }
    if (token.name == "caption" || token.name == "col" || token.name == "colgroup" ||
        token.name == "tbody" || token.name == "tfoot" || token.name == "thead" ||
        token.name == "tr") {
      if (!InTableScope("tr")) {
        return; // parse error; ignore
      }
      ClearStackBackToTableRowContext();
      PopElement();
      mode_ = Mode::kInTableBody;
      Reprocess(std::move(token));
      return;
    }
    // Anything else.
    mode_ = Mode::kInTable;
    ProcessStartTag(std::move(token));
    return;

  case Mode::kInCell:
    if (IsTableCaptionColumn(token.name)) {
      if (!InTableScope("td") && !InTableScope("th")) {
        return; // parse error; ignore
      }
      CloseTableCell();
      Reprocess(std::move(token));
      return;
    }
    mode_ = Mode::kInBody;
    ProcessStartTag(std::move(token));
    return;

  case Mode::kAfterBody:
    if (token.name == "html") {
      mode_ = Mode::kInBody;
      Reprocess(std::move(token));
      return;
    }
    mode_ = Mode::kInBody;
    Reprocess(std::move(token));
    return;

  case Mode::kAfterAfterBody:
    if (token.name == "html") {
      mode_ = Mode::kInBody;
      Reprocess(std::move(token));
      return;
    }
    mode_ = Mode::kInBody;
    Reprocess(std::move(token));
    return;
  }
}

void Parser::ProcessEndTag(Token token)
{
  switch (mode_) {
  case Mode::kInitial:
  case Mode::kBeforeHtml:
    return; // parse error; ignored

  case Mode::kBeforeHead:
    // An end tag here is a parse error; ignore it rather than popping the
    // html root (head has not been inserted yet).
    return;

  case Mode::kInHead:
    if (token.name == "head") {
      PopElement();
      mode_ = Mode::kAfterHead;
      return;
    }
    if (token.name == "html" || token.name == "body") {
      PopElement();
      mode_ = Mode::kAfterHead;
      Reprocess(std::move(token));
      return;
    }
    PopElement(); // implicit close of head
    mode_ = Mode::kAfterHead;
    Reprocess(std::move(token));
    return;

  case Mode::kAfterHead:
    return; // parse error; ignored

  case Mode::kInBody: {
    const std::string& tag = token.name;
    if (tag == "body") {
      if (InScope("body")) {
        mode_ = Mode::kAfterBody;
      }
      return;
    }
    if (tag == "html") {
      mode_ = Mode::kAfterBody;
      Reprocess(std::move(token));
      return;
    }
    if (tag == "br") {
      // WHATWG: </br> is treated as a <br> start tag.
      Reprocess(Token::MakeStartTag("br", {}, /*self_closing=*/false));
      return;
    }
    if (tag == "p") {
      if (InScope("p")) {
        PopThrough("p");
      }
      return;
    }
    if (tag == "li") {
      if (InListItemScope("li")) {
        GenerateImpliedEndTags("li");
        PopThrough("li");
      }
      return;
    }
    if (IsHeadingTag(tag)) {
      for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        if (IsHeadingTag((*it)->tag_name())) {
          while (!stack_.empty()) {
            dom::Element* e = stack_.back();
            PopElement();
            if (IsHeadingTag(e->tag_name())) {
              break;
            }
          }
          return;
        }
      }
      return;
    }
    if (IsFormattingElement(tag)) {
      RunAdoptionAgency(tag);
      return;
    }
    if (InScope(tag)) {
      PopThrough(tag);
    }
    return;
  }

  case Mode::kText:
    // The matching end tag of a raw text element.
    PopElement();
    mode_ = mode_before_text_;
    return;

  case Mode::kInTable:
    ProcessEndTagInTable(std::move(token));
    return;

  case Mode::kInTableText:
    ProcessEndTagInTable(std::move(token));
    return;

  case Mode::kInCaption: {
    const std::string& tag = token.name;
    if (tag == "caption") {
      if (!InTableScope("caption")) {
        return; // parse error; ignore
      }
      GenerateImpliedEndTags();
      PopThrough("caption");
      ClearActiveFormattingToMarker();
      mode_ = Mode::kInTable;
      return;
    }
    if (tag == "table") {
      if (!InTableScope("caption")) {
        return; // parse error; ignore
      }
      GenerateImpliedEndTags();
      PopThrough("caption");
      ClearActiveFormattingToMarker();
      mode_ = Mode::kInTable;
      Reprocess(std::move(token));
      return;
    }
    if (tag == "body" || tag == "col" || tag == "colgroup" || tag == "html" || tag == "tbody" ||
        tag == "td" || tag == "tfoot" || tag == "th" || tag == "thead" || tag == "tr") {
      return; // parse error; ignore
    }
    mode_ = Mode::kInBody;
    ProcessEndTag(std::move(token));
    return;
  }

  case Mode::kInColumnGroup:
    if (token.name == "colgroup") {
      if (CurrentNode() != nullptr && CurrentNode()->tag_name() != "colgroup") {
        return; // parse error; ignore
      }
      PopElement();
      mode_ = Mode::kInTable;
      return;
    }
    if (token.name == "col") {
      return; // parse error; ignore
    }
    if (token.name == "template") {
      mode_ = Mode::kInHead;
      ProcessEndTag(std::move(token));
      return;
    }
    // Anything else.
    if (CurrentNode() != nullptr && CurrentNode()->tag_name() != "colgroup") {
      return; // parse error; ignore
    }
    PopElement();
    mode_ = Mode::kInTable;
    Reprocess(std::move(token));
    return;

  case Mode::kInTableBody: {
    const std::string& tag = token.name;
    if (tag == "tbody" || tag == "tfoot" || tag == "thead") {
      if (!InTableScope(tag)) {
        return; // parse error; ignore
      }
      ClearStackBackToTableBodyContext();
      PopElement();
      mode_ = Mode::kInTable;
      return;
    }
    if (tag == "table") {
      if (!InTableScope("tbody") && !InTableScope("thead") && !InTableScope("tfoot")) {
        return; // parse error; ignore
      }
      ClearStackBackToTableBodyContext();
      PopElement();
      mode_ = Mode::kInTable;
      Reprocess(std::move(token));
      return;
    }
    if (tag == "body" || tag == "caption" || tag == "col" || tag == "colgroup" || tag == "html" ||
        tag == "td" || tag == "th" || tag == "tr") {
      return; // parse error; ignore
    }
    mode_ = Mode::kInTable;
    ProcessEndTag(std::move(token));
    return;
  }

  case Mode::kInRow: {
    const std::string& tag = token.name;
    if (tag == "tr") {
      if (!InTableScope("tr")) {
        return; // parse error; ignore
      }
      ClearStackBackToTableRowContext();
      PopElement();
      mode_ = Mode::kInTableBody;
      return;
    }
    if (tag == "table") {
      if (!InTableScope("tr")) {
        return; // parse error; ignore
      }
      ClearStackBackToTableRowContext();
      PopElement();
      mode_ = Mode::kInTableBody;
      Reprocess(std::move(token));
      return;
    }
    if (tag == "tbody" || tag == "tfoot" || tag == "thead") {
      if (!InTableScope(tag)) {
        return; // parse error; ignore
      }
      if (!InTableScope("tr")) {
        return; // parse error; ignore
      }
      ClearStackBackToTableRowContext();
      PopElement();
      mode_ = Mode::kInTableBody;
      Reprocess(std::move(token));
      return;
    }
    if (tag == "body" || tag == "caption" || tag == "col" || tag == "colgroup" || tag == "html" ||
        tag == "td" || tag == "th") {
      return; // parse error; ignore
    }
    mode_ = Mode::kInTable;
    ProcessEndTag(std::move(token));
    return;
  }

  case Mode::kInCell: {
    const std::string& tag = token.name;
    if (tag == "td" || tag == "th") {
      if (!InTableScope(tag)) {
        return; // parse error; ignore
      }
      GenerateImpliedEndTags();
      while (!stack_.empty()) {
        const std::string_view t = stack_.back()->tag_name();
        PopElement();
        if (t == "td" || t == "th") {
          break;
        }
      }
      ClearActiveFormattingToMarker();
      mode_ = Mode::kInRow;
      return;
    }
    if (tag == "table" || tag == "tbody" || tag == "tfoot" || tag == "thead" || tag == "tr") {
      if (!InTableScope(tag)) {
        return; // parse error; ignore
      }
      CloseTableCell();
      Reprocess(std::move(token));
      return;
    }
    if (tag == "body" || tag == "caption" || tag == "col" || tag == "colgroup" || tag == "html") {
      return; // parse error; ignore
    }
    mode_ = Mode::kInBody;
    ProcessEndTag(std::move(token));
    return;
  }

  case Mode::kAfterBody:
    if (token.name == "html") {
      mode_ = Mode::kAfterAfterBody;
      return;
    }
    mode_ = Mode::kInBody;
    Reprocess(std::move(token));
    return;

  case Mode::kAfterAfterBody:
    return; // parse error; ignored
  }
}

// The rules for the "in table" insertion mode (13.2.6.4.9), start tags.
void Parser::ProcessStartTagInTable(Token token)
{
  const std::string& tag = token.name;
  if (tag == "caption") {
    ClearStackBackToTableContext();
    PushFormattingMarker();
    InsertElement(CreateElement(token).release());
    mode_ = Mode::kInCaption;
    return;
  }
  if (tag == "colgroup") {
    ClearStackBackToTableContext();
    InsertElement(CreateElement(token).release());
    mode_ = Mode::kInColumnGroup;
    return;
  }
  if (tag == "col") {
    ClearStackBackToTableContext();
    InsertElement(new dom::Element("colgroup"));
    mode_ = Mode::kInColumnGroup;
    Reprocess(std::move(token));
    return;
  }
  if (tag == "tbody" || tag == "tfoot" || tag == "thead") {
    ClearStackBackToTableContext();
    InsertElement(CreateElement(token).release());
    mode_ = Mode::kInTableBody;
    return;
  }
  if (tag == "td" || tag == "th" || tag == "tr") {
    ClearStackBackToTableContext();
    InsertElement(new dom::Element("tbody"));
    mode_ = Mode::kInTableBody;
    Reprocess(std::move(token));
    return;
  }
  if (tag == "table") {
    if (!InTableScope("table")) {
      return; // parse error; ignore
    }
    PopThrough("table");
    ResetInsertionMode();
    Reprocess(std::move(token));
    return;
  }
  if (tag == "style" || tag == "script" || tag == "template") {
    mode_ = Mode::kInHead;
    ProcessStartTag(std::move(token));
    return;
  }
  if (tag == "input") {
    // A hidden input is inserted and immediately popped; anything else is
    // handled by foster parenting (below).
    for (const Attribute& attr : token.attributes) {
      if (attr.name == "type" && ToLowerAscii(attr.value) == "hidden") {
        InsertElement(CreateElement(token).release());
        PopElement();
        return;
      }
    }
    foster_parenting_ = true;
    mode_ = Mode::kInBody;
    ProcessStartTag(std::move(token));
    foster_parenting_ = false;
    return;
  }
  // Anything else: enable foster parenting, process in body, then disable it.
  foster_parenting_ = true;
  mode_ = Mode::kInBody;
  ProcessStartTag(std::move(token));
  foster_parenting_ = false;
}

// The rules for the "in table" insertion mode (13.2.6.4.9), end tags.
void Parser::ProcessEndTagInTable(Token token)
{
  const std::string& tag = token.name;
  if (tag == "table") {
    if (!InTableScope("table")) {
      return; // parse error; ignore
    }
    PopThrough("table");
    ResetInsertionMode();
    return;
  }
  if (tag == "body" || tag == "caption" || tag == "col" || tag == "colgroup" || tag == "html" ||
      tag == "tbody" || tag == "td" || tag == "tfoot" || tag == "th" || tag == "thead" ||
      tag == "tr") {
    return; // parse error; ignore
  }
  if (tag == "template") {
    mode_ = Mode::kInHead;
    ProcessEndTag(std::move(token));
    return;
  }
  // Anything else.
  foster_parenting_ = true;
  mode_ = Mode::kInBody;
  ProcessEndTag(std::move(token));
  foster_parenting_ = false;
}

// Processes a token with the rules for the "in head" insertion mode
// (13.2.6.4.4).  Used by the "after head" rules for base/link/meta/script/
// style/title etc., where the head element pointer is pushed onto the stack
// first.  If the in-head rules did not change the insertion mode (e.g. a
// void head element), the mode reverts to "after head".
void Parser::ProcessInHead(Token token)
{
  const Mode saved_mode = mode_;
  mode_ = Mode::kInHead;
  switch (token.type) {
  case TokenType::kStartTag:
    ProcessStartTag(std::move(token));
    break;
  case TokenType::kEndTag:
    ProcessEndTag(std::move(token));
    break;
  case TokenType::kComment:
    ProcessComment(std::move(token));
    break;
  case TokenType::kCharacter:
    ProcessCharacter(std::move(token));
    break;
  case TokenType::kDoctype:
    break; // parse error; ignored
  case TokenType::kEOF:
    break;
  }
  if (mode_ == Mode::kInHead) {
    mode_ = saved_mode;
  }
}

} // namespace neko::html
