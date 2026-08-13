#pragma once

#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "neko/html/token.h"

namespace neko::html {

// HTML tokenizer (WHATWG 13.2.5-inspired state machine).
//
// Supported features: tags, attributes (all quoting styles), comments,
// doctype, character references (numeric + a common named subset), raw text
// elements (script/style: RAWTEXT; title/textarea: RCDATA).
//
// Known deviations (see docs/html/README.md):
//   - script content uses RAWTEXT semantics (no JS escape states);
//   - only a subset of the WHATWG named character reference table;
//   - CDATA sections and <! handling are simplified.
class Tokenizer {
 public:
  explicit Tokenizer(std::string_view input);

  // The next token; returns kEOF once exhausted (and forever after).
  Token Next();

  // Switches to raw text mode for |tag_name| (script/style -> RAWTEXT,
  // title/textarea -> RCDATA).  The parser calls this right after emitting
  // the start tag of a raw text element.  The tokenizer returns to data mode
  // automatically after the matching end tag.
  void StartRawText(std::string_view tag_name);

 private:
  enum class State {
    kData,
    kTagOpen,
    kEndTagOpen,
    kTagName,
    kBeforeAttributeName,
    kAttributeName,
    kAfterAttributeName,
    kBeforeAttributeValue,
    kAttributeValueDoubleQuoted,
    kAttributeValueSingleQuoted,
    kAttributeValueUnquoted,
    kAfterAttributeValueQuoted,
    kSelfClosingStartTag,
    kMarkupDeclarationOpen,
    kBogusComment,
    kCommentStart,
    kCommentStartDash,
    kComment,
    kCommentEndDash,
    kCommentEnd,
    kCommentEndBang,
    kBeforeDoctypeName,
    kDoctypeName,
    kAfterDoctypeName,
    kRcdata,
    kRcdataLessThanSign,
    kRcdataEndTagOpen,
    kRcdataEndTagName,
    kRawtext,
    kRawtextLessThanSign,
    kRawtextEndTagOpen,
    kRawtextEndTagName,
  };

  void ProcessChar(char c);
  void ProcessEof();
  void ReconsumeIn(State state);
  void Emit(Token token);
  void FlushCharacterRun();
  std::string ConsumeCharacterReference();
  std::string_view Peek(std::size_t offset) const;
  void Skip(std::size_t count);
  bool MatchIgnoreCase(std::string_view text) const;

  std::string_view input_;
  std::size_t pos_ = 0;
  State state_ = State::kData;
  bool eof_ = false;
  bool reconsume_ = false;
  char last_char_ = '\0';

  std::deque<Token> out_;
  Token pending_;

  // Current tag construction.
  std::string tag_name_;
  std::vector<Attribute> attributes_;
  std::string attribute_name_;
  std::string attribute_value_;
  bool self_closing_ = false;
  bool is_end_tag_ = false;

  // Comment / doctype construction.
  std::string comment_data_;
  std::string doctype_name_;

  // Character run buffer.
  std::string char_run_;

  // Raw text (RAWTEXT/RCDATA) handling.
  std::string raw_text_tag_;
  std::string pending_text_;  // "</..." candidate text if the end tag fails
  std::string end_tag_name_;  // accumulated end-tag name while matching
};

}  // namespace neko::html
