#include "neko/html/parser.h"

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

bool Parser::InScope(std::string_view tag) const {
  for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
    const std::string_view current = (*it)->tag_name();
    if (current == tag) {
      return true;
    }
    // Scope boundaries (subset of WHATWG).
    if (current == "html" || current == "table" || current == "td" || current == "th" ||
        current == "caption" || current == "applet" || current == "marquee" ||
        current == "object" || current == "template") {
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
  if (InScope("p")) {
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
        if (FindInStack("a") != nullptr) {
          PopThrough("a");
        }
        InsertElement(CreateElement(token).release());
        return;
      }

      // Any other start tag: inline element or unknown element.
      InsertElement(CreateElement(token).release());
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
