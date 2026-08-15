#pragma once

#include "neko/html/token.h"

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace neko::html {

// HTML tokenizer (WHATWG 13.2.5-inspired state machine).
//
// Supported features: tags, attributes (all quoting styles), comments,
// doctype (including public/system identifiers), character references
// (numeric + the full WHATWG named table), raw text elements (script: script
// data with escape/double-escape states; style/xmp/iframe/noembed: RAWTEXT;
// title/textarea: RCDATA), plaintext, newline normalization, and the
// attribute-context character reference rules.
//
// Known deviations (see docs/html/README.md):
//   - CDATA sections and processing instructions are not tokenized (foreign
//     content is not produced).
class Tokenizer
{
public:
  explicit Tokenizer(std::string_view input);

  // The next token; returns kEOF once exhausted (and forever after).
  Token Next();

  // Switches to raw text mode for |tag_name| (script/style -> RAWTEXT,
  // title/textarea -> RCDATA).  The parser calls this right after emitting
  // the start tag of a raw text element.  The tokenizer returns to data mode
  // automatically after the matching end tag.
  void StartRawText(std::string_view tag_name);

  // Switches to the PLAINTEXT state (13.2.5.5): everything until EOF is
  // emitted as character tokens.  The parser calls this after emitting a
  // "plaintext" start tag.
  void StartPlaintext();

private:
  enum class State
  {
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
    kAfterDoctypePublicKeyword,
    kBeforeDoctypePublicIdentifier,
    kDoctypePublicIdentifierDoubleQuoted,
    kDoctypePublicIdentifierSingleQuoted,
    kAfterDoctypePublicIdentifier,
    kBetweenDoctypePublicAndSystemIdentifiers,
    kAfterDoctypeSystemKeyword,
    kBeforeDoctypeSystemIdentifier,
    kDoctypeSystemIdentifierDoubleQuoted,
    kDoctypeSystemIdentifierSingleQuoted,
    kAfterDoctypeSystemIdentifier,
    kBogusDoctype,
    kPlaintext,
    kRcdata,
    kRcdataLessThanSign,
    kRcdataEndTagOpen,
    kRcdataEndTagName,
    kRawtext,
    kRawtextLessThanSign,
    kRawtextEndTagOpen,
    kRawtextEndTagName,
    kScriptData,
    kScriptDataLessThanSign,
    kScriptDataEndTagOpen,
    kScriptDataEndTagName,
    kScriptDataEscapeStart,
    kScriptDataEscapeStartDash,
    kScriptDataEscaped,
    kScriptDataEscapedDash,
    kScriptDataEscapedDashDash,
    kScriptDataEscapedLessThanSign,
    kScriptDataEscapedEndTagOpen,
    kScriptDataEscapedEndTagName,
    kScriptDataDoubleEscapeStart,
    kScriptDataDoubleEscaped,
    kScriptDataDoubleEscapedDash,
    kScriptDataDoubleEscapedDashDash,
    kScriptDataDoubleEscapedLessThanSign,
    kScriptDataDoubleEscapeEnd,
  };

  void ProcessChar(char c);
  void ProcessEof();
  void ReconsumeIn(State state);
  void Emit(Token token);
  void FlushCharacterRun();
  void EmitDoctype();
  std::string ConsumeCharacterReference(bool in_attribute);
  std::string_view Peek(std::size_t offset) const;
  void Skip(std::size_t count);
  bool MatchIgnoreCase(std::string_view text) const;

  std::string_view input_;
  std::string owned_input_; // normalized copy kept alive when input_ is transformed
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
  std::string doctype_public_id_;
  std::string doctype_system_id_;
  bool doctype_force_quirks_ = false;
  bool doctype_public_set_ = false; // public id present (possibly empty)
  bool doctype_system_set_ = false; // system id present (possibly empty)

  // Character run buffer.
  std::string char_run_;

  // Raw text (RAWTEXT/RCDATA) handling.
  std::string raw_text_tag_;
  std::string pending_text_; // "</..." candidate text if the end tag fails
  std::string end_tag_name_; // accumulated end-tag name while matching

  // Script data handling (WHATWG "temporary buffer").
  std::string temp_buffer_;
};

} // namespace neko::html
