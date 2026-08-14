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
// in head, after head, in body, text, after body, after after body.  Active
// formatting elements and the adoption agency algorithm are implemented.
// Tables are treated as ordinary blocks (no foster parenting), and markers in
// the active formatting element list are not yet used; see docs/html/README.md.
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
  bool InButtonScope(std::string_view tag) const;
  dom::Element* FindInStack(std::string_view tag) const;
  void ClosePElement();
  bool IsVoidElement(std::string_view tag) const;
  bool IsBlockElement(std::string_view tag) const;

  // Maximum element nesting depth while building the DOM.  Over-deep
  // subtrees are dropped so that pathological HTML (e.g. hundreds of
  // thousands of nested <div>s) cannot overflow the stack in the style,
  // layout or paint stages that recursively walk the tree.
  static constexpr std::size_t kMaxDepth = 512;
  // While > 0, an over-deep subtree is being discarded: start tags
  // increment it, end tags decrement it, and no DOM nodes are created.
  std::size_t skip_depth_ = 0;

  // Active formatting elements (WHATWG 13.2.4.3) and the adoption agency
  // algorithm (13.2.6.4.7).  Markers are not implemented yet (tables are
  // treated as blocks), so the list holds only element pointers.
  void PushActiveFormatting(dom::Element* element);
  void RemoveFromActiveFormatting(dom::Element* element);
  bool InActiveFormatting(dom::Element* element) const;
  bool InStack(dom::Element* element) const;
  bool ElementInScope(dom::Element* element) const;
  void ReconstructActiveFormatting();
  void RunAdoptionAgency(std::string_view subject);
  dom::Element* CloneElement(const dom::Element& source);

  std::string_view html_;
  Tokenizer tokenizer_;
  std::optional<Token> pending_;
  std::unique_ptr<dom::Document> document_;
  std::vector<dom::Element*> stack_;
  std::vector<dom::Element*> active_formatting_;
  Mode mode_ = Mode::kInitial;
  Mode mode_before_text_ = Mode::kInBody;
};

}  // namespace neko::html
