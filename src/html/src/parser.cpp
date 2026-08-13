#include "neko/html/parser.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace neko::html {
namespace {

bool IsAllWhitespace(std::string_view text) {
  for (const char c : text) {
    if (!IsAsciiWhitespace(c)) {
      return false;
    }
  }
  return !text.empty();
}

bool IsHeadingTag(std::string_view tag) {
  return tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" || tag == "h5" || tag == "h6";
}

// Elements in the WHATWG "formatting" category (13.2.4.3).
bool IsFormattingElement(std::string_view tag) {
  return tag == "a" || tag == "b" || tag == "big" || tag == "code" || tag == "em" ||
         tag == "font" || tag == "i" || tag == "nobr" || tag == "s" || tag == "small" ||
         tag == "strike" || tag == "strong" || tag == "tt" || tag == "u";
}

// Elements in the WHATWG "special" category (used by the adoption agency).
bool IsSpecialElement(std::string_view tag) {
  return tag == "address" || tag == "applet" || tag == "area" || tag == "article" ||
         tag == "aside" || tag == "base" || tag == "basefont" || tag == "bgsound" ||
         tag == "blockquote" || tag == "body" || tag == "br" || tag == "button" ||
         tag == "caption" || tag == "center" || tag == "col" || tag == "colgroup" ||
         tag == "dd" || tag == "details" || tag == "dir" || tag == "div" || tag == "dl" ||
         tag == "dt" || tag == "embed" || tag == "fieldset" || tag == "figcaption" ||
         tag == "figure" || tag == "footer" || tag == "form" || tag == "frame" ||
         tag == "frameset" || tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" ||
         tag == "h5" || tag == "h6" || tag == "head" || tag == "header" || tag == "hgroup" ||
         tag == "hr" || tag == "html" || tag == "iframe" || tag == "img" || tag == "input" ||
         tag == "keygen" || tag == "li" || tag == "link" || tag == "listing" || tag == "main" ||
         tag == "marquee" || tag == "menu" || tag == "meta" || tag == "nav" || tag == "noembed" ||
         tag == "noframes" || tag == "noscript" || tag == "object" || tag == "ol" ||
         tag == "p" || tag == "param" || tag == "plaintext" || tag == "pre" || tag == "script" ||
         tag == "section" || tag == "select" || tag == "source" || tag == "style" ||
         tag == "summary" || tag == "table" || tag == "tbody" || tag == "td" ||
         tag == "template" || tag == "textarea" || tag == "tfoot" || tag == "th" ||
         tag == "thead" || tag == "title" || tag == "tr" || tag == "track" || tag == "ul" ||
         tag == "wbr" || tag == "xmp";
}

// True when two elements have the same set of parsed attributes (name + value,
// order-independent).  The namespace is always HTML in this parser, so it is
// not compared.
bool SameAttributes(const dom::Element& a, const dom::Element& b) {
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
std::unique_ptr<dom::Element> CreateElement(const Token& token) {
  auto element = std::make_unique<dom::Element>(token.name);
  for (const Attribute& attr : token.attributes) {
    element->SetAttribute(attr.name, attr.value);
  }
  return element;
}

}  // namespace

Parser::Parser(std::string_view html) : html_(html), tokenizer_(html) {}

std::unique_ptr<dom::Document> Parser::Parse() {
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
            PopElement();  // implicit close of head
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

Token Parser::NextToken() {
  if (pending_.has_value()) {
    Token token = std::move(pending_.value());
    pending_.reset();
    return token;
  }
  return tokenizer_.Next();
}

void Parser::Reprocess(Token token) { pending_ = std::move(token); }

dom::Element* Parser::CurrentNode() { return stack_.empty() ? nullptr : stack_.back(); }

void Parser::AppendNode(std::unique_ptr<dom::Node> node) {
  if (stack_.empty()) {
    document_->AppendChild(std::move(node));
  } else {
    stack_.back()->AppendChild(std::move(node));
  }
}

void Parser::InsertElement(dom::Element* element) {
  if (stack_.empty()) {
    document_->AppendChild(std::unique_ptr<dom::Node>(element));
  } else {
    stack_.back()->AppendChild(std::unique_ptr<dom::Node>(element));
  }
  stack_.push_back(element);
}

void Parser::PopElement() { stack_.pop_back(); }

void Parser::PopThrough(std::string_view tag) {
  while (!stack_.empty()) {
    dom::Element* element = stack_.back();
    PopElement();
    if (element->tag_name() == tag) {
      break;
    }
  }
}

void Parser::AppendText(std::string_view text) {
  dom::Node* current = CurrentNode();
  if (current == nullptr) {
    current = document_.get();
  }
  std::string clean;
  for (const char c : text) {
    if (c == '\0') {
      clean += "\xEF\xBF\xBD";  // U+FFFD replacement character
    } else {
      clean.push_back(c);
    }
  }
  dom::Node* last = current->last_child();
  if (last != nullptr && last->node_type() == dom::NodeType::kText) {
    static_cast<dom::Text*>(last)->AppendData(clean);
    return;
  }
  current->AppendChild(std::make_unique<dom::Text>(std::move(clean)));
}

// Whether |tag| terminates scope searches.  These are the WHATWG 13.2.6 scope
// boundaries for the default scope (applet, caption, html, table, td, th,
// marquee, object, template) plus the MathML/SVG ones we do not create.  The
// HTML parser never produces SVG/MathML elements, so those boundaries are
// omitted.
bool IsScopeBoundaryTag(std::string_view tag) {
  return tag == "applet" || tag == "caption" || tag == "html" || tag == "table" ||
         tag == "td" || tag == "th" || tag == "marquee" || tag == "object" ||
         tag == "template";
}

bool Parser::InScope(std::string_view tag) const {
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
bool Parser::InButtonScope(std::string_view tag) const {
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

dom::Element* Parser::FindInStack(std::string_view tag) const {
  for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
    if ((*it)->tag_name() == tag) {
      return *it;
    }
  }
  return nullptr;
}

void Parser::ClosePElement() {
  // "close a p element": only if the open <p> is in button scope.
  if (InButtonScope("p")) {
    PopThrough("p");
  }
}

bool Parser::IsVoidElement(std::string_view tag) const {
  return tag == "area" || tag == "base" || tag == "basefont" || tag == "bgsound" || tag == "br" ||
         tag == "col" || tag == "embed" || tag == "hr" || tag == "img" || tag == "input" ||
         tag == "keygen" || tag == "link" || tag == "meta" || tag == "param" || tag == "source" ||
         tag == "track" || tag == "wbr";
}

bool Parser::IsBlockElement(std::string_view tag) const {
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

dom::Element* Parser::CloneElement(const dom::Element& source) {
  auto element = std::make_unique<dom::Element>(std::string(source.tag_name()));
  for (const dom::Attribute& attr : source.attributes()) {
    element->SetAttribute(attr.name, attr.value);
  }
  return element.release();
}

void Parser::PushActiveFormatting(dom::Element* element) {
  // Noah's Ark clause: at most three elements with the same tag name,
  // namespace, and attributes after the last marker.  Markers are not
  // implemented yet (tables are treated as blocks), so the whole list is
  // searched.
  int same = 0;
  dom::Element* earliest = nullptr;
  for (dom::Element* existing : active_formatting_) {
    if (existing != nullptr && existing->tag_name() == element->tag_name() &&
        SameAttributes(*existing, *element)) {
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

void Parser::RemoveFromActiveFormatting(dom::Element* element) {
  active_formatting_.erase(
      std::remove(active_formatting_.begin(), active_formatting_.end(), element),
      active_formatting_.end());
}

bool Parser::InActiveFormatting(dom::Element* element) const {
  return std::find(active_formatting_.begin(), active_formatting_.end(), element) !=
         active_formatting_.end();
}

bool Parser::InStack(dom::Element* element) const {
  return std::find(stack_.begin(), stack_.end(), element) != stack_.end();
}

bool Parser::ElementInScope(dom::Element* element) const {
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

void Parser::ReconstructActiveFormatting() {
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

void Parser::RunAdoptionAgency(std::string_view subject) {
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

  for (int outer = 0; outer < 8; ++outer) {
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

    dom::Element* node = furthest_block;
    dom::Element* last_node = furthest_block;

    int inner = 0;
    for (;;) {
      ++inner;
      const long node_idx = stack_index(node);
      node = (node_idx - 1 >= 0) ? stack_[static_cast<std::size_t>(node_idx - 1)] : nullptr;
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
        stack_.insert(fb, new_formatting);  // immediately below furthest block
      } else {
        stack_.push_back(new_formatting);
      }
    }

    return;
  }
}

void Parser::ProcessToken(Token token) {
  switch (token.type) {
    case TokenType::kDoctype:
      // The doctype is not represented in the DOM yet (Phase 3 scope); only
      // the quirks flag is consumed by the tokenizer.
      mode_ = Mode::kBeforeHtml;
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

void Parser::ProcessComment(Token token) {
  switch (mode_) {
    case Mode::kInitial:
    case Mode::kBeforeHtml:
      AppendNode(std::make_unique<dom::Comment>(token.data));
      return;
    case Mode::kBeforeHead:
    case Mode::kInHead:
    case Mode::kAfterHead:
    case Mode::kInBody:
      AppendNode(std::make_unique<dom::Comment>(token.data));
      return;
    case Mode::kAfterBody:
    case Mode::kAfterAfterBody: {
      dom::Element* html = FindInStack("html");
      if (html != nullptr) {
        html->AppendChild(std::make_unique<dom::Comment>(token.data));
      }
      return;
    }
    case Mode::kText:
      AppendNode(std::make_unique<dom::Comment>(token.data));
      return;
  }
}

void Parser::ProcessCharacter(Token token) {
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
      PopElement();  // implicit close of head
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

void Parser::ProcessStartTag(Token token) {
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
        mode_ = Mode::kInHead;
        return;
      }
      InsertElement(new dom::Element("head"));
      mode_ = Mode::kInHead;
      Reprocess(std::move(token));
      return;

    case Mode::kInHead:
      if (token.name == "html") {
        return;  // merge attributes is not needed; html already exists
      }
      if (IsVoidElement(token.name) && (token.name == "base" || token.name == "basefont" ||
                                        token.name == "bgsound" || token.name == "link" ||
                                        token.name == "meta")) {
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
      PopElement();  // implicit close of head
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
      InsertElement(new dom::Element("body"));
      mode_ = Mode::kInBody;
      Reprocess(std::move(token));
      return;

    case Mode::kInBody: {
      const std::string& tag = token.name;

      if (tag == "html") {
        return;  // attributes of the document element are already set
      }
      if (tag == "head" || tag == "body") {
        return;  // parse error; ignored
      }
      if (IsVoidElement(tag)) {
        InsertElement(CreateElement(token).release());
        PopElement();
        return;
      }
      if (tag == "title" || tag == "style" || tag == "script" || tag == "textarea") {
        InsertElement(CreateElement(token).release());
        mode_before_text_ = Mode::kInBody;
        tokenizer_.StartRawText(tag);
        mode_ = Mode::kText;
        return;
      }
      if (tag == "p") {
        ClosePElement();
        InsertElement(CreateElement(token).release());
        return;
      }
      if (tag == "li") {
        if (FindInStack("li") != nullptr) {
          PopThrough("li");
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
      if (IsBlockElement(tag)) {
        ClosePElement();
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
      if (token.self_closing) {
        PopElement();
      }
      return;
    }

    case Mode::kText:
      // Character data should have arrived as kCharacter; a stray start tag
      // is dropped.
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

void Parser::ProcessEndTag(Token token) {
  switch (mode_) {
    case Mode::kInitial:
    case Mode::kBeforeHtml:
      return;  // parse error; ignored

    case Mode::kBeforeHead:
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
      PopElement();  // implicit close of head
      mode_ = Mode::kAfterHead;
      Reprocess(std::move(token));
      return;

    case Mode::kAfterHead:
      return;  // parse error; ignored

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
        if (InScope("li")) {
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

    case Mode::kAfterBody:
      if (token.name == "html") {
        mode_ = Mode::kAfterAfterBody;
        return;
      }
      mode_ = Mode::kInBody;
      Reprocess(std::move(token));
      return;

    case Mode::kAfterAfterBody:
      return;  // parse error; ignored
  }
}

}  // namespace neko::html
