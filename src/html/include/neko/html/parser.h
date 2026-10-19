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

  // Fragment parsing (WHATWG HTML 13.4 "Parsing HTML fragments"): parses |html|
  // with |context| as the fragment context element.  The tokenizer state is
  // initialized from |context| (title/textarea -> RCDATA; style/xmp/iframe/
  // noembed/noframes/noscript -> RAWTEXT; script -> script data; plaintext ->
  // PLAINTEXT; otherwise data), and the insertion mode is derived by
  // ResetInsertionMode() from the context element.  |context| must outlive the
  // parse; it is only read (its tag name), never modified or inserted.
  Parser(std::string_view html, const dom::Element& context);

  // Parses the document.  Never returns null; malformed HTML produces a
  // best-effort DOM.
  std::unique_ptr<dom::Document> Parse();

  // Parses the fragment.  Returns the fragment's nodes (the children of the
  // fragment), never null.  Scripts in the fragment are not executed.  Must be
  // called on a parser built with the (html, context) constructor.
  std::unique_ptr<dom::DocumentFragment> ParseFragment();

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
    kInSelect,
    kInSelectInTable,
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
  // "in select" / "in select in table" insertion modes (13.2.6.4.16/17).
  void ProcessInSelect(Token token);
  void ProcessInSelectInTable(Token token);
  // Runs |token| with the "in body" rules from another insertion mode,
  // restoring the mode unless the rules changed it themselves.
  void ProcessWithInBodyRules(Token token);

  // The tree construction dispatcher (WHATWG 13.2.6) and the rules for parsing
  // tokens in foreign content (13.2.6.5).  When the adjusted current node is
  // not in the HTML namespace (and the token is not one of the integration
  // point exceptions), the token is handled by the foreign content rules
  // instead of the current insertion mode.
  dom::Element* AdjustedCurrentNode() const;
  bool IsAdjustedCurrentNodeForeign() const;
  bool ShouldProcessInForeignContent(const Token& token) const;
  void ProcessForeignContent(Token token);

  // Inserts |element| into the document and returns it as a non-owning
  // pointer (nullptr when the depth guard drops it): the tree takes
  // ownership on insertion, so callers hand over the unique_ptr.
  dom::Element* InsertElement(std::unique_ptr<dom::Element> element);
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
  // WHATWG HTML 13.2.5.3: redirect insertions addressed at a <template>
  // element into its template contents fragment.
  static std::pair<dom::Node*, dom::Node*> RerouteTemplateInsertion(dom::Node* parent,
                                                                    dom::Node* before);
  // WHATWG HTML 13.4: in fragment parsing, insertions whose adjusted insertion
  // location is the synthetic root html element are redirected into the
  // fragment (the spec models this with an insertion target redirection map).
  // A no-op outside fragment parsing.
  dom::Node* FragmentInsertionTarget(dom::Node* parent) const;
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

  // Fragment parsing state (WHATWG HTML 13.4): the context element provided by
  // the (html, context) constructor, the synthetic root <html> element that
  // seeds the stack of open elements, and the document fragment that receives
  // the parsed nodes.  All three are null/inactive for document parsing.
  const dom::Element* fragment_context_ = nullptr;
  dom::Element* fragment_root_ = nullptr;
  std::unique_ptr<dom::DocumentFragment> fragment_;
};

} // namespace neko::html
