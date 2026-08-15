#pragma once

#include "neko/dom/element.h"
#include "neko/html/token.h"
#include "neko/html/tokenizer.h"

#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace neko::html {

// HTML tree construction (WHATWG 13.2.6-inspired).
//
// Implements the insertion modes: initial, before html, before head, in head,
// after head, in body, text, in table, in table text, in caption, in column
// group, in table body, in row, in cell, after body, after after body.  Active
// formatting elements and the adoption agency algorithm are implemented,
// including markers.  Table content uses foster parenting (13.2.6.1); see
// docs/html/README.md.
class Parser
{
public:
  explicit Parser(std::string_view html);

  // Parses the document.  Never returns null; malformed HTML produces a
  // best-effort DOM.
  std::unique_ptr<dom::Document> Parse();

private:
  enum class Mode
  {
    kInitial,
    kBeforeHtml,
    kBeforeHead,
    kInHead,
    kAfterHead,
    kInBody,
    kText,
    kInTable,
    kInTableText,
    kInCaption,
    kInColumnGroup,
    kInTableBody,
    kInRow,
    kInCell,
    kAfterBody,
    kAfterAfterBody,
  };

  void ProcessToken(Token token);
  void ProcessStartTag(Token token);
  void ProcessEndTag(Token token);
  void ProcessCharacter(Token token);
  void ProcessComment(Token token);
  void ProcessStartTagInTable(Token token);
  void ProcessEndTagInTable(Token token);

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
  bool InTableScope(std::string_view tag) const;
  bool InListItemScope(std::string_view tag) const;
  dom::Element* FindInStack(std::string_view tag) const;
  void ClosePElement();
  bool IsVoidElement(std::string_view tag) const;
  bool IsBlockElement(std::string_view tag) const;
  bool IsRawTextElement(std::string_view tag) const;

  void GenerateImpliedEndTags(std::optional<std::string_view> except = std::nullopt);
  void ClearStackBackToTableContext();
  void ClearStackBackToTableBodyContext();
  void ClearStackBackToTableRowContext();
  void CloseTableCell();
  void ResetInsertionMode();
  void PushFormattingMarker();
  void ClearActiveFormattingToMarker();

  // The adjusted insertion location (13.2.6.1): a (parent, before) pair.
  // |before| is null to append.  Honors foster parenting.
  std::pair<dom::Node*, dom::Node*> AdjustedInsertionLocation() const;
  void AppendTextAt(dom::Node* parent, dom::Node* before, std::string_view text);
  void InsertNodeAt(dom::Node* parent, dom::Node* before, std::unique_ptr<dom::Node> node);

  // Processes |token| with the rules for the "in head" insertion mode, used
  // when after-head content must be appended to the head element.
  void
  ProcessInHead(Token token); // Maximum element nesting depth while building the DOM.  Over-deep
  // subtrees are dropped so that pathological HTML (e.g. hundreds of
  // thousands of nested <div>s) cannot overflow the stack in the style,
  // layout or paint stages that recursively walk the tree.
  static constexpr std::size_t kMaxDepth = 512;
  // While > 0, an over-deep subtree is being discarded: start tags
  // increment it, end tags decrement it, and no DOM nodes are created.
  std::size_t skip_depth_ = 0;

  // Active formatting elements (WHATWG 13.2.4.3) and the adoption agency
  // algorithm (13.2.6.4.7).  Markers are represented by null entries.
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
  Mode original_mode_ = Mode::kInBody;
  dom::Element* head_element_ = nullptr;
  bool foster_parenting_ = false;
  // Pending character tokens for the "in table text" insertion mode.
  std::vector<Token> pending_table_chars_;
  Mode mode_before_text_ = Mode::kInBody;
};

} // namespace neko::html
