#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "neko/dom/element.h"
#include "neko/html/token.h"
#include "neko/html/tokenizer.h"

namespace neko::html {

// HTML tree construction (WHATWG 13.2.6-inspired).
//
// Implements the common insertion modes: initial, before html, before head,
// in head, after head, in body, text, after body, after after body.  Tables
// are treated as ordinary blocks (no foster parenting).  Active formatting
// element reconstruction is not implemented; see docs/html/README.md.
class Parser {
 public:
  explicit Parser(std::string_view html);

  // Parses the document.  Never returns null; malformed HTML produces a
  // best-effort DOM.
  std::unique_ptr<dom::Document> Parse();

 private:
  enum class Mode {
    kInitial,
    kBeforeHtml,
    kBeforeHead,
    kInHead,
    kAfterHead,
    kInBody,
    kText,
    kAfterBody,
    kAfterAfterBody,
  };

  void ProcessToken(Token token);
  void ProcessStartTag(Token token);
  void ProcessEndTag(Token token);
  void ProcessCharacter(Token token);
  void ProcessComment(Token token);

  void InsertElement(dom::Element* element);
  void AppendNode(std::unique_ptr<dom::Node> node);
  void AppendText(std::string_view text);
  void PopElement();
  void PopThrough(std::string_view tag);
  dom::Element* CurrentNode();
  void Reprocess(Token token);
  Token NextToken();

  bool InScope(std::string_view tag) const;
  dom::Element* FindInStack(std::string_view tag) const;
  void ClosePElement();
  bool IsVoidElement(std::string_view tag) const;
  bool IsBlockElement(std::string_view tag) const;

  std::string_view html_;
  Tokenizer tokenizer_;
  std::optional<Token> pending_;
  std::unique_ptr<dom::Document> document_;
  std::vector<dom::Element*> stack_;
  Mode mode_ = Mode::kInitial;
  Mode mode_before_text_ = Mode::kInBody;
};

}  // namespace neko::html
