#include "neko/html/tokenizer.h"

#include "neko/base/utf8.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace neko::html {
namespace {

// WHATWG ASCII whitespace: tab, LF, FF, CR, space.
bool IsWhitespace(char c)
{
  return c == '\t' || c == '\n' || c == '\x0C' || c == '\r' || c == ' ';
}

// A named character reference: name (without '&' or ';') mapped to up to two
// code points (a few WHATWG references are two-codepoint sequences).
struct NamedEntity
{
  const char* name;
  char32_t codepoints[2];
  int count;
};

// The full WHATWG named character reference table (2125 entries, sorted by
// name) plus the legacy no-semicolon name set, generated from the official
// entities.json (see tools/gen_html_entities.py).
#include "entities_generated.inc"

// Entities that may omit the trailing semicolon (WHATWG legacy set).
bool IsLegacyNoSemicolon(std::string_view name)
{
  return std::binary_search(std::begin(kLegacyNoSemicolon), std::end(kLegacyNoSemicolon), name);
}

// Binary search over the sorted full table.
const NamedEntity* LookupNamedEntity(std::string_view name)
{
  const auto it = std::lower_bound(std::begin(kNamedEntities),
                                   std::end(kNamedEntities),
                                   name,
                                   [](const NamedEntity& entry, std::string_view target) {
                                     return std::string_view(entry.name) < target;
                                   });
  if (it != std::end(kNamedEntities) && std::string_view(it->name) == name) {
    return it;
  }
  return nullptr;
}

int HexDigitValue(char c)
{
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

} // namespace

Tokenizer::Tokenizer(std::string_view input)
{
  // Preprocess the input stream by normalizing newlines (WHATWG 13.2.3.5):
  // CRLF and CR become LF so that U+000D never reaches the tokenization stage.
  if (input.find('\r') == std::string_view::npos) {
    input_ = input;
  } else {
    owned_input_.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
      const char c = input[i];
      if (c == '\r') {
        owned_input_.push_back('\n');
        if (i + 1 < input.size() && input[i + 1] == '\n') {
          ++i; // CRLF -> LF
        }
      } else {
        owned_input_.push_back(c);
      }
    }
    input_ = owned_input_;
  }
}

void Tokenizer::StartRawText(std::string_view tag_name)
{
  raw_text_tag_ = std::string(tag_name);
  if (tag_name == "title" || tag_name == "textarea") {
    state_ = State::kRcdata;
  } else if (tag_name == "script") {
    state_ = State::kScriptData;
  } else {
    state_ = State::kRawtext;
  }
}

void Tokenizer::StartPlaintext()
{
  state_ = State::kPlaintext;
}

std::string_view Tokenizer::Peek(std::size_t offset) const
{
  return (pos_ + offset < input_.size()) ? input_.substr(pos_ + offset) : std::string_view{};
}

void Tokenizer::Skip(std::size_t count)
{
  pos_ += count;
}

bool Tokenizer::MatchIgnoreCase(std::string_view text) const
{
  const std::string_view remaining = Peek(0);
  if (remaining.size() < text.size()) {
    return false;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    char a = remaining[i];
    char b = text[i];
    if (a >= 'A' && a <= 'Z') {
      a = static_cast<char>(a + 32);
    }
    if (b >= 'A' && b <= 'Z') {
      b = static_cast<char>(b + 32);
    }
    if (a != b) {
      return false;
    }
  }
  return true;
}

void Tokenizer::ReconsumeIn(State state)
{
  state_ = state;
  reconsume_ = true;
}

void Tokenizer::Emit(Token token)
{
  out_.push_back(std::move(token));
}

void Tokenizer::FlushCharacterRun()
{
  if (char_run_.empty()) {
    return;
  }
  Emit(Token::MakeCharacter(std::move(char_run_)));
  char_run_.clear();
}

void Tokenizer::EmitDoctype()
{
  Token token = Token::MakeDoctype(doctype_name_, doctype_force_quirks_);
  if (doctype_public_set_) {
    token.public_id = doctype_public_id_;
  }
  if (doctype_system_set_) {
    token.system_id = doctype_system_id_;
  }
  Emit(std::move(token));
}

std::string Tokenizer::ConsumeCharacterReference(bool in_attribute)
{
  // pos_ points just past '&'.
  const std::string_view rest = Peek(0);
  if (rest.empty()) {
    return "&";
  }
  if (rest[0] == '#') {
    ++pos_;
    const std::string_view num = Peek(0);
    bool hex = false;
    std::size_t digits_start = 0;
    if (!num.empty() && (num[0] == 'x' || num[0] == 'X')) {
      hex = true;
      ++pos_;
      digits_start = 1;
    }
    char32_t code_point = 0;
    bool any_digit = false;
    std::size_t i = digits_start;
    for (; i < num.size(); ++i) {
      const int v = HexDigitValue(num[i]);
      if (v < 0) {
        break;
      }
      any_digit = true;
      code_point = code_point * (hex ? 16 : 10) + static_cast<char32_t>(v);
      if (code_point > 0x10FFFF) {
        code_point = 0xFFFD;
      }
      ++pos_;
    }
    if (!any_digit) {
      // Not a numeric reference; rewind to just after '#'.
      pos_ -= (digits_start > 0) ? 1 : 0;
      return "&#";
    }
    if (i < num.size() && num[i] == ';') {
      ++pos_;
    }
    if (code_point == 0 || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
      code_point = 0xFFFD;
    }
    return base::EncodeUtf8(code_point);
  }

  // Named reference: scan [a-zA-Z0-9]* and take the longest table match.
  // The reference is only consumed if the character after the matched name
  // is ';' (or a legacy no-semicolon name not followed by a name char or, in
  // an attribute, not followed by '='); if the name is a prefix of a longer
  // alnum run (e.g. "amp" in "&ampfoo;") the whole sequence is emitted
  // literally (WHATWG 13.2.5.78).
  const std::size_t start = pos_;
  while (pos_ < input_.size() && IsAsciiAlnum(input_[pos_])) {
    ++pos_;
  }
  const std::string_view scanned = input_.substr(start, pos_ - start);
  for (std::size_t len = scanned.size(); len > 0; --len) {
    const std::string_view candidate = scanned.substr(0, len);
    const NamedEntity* entry = LookupNamedEntity(candidate);
    if (entry == nullptr) {
      continue;
    }
    const std::size_t after = start + len;
    const bool followed_by_semicolon = after < input_.size() && input_[after] == ';';
    const bool followed_by_alnum = after < input_.size() && IsAsciiAlnum(input_[after]);
    const bool followed_by_equals = after < input_.size() && input_[after] == '=';
    auto encode = [&]() {
      std::string out;
      for (int i = 0; i < entry->count; ++i) {
        out += base::EncodeUtf8(entry->codepoints[i]);
      }
      return out;
    };
    if (followed_by_semicolon) {
      pos_ = after + 1;
      return encode();
    }
    // In an attribute, a legacy name directly followed by '=' or an ASCII
    // alnum is not a reference (13.2.5.78 "for historical reasons").
    if (IsLegacyNoSemicolon(candidate) && !followed_by_alnum &&
        !(in_attribute && followed_by_equals)) {
      pos_ = after;
      return encode();
    }
    // The candidate is a prefix of a longer name; not a valid reference.
  }
  pos_ = start;
  return "&";
}

void Tokenizer::ProcessEof()
{
  FlushCharacterRun();
  // A pending "</..." literal that was being matched is emitted as text.
  if (!pending_text_.empty()) {
    Emit(Token::MakeCharacter(std::move(pending_text_)));
    pending_text_.clear();
  }
  switch (state_) {
  case State::kData:
  case State::kRcdata:
  case State::kRawtext:
  case State::kScriptData:
  case State::kScriptDataLessThanSign:
  case State::kScriptDataEndTagOpen:
  case State::kScriptDataEndTagName:
  case State::kScriptDataEscapeStart:
  case State::kScriptDataEscapeStartDash:
  case State::kScriptDataEscaped:
  case State::kScriptDataEscapedDash:
  case State::kScriptDataEscapedDashDash:
  case State::kScriptDataEscapedLessThanSign:
  case State::kScriptDataEscapedEndTagOpen:
  case State::kScriptDataEscapedEndTagName:
  case State::kScriptDataDoubleEscapeStart:
  case State::kScriptDataDoubleEscaped:
  case State::kScriptDataDoubleEscapedDash:
  case State::kScriptDataDoubleEscapedDashDash:
  case State::kScriptDataDoubleEscapedLessThanSign:
  case State::kScriptDataDoubleEscapeEnd:
  case State::kPlaintext:
    break;
  case State::kTagOpen:
    // eof-before-tag-name: emit the literal '<' and then EOF.
    char_run_.push_back('<');
    FlushCharacterRun();
    break;
  case State::kEndTagOpen:
    // eof-before-tag-name: emit the literal '</' and then EOF.
    char_run_.append("</");
    FlushCharacterRun();
    break;
  case State::kTagName:
  case State::kBeforeAttributeName:
  case State::kAttributeName:
  case State::kAfterAttributeName:
  case State::kBeforeAttributeValue:
  case State::kAttributeValueDoubleQuoted:
  case State::kAttributeValueSingleQuoted:
  case State::kAttributeValueUnquoted:
  case State::kAfterAttributeValueQuoted:
  case State::kSelfClosingStartTag:
    // eof-in-tag: the tag is ignored entirely (13.2.5.6-8, 13.2.5.32-40).
    break;
  case State::kCommentStart:
  case State::kCommentStartDash:
  case State::kComment:
  case State::kCommentEndDash:
  case State::kCommentEnd:
  case State::kCommentEndBang:
  case State::kBogusComment:
    Emit(Token::MakeComment(std::move(comment_data_)));
    break;
  case State::kBeforeDoctypeName:
  case State::kDoctypeName:
  case State::kAfterDoctypeName:
  case State::kAfterDoctypePublicKeyword:
  case State::kBeforeDoctypePublicIdentifier:
  case State::kDoctypePublicIdentifierDoubleQuoted:
  case State::kDoctypePublicIdentifierSingleQuoted:
  case State::kAfterDoctypePublicIdentifier:
  case State::kBetweenDoctypePublicAndSystemIdentifiers:
  case State::kAfterDoctypeSystemKeyword:
  case State::kBeforeDoctypeSystemIdentifier:
  case State::kDoctypeSystemIdentifierDoubleQuoted:
  case State::kDoctypeSystemIdentifierSingleQuoted:
  case State::kAfterDoctypeSystemIdentifier:
  case State::kBogusDoctype:
    doctype_force_quirks_ = true; // eof-in-doctype
    EmitDoctype();
    break;
  default:
    break;
  }
  // Ensure a single EOF token afterwards.
  state_ = State::kData;
}

Token Tokenizer::Next()
{
  for (;;) {
    if (!out_.empty()) {
      Token token = std::move(out_.front());
      out_.pop_front();
      return token;
    }
    if (eof_) {
      return Token::MakeEOF();
    }
    if (pos_ >= input_.size()) {
      eof_ = true;
      ProcessEof();
      continue;
    }
    if (reconsume_) {
      // Reprocess the character that was consumed just before ReconsumeIn()
      // was called (the buffer position has already moved past it).
      reconsume_ = false;
      ProcessChar(last_char_);
    } else {
      last_char_ = input_[pos_++];
      ProcessChar(last_char_);
    }
  }
}

void Tokenizer::ProcessChar(char c)
{
  switch (state_) {
  case State::kData:
    if (c == '<') {
      FlushCharacterRun();
      state_ = State::kTagOpen;
    } else if (c == '&') {
      char_run_ += ConsumeCharacterReference(/*in_attribute=*/false);
    } else {
      char_run_.push_back(c);
    }
    break;

  case State::kTagOpen:
    is_end_tag_ = false;
    if (c == '!') {
      state_ = State::kMarkupDeclarationOpen;
    } else if (c == '/') {
      is_end_tag_ = true;
      state_ = State::kEndTagOpen;
    } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      tag_name_.clear();
      attributes_.clear();
      tag_name_.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
      state_ = State::kTagName;
    } else if (c == '?') {
      comment_data_.clear();
      comment_data_.push_back(c);
      state_ = State::kBogusComment;
    } else {
      // '<' not followed by a tag: literal text.
      FlushCharacterRun();
      char_run_.push_back('<');
      ReconsumeIn(State::kData);
    }
    break;

  case State::kEndTagOpen:
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      tag_name_.clear();
      attributes_.clear();
      tag_name_.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
      state_ = State::kTagName;
    } else {
      // "</" not followed by a letter: literal text.
      FlushCharacterRun();
      char_run_.append("</");
      ReconsumeIn(State::kData);
    }
    break;

  case State::kTagName:
    if (IsAsciiWhitespace(c)) {
      state_ = State::kBeforeAttributeName;
    } else if (c == '/') {
      state_ = State::kSelfClosingStartTag;
    } else if (c == '>') {
      if (is_end_tag_) {
        Emit(Token::MakeEndTag(std::move(tag_name_)));
      } else {
        Emit(Token::MakeStartTag(std::move(tag_name_), std::move(attributes_), self_closing_));
      }
      self_closing_ = false;
      is_end_tag_ = false;
      state_ = State::kData;
    } else {
      tag_name_.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
    }
    break;

  case State::kBeforeAttributeName:
    if (IsAsciiWhitespace(c)) {
      // stay
    } else if (c == '>' || c == '/') {
      // End of the start tag / self-closing slash: go to after-attribute
      // name so no empty-named attribute is created for `<div />`.
      ReconsumeIn(State::kAfterAttributeName);
    } else if (c == '<' || c == '=') {
      ReconsumeIn(State::kAttributeName);
    } else {
      attribute_name_.clear();
      attribute_value_.clear();
      ReconsumeIn(State::kAttributeName);
    }
    break;

  case State::kAttributeName:
    if (IsAsciiWhitespace(c) || c == '/' || c == '>') {
      // Attribute without a value.
      attributes_.push_back(Attribute{std::move(attribute_name_), std::string()});
      attribute_name_.clear();
      ReconsumeIn(State::kAfterAttributeName);
    } else if (c == '=') {
      state_ = State::kBeforeAttributeValue;
    } else if (c == '<' || c == '"' || c == '\'' || c == '=') {
      attribute_name_.push_back(c);
    } else {
      attribute_name_.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
    }
    break;

  case State::kAfterAttributeName:
    if (IsAsciiWhitespace(c)) {
      // stay
    } else if (c == '/') {
      state_ = State::kSelfClosingStartTag;
    } else if (c == '=') {
      state_ = State::kBeforeAttributeValue;
    } else if (c == '>') {
      if (is_end_tag_) {
        Emit(Token::MakeEndTag(std::move(tag_name_)));
      } else {
        Emit(Token::MakeStartTag(std::move(tag_name_), std::move(attributes_), self_closing_));
      }
      self_closing_ = false;
      is_end_tag_ = false;
      state_ = State::kData;
    } else {
      attribute_name_.clear();
      attribute_value_.clear();
      ReconsumeIn(State::kAttributeName);
    }
    break;

  case State::kBeforeAttributeValue:
    if (IsAsciiWhitespace(c)) {
      // stay
    } else if (c == '"') {
      attribute_value_.clear();
      state_ = State::kAttributeValueDoubleQuoted;
    } else if (c == '\'') {
      attribute_value_.clear();
      state_ = State::kAttributeValueSingleQuoted;
    } else if (c == '>') {
      attributes_.push_back(Attribute{std::move(attribute_name_), std::string()});
      attribute_name_.clear();
      if (is_end_tag_) {
        Emit(Token::MakeEndTag(std::move(tag_name_)));
      } else {
        Emit(Token::MakeStartTag(std::move(tag_name_), std::move(attributes_), self_closing_));
      }
      self_closing_ = false;
      is_end_tag_ = false;
      state_ = State::kData;
    } else {
      attribute_value_.clear();
      ReconsumeIn(State::kAttributeValueUnquoted);
    }
    break;

  case State::kAttributeValueDoubleQuoted:
    if (c == '"') {
      attributes_.push_back(Attribute{std::move(attribute_name_), std::move(attribute_value_)});
      attribute_name_.clear();
      state_ = State::kAfterAttributeValueQuoted;
    } else if (c == '&') {
      attribute_value_ += ConsumeCharacterReference(/*in_attribute=*/true);
    } else {
      attribute_value_.push_back(c);
    }
    break;

  case State::kAttributeValueSingleQuoted:
    if (c == '\'') {
      attributes_.push_back(Attribute{std::move(attribute_name_), std::move(attribute_value_)});
      attribute_name_.clear();
      state_ = State::kAfterAttributeValueQuoted;
    } else if (c == '&') {
      attribute_value_ += ConsumeCharacterReference(/*in_attribute=*/true);
    } else {
      attribute_value_.push_back(c);
    }
    break;

  case State::kAttributeValueUnquoted:
    if (IsAsciiWhitespace(c)) {
      attributes_.push_back(Attribute{std::move(attribute_name_), std::move(attribute_value_)});
      attribute_name_.clear();
      state_ = State::kBeforeAttributeName;
    } else if (c == '>') {
      attributes_.push_back(Attribute{std::move(attribute_name_), std::move(attribute_value_)});
      attribute_name_.clear();
      if (is_end_tag_) {
        Emit(Token::MakeEndTag(std::move(tag_name_)));
      } else {
        Emit(Token::MakeStartTag(std::move(tag_name_), std::move(attributes_), self_closing_));
      }
      self_closing_ = false;
      is_end_tag_ = false;
      state_ = State::kData;
    } else if (c == '&') {
      attribute_value_ += ConsumeCharacterReference(/*in_attribute=*/true);
    } else {
      attribute_value_.push_back(c);
    }
    break;

  case State::kAfterAttributeValueQuoted:
    if (IsAsciiWhitespace(c)) {
      state_ = State::kBeforeAttributeName;
    } else if (c == '/') {
      state_ = State::kSelfClosingStartTag;
    } else if (c == '>') {
      if (is_end_tag_) {
        Emit(Token::MakeEndTag(std::move(tag_name_)));
      } else {
        Emit(Token::MakeStartTag(std::move(tag_name_), std::move(attributes_), self_closing_));
      }
      self_closing_ = false;
      is_end_tag_ = false;
      state_ = State::kData;
    } else {
      ReconsumeIn(State::kBeforeAttributeName);
    }
    break;

  case State::kSelfClosingStartTag:
    if (c == '>') {
      if (is_end_tag_) {
        Emit(Token::MakeEndTag(std::move(tag_name_)));
      } else {
        self_closing_ = true;
        Emit(Token::MakeStartTag(std::move(tag_name_), std::move(attributes_), self_closing_));
      }
      self_closing_ = false;
      is_end_tag_ = false;
      state_ = State::kData;
    } else if (IsAsciiWhitespace(c)) {
      // stay
    } else {
      ReconsumeIn(State::kBeforeAttributeName);
    }
    break;

  case State::kMarkupDeclarationOpen:
    // |c| is the first character after "<!".  "--" starts a comment and
    // "DOCTYPE" starts a doctype; anything else is a bogus comment.
    if (c == '-' && Peek(0).size() >= 1 && Peek(0)[0] == '-') {
      Skip(1); // the second '-'
      comment_data_.clear();
      state_ = State::kCommentStart;
    } else if ((c == 'd' || c == 'D') && MatchIgnoreCase("OCTYPE")) {
      Skip(6);
      state_ = State::kBeforeDoctypeName;
    } else {
      comment_data_.clear();
      comment_data_.push_back(c);
      state_ = State::kBogusComment;
    }
    break;

  case State::kBogusComment:
    if (c == '>') {
      Emit(Token::MakeComment(std::move(comment_data_)));
      comment_data_.clear();
      state_ = State::kData;
    } else {
      comment_data_.push_back(c);
    }
    break;

  case State::kCommentStart:
    if (c == '-') {
      state_ = State::kCommentStartDash;
    } else if (c == '>') {
      Emit(Token::MakeComment(std::string()));
      state_ = State::kData;
    } else {
      comment_data_.push_back(c);
      state_ = State::kComment;
    }
    break;

  case State::kCommentStartDash:
    if (c == '-') {
      state_ = State::kCommentEnd;
    } else if (c == '>') {
      Emit(Token::MakeComment(std::move(comment_data_)));
      comment_data_.clear();
      state_ = State::kData;
    } else {
      comment_data_.push_back('-');
      comment_data_.push_back(c);
      state_ = State::kComment;
    }
    break;

  case State::kComment:
    if (c == '-') {
      state_ = State::kCommentEndDash;
    } else {
      comment_data_.push_back(c);
    }
    break;

  case State::kCommentEndDash:
    if (c == '-') {
      state_ = State::kCommentEnd;
    } else {
      comment_data_.push_back('-');
      comment_data_.push_back(c);
      state_ = State::kComment;
    }
    break;

  case State::kCommentEnd:
    if (c == '>') {
      Emit(Token::MakeComment(std::move(comment_data_)));
      comment_data_.clear();
      state_ = State::kData;
    } else if (c == '!') {
      state_ = State::kCommentEndBang;
    } else if (c == '-') {
      comment_data_.push_back('-');
    } else {
      comment_data_.append("--");
      comment_data_.push_back(c);
      state_ = State::kComment;
    }
    break;

  case State::kCommentEndBang:
    if (c == '-') {
      comment_data_.append("--!");
      state_ = State::kCommentEndDash;
    } else if (c == '>') {
      Emit(Token::MakeComment(std::move(comment_data_)));
      comment_data_.clear();
      state_ = State::kData;
    } else {
      comment_data_.append("--!");
      comment_data_.push_back(c);
      state_ = State::kComment;
    }
    break;

  case State::kBeforeDoctypeName:
    if (IsAsciiWhitespace(c)) {
      // stay
    } else if (c == '>') {
      doctype_force_quirks_ = true; // missing-doctype-name
      EmitDoctype();
      state_ = State::kData;
    } else {
      doctype_name_.clear();
      doctype_public_id_.clear();
      doctype_system_id_.clear();
      doctype_public_set_ = false;
      doctype_system_set_ = false;
      doctype_force_quirks_ = false;
      doctype_name_.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
      state_ = State::kDoctypeName;
    }
    break;

  case State::kDoctypeName:
    if (IsAsciiWhitespace(c)) {
      state_ = State::kAfterDoctypeName;
    } else if (c == '>') {
      EmitDoctype();
      doctype_name_.clear();
      state_ = State::kData;
    } else {
      doctype_name_.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
    }
    break;

  case State::kAfterDoctypeName:
    if (IsAsciiWhitespace(c)) {
      // stay
    } else if (c == '>') {
      EmitDoctype();
      doctype_name_.clear();
      state_ = State::kData;
    } else if ((c == 'p' || c == 'P') && MatchIgnoreCase("UBLIC")) {
      Skip(5);
      state_ = State::kAfterDoctypePublicKeyword;
    } else if ((c == 's' || c == 'S') && MatchIgnoreCase("YSTEM")) {
      Skip(5);
      state_ = State::kAfterDoctypeSystemKeyword;
    } else {
      // invalid-character-sequence-after-doctype-name
      doctype_force_quirks_ = true;
      ReconsumeIn(State::kBogusDoctype);
    }
    break;

  case State::kAfterDoctypePublicKeyword:
    if (IsAsciiWhitespace(c)) {
      state_ = State::kBeforeDoctypePublicIdentifier;
    } else if (c == '"') {
      doctype_public_set_ = true;
      doctype_public_id_.clear();
      state_ = State::kDoctypePublicIdentifierDoubleQuoted;
    } else if (c == '\'') {
      doctype_public_set_ = true;
      doctype_public_id_.clear();
      state_ = State::kDoctypePublicIdentifierSingleQuoted;
    } else if (c == '>') {
      doctype_force_quirks_ = true; // missing-doctype-public-identifier
      EmitDoctype();
      state_ = State::kData;
    } else {
      doctype_force_quirks_ = true;
      ReconsumeIn(State::kBogusDoctype);
    }
    break;

  case State::kBeforeDoctypePublicIdentifier:
    if (IsAsciiWhitespace(c)) {
      // stay
    } else if (c == '"') {
      doctype_public_set_ = true;
      doctype_public_id_.clear();
      state_ = State::kDoctypePublicIdentifierDoubleQuoted;
    } else if (c == '\'') {
      doctype_public_set_ = true;
      doctype_public_id_.clear();
      state_ = State::kDoctypePublicIdentifierSingleQuoted;
    } else if (c == '>') {
      doctype_force_quirks_ = true; // missing-doctype-public-identifier
      EmitDoctype();
      state_ = State::kData;
    } else {
      doctype_force_quirks_ = true;
      ReconsumeIn(State::kBogusDoctype);
    }
    break;

  case State::kDoctypePublicIdentifierDoubleQuoted:
    if (c == '"') {
      state_ = State::kAfterDoctypePublicIdentifier;
    } else if (c == '\0') {
      doctype_public_id_ += "\xEF\xBF\xBD";
    } else if (c == '>') {
      doctype_force_quirks_ = true; // abrupt-doctype-public-identifier
      EmitDoctype();
      state_ = State::kData;
    } else {
      doctype_public_id_.push_back(c);
    }
    break;

  case State::kDoctypePublicIdentifierSingleQuoted:
    if (c == '\'') {
      state_ = State::kAfterDoctypePublicIdentifier;
    } else if (c == '\0') {
      doctype_public_id_ += "\xEF\xBF\xBD";
    } else if (c == '>') {
      doctype_force_quirks_ = true; // abrupt-doctype-public-identifier
      EmitDoctype();
      state_ = State::kData;
    } else {
      doctype_public_id_.push_back(c);
    }
    break;

  case State::kAfterDoctypePublicIdentifier:
    if (IsAsciiWhitespace(c)) {
      state_ = State::kBetweenDoctypePublicAndSystemIdentifiers;
    } else if (c == '>') {
      EmitDoctype();
      doctype_name_.clear();
      state_ = State::kData;
    } else if (c == '"') {
      doctype_system_set_ = true;
      doctype_system_id_.clear();
      state_ = State::kDoctypeSystemIdentifierDoubleQuoted;
    } else if (c == '\'') {
      doctype_system_set_ = true;
      doctype_system_id_.clear();
      state_ = State::kDoctypeSystemIdentifierSingleQuoted;
    } else {
      doctype_force_quirks_ = true;
      ReconsumeIn(State::kBogusDoctype);
    }
    break;

  case State::kBetweenDoctypePublicAndSystemIdentifiers:
    if (IsAsciiWhitespace(c)) {
      // stay
    } else if (c == '>') {
      EmitDoctype();
      doctype_name_.clear();
      state_ = State::kData;
    } else if (c == '"') {
      doctype_system_set_ = true;
      doctype_system_id_.clear();
      state_ = State::kDoctypeSystemIdentifierDoubleQuoted;
    } else if (c == '\'') {
      doctype_system_set_ = true;
      doctype_system_id_.clear();
      state_ = State::kDoctypeSystemIdentifierSingleQuoted;
    } else {
      doctype_force_quirks_ = true;
      ReconsumeIn(State::kBogusDoctype);
    }
    break;

  case State::kAfterDoctypeSystemKeyword:
    if (IsAsciiWhitespace(c)) {
      state_ = State::kBeforeDoctypeSystemIdentifier;
    } else if (c == '"') {
      doctype_system_set_ = true;
      doctype_system_id_.clear();
      state_ = State::kDoctypeSystemIdentifierDoubleQuoted;
    } else if (c == '\'') {
      doctype_system_set_ = true;
      doctype_system_id_.clear();
      state_ = State::kDoctypeSystemIdentifierSingleQuoted;
    } else if (c == '>') {
      doctype_force_quirks_ = true; // missing-doctype-system-identifier
      EmitDoctype();
      state_ = State::kData;
    } else {
      doctype_force_quirks_ = true;
      ReconsumeIn(State::kBogusDoctype);
    }
    break;

  case State::kBeforeDoctypeSystemIdentifier:
    if (IsAsciiWhitespace(c)) {
      // stay
    } else if (c == '"') {
      doctype_system_set_ = true;
      doctype_system_id_.clear();
      state_ = State::kDoctypeSystemIdentifierDoubleQuoted;
    } else if (c == '\'') {
      doctype_system_set_ = true;
      doctype_system_id_.clear();
      state_ = State::kDoctypeSystemIdentifierSingleQuoted;
    } else if (c == '>') {
      doctype_force_quirks_ = true; // missing-doctype-system-identifier
      EmitDoctype();
      state_ = State::kData;
    } else {
      doctype_force_quirks_ = true;
      ReconsumeIn(State::kBogusDoctype);
    }
    break;

  case State::kDoctypeSystemIdentifierDoubleQuoted:
    if (c == '"') {
      state_ = State::kAfterDoctypeSystemIdentifier;
    } else if (c == '\0') {
      doctype_system_id_ += "\xEF\xBF\xBD";
    } else if (c == '>') {
      doctype_force_quirks_ = true; // abrupt-doctype-system-identifier
      EmitDoctype();
      state_ = State::kData;
    } else {
      doctype_system_id_.push_back(c);
    }
    break;

  case State::kDoctypeSystemIdentifierSingleQuoted:
    if (c == '\'') {
      state_ = State::kAfterDoctypeSystemIdentifier;
    } else if (c == '\0') {
      doctype_system_id_ += "\xEF\xBF\xBD";
    } else if (c == '>') {
      doctype_force_quirks_ = true; // abrupt-doctype-system-identifier
      EmitDoctype();
      state_ = State::kData;
    } else {
      doctype_system_id_.push_back(c);
    }
    break;

  case State::kAfterDoctypeSystemIdentifier:
    if (IsAsciiWhitespace(c)) {
      // stay
    } else if (c == '>') {
      EmitDoctype();
      doctype_name_.clear();
      state_ = State::kData;
    } else {
      ReconsumeIn(State::kBogusDoctype);
    }
    break;

  case State::kBogusDoctype:
    if (c == '>') {
      EmitDoctype();
      doctype_name_.clear();
      state_ = State::kData;
    }
    // Anything else (including NUL) is ignored.
    break;

  case State::kPlaintext:
    if (c == '\0') {
      char_run_ += "\xEF\xBF\xBD";
    } else {
      char_run_.push_back(c);
    }
    break;

  case State::kScriptData:
    if (c == '<') {
      FlushCharacterRun();
      state_ = State::kScriptDataLessThanSign;
    } else {
      char_run_.push_back(c);
    }
    break;

  case State::kScriptDataLessThanSign:
    if (c == '/') {
      pending_text_.clear();
      pending_text_.append("</");
      end_tag_name_.clear();
      state_ = State::kScriptDataEndTagOpen;
    } else if (c == '!') {
      char_run_.append("<!");
      state_ = State::kScriptDataEscapeStart;
    } else {
      char_run_.push_back('<');
      ReconsumeIn(State::kScriptData);
    }
    break;

  case State::kScriptDataEndTagOpen:
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      pending_text_.push_back(c);
      end_tag_name_.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
      state_ = State::kScriptDataEndTagName;
    } else {
      FlushCharacterRun();
      char_run_ += pending_text_;
      pending_text_.clear();
      ReconsumeIn(State::kScriptData);
    }
    break;

  case State::kScriptDataEndTagName:
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      pending_text_.push_back(c);
      end_tag_name_.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
    } else if (c == '>') {
      if (end_tag_name_ == raw_text_tag_) {
        Emit(Token::MakeEndTag(std::move(end_tag_name_)));
        pending_text_.clear();
        state_ = State::kData;
      } else {
        FlushCharacterRun();
        char_run_ += pending_text_;
        char_run_.push_back('>');
        pending_text_.clear();
        end_tag_name_.clear();
        state_ = State::kScriptData;
      }
    } else if (IsAsciiWhitespace(c) || c == '/') {
      if (end_tag_name_ == raw_text_tag_) {
        Emit(Token::MakeEndTag(std::move(end_tag_name_)));
        pending_text_.clear();
        state_ = State::kData;
      } else {
        FlushCharacterRun();
        char_run_ += pending_text_;
        pending_text_.clear();
        end_tag_name_.clear();
        ReconsumeIn(State::kScriptData);
      }
    } else {
      FlushCharacterRun();
      char_run_ += pending_text_;
      char_run_.push_back(c);
      pending_text_.clear();
      end_tag_name_.clear();
      state_ = State::kScriptData;
    }
    break;

  case State::kScriptDataEscapeStart:
    if (c == '-') {
      char_run_.push_back('-');
      state_ = State::kScriptDataEscapeStartDash;
    } else {
      ReconsumeIn(State::kScriptData);
    }
    break;

  case State::kScriptDataEscapeStartDash:
    if (c == '-') {
      char_run_.push_back('-');
      state_ = State::kScriptDataEscapedDashDash;
    } else {
      ReconsumeIn(State::kScriptData);
    }
    break;

  case State::kScriptDataEscaped:
    if (c == '-') {
      char_run_.push_back('-');
      state_ = State::kScriptDataEscapedDash;
    } else if (c == '<') {
      state_ = State::kScriptDataEscapedLessThanSign;
    } else {
      char_run_.push_back(c);
    }
    break;

  case State::kScriptDataEscapedDash:
    if (c == '-') {
      char_run_.push_back('-');
      state_ = State::kScriptDataEscapedDashDash;
    } else if (c == '<') {
      state_ = State::kScriptDataEscapedLessThanSign;
    } else {
      char_run_.push_back(c);
      state_ = State::kScriptDataEscaped;
    }
    break;

  case State::kScriptDataEscapedDashDash:
    if (c == '-') {
      char_run_.push_back('-');
    } else if (c == '<') {
      state_ = State::kScriptDataEscapedLessThanSign;
    } else if (c == '>') {
      char_run_.push_back('>');
      state_ = State::kScriptData;
    } else {
      char_run_.push_back(c);
      state_ = State::kScriptDataEscaped;
    }
    break;

  case State::kScriptDataEscapedLessThanSign:
    if (c == '/') {
      pending_text_.clear();
      pending_text_.append("</");
      end_tag_name_.clear();
      state_ = State::kScriptDataEscapedEndTagOpen;
    } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      temp_buffer_.clear();
      char_run_.push_back('<');
      ReconsumeIn(State::kScriptDataDoubleEscapeStart);
    } else {
      char_run_.push_back('<');
      ReconsumeIn(State::kScriptDataEscaped);
    }
    break;

  case State::kScriptDataEscapedEndTagOpen:
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      pending_text_.push_back(c);
      end_tag_name_.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
      state_ = State::kScriptDataEscapedEndTagName;
    } else {
      FlushCharacterRun();
      char_run_ += pending_text_;
      pending_text_.clear();
      ReconsumeIn(State::kScriptDataEscaped);
    }
    break;

  case State::kScriptDataEscapedEndTagName:
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      pending_text_.push_back(c);
      end_tag_name_.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
    } else if (c == '>') {
      if (end_tag_name_ == raw_text_tag_) {
        Emit(Token::MakeEndTag(std::move(end_tag_name_)));
        pending_text_.clear();
        state_ = State::kData;
      } else {
        FlushCharacterRun();
        char_run_ += pending_text_;
        char_run_.push_back('>');
        pending_text_.clear();
        end_tag_name_.clear();
        state_ = State::kScriptDataEscaped;
      }
    } else if (IsAsciiWhitespace(c) || c == '/') {
      if (end_tag_name_ == raw_text_tag_) {
        Emit(Token::MakeEndTag(std::move(end_tag_name_)));
        pending_text_.clear();
        state_ = State::kData;
      } else {
        FlushCharacterRun();
        char_run_ += pending_text_;
        pending_text_.clear();
        end_tag_name_.clear();
        ReconsumeIn(State::kScriptDataEscaped);
      }
    } else {
      FlushCharacterRun();
      char_run_ += pending_text_;
      char_run_.push_back(c);
      pending_text_.clear();
      end_tag_name_.clear();
      state_ = State::kScriptDataEscaped;
    }
    break;

  case State::kScriptDataDoubleEscapeStart:
    if (IsAsciiWhitespace(c) || c == '/' || c == '>') {
      char_run_.push_back(c);
      state_ =
          (temp_buffer_ == "script") ? State::kScriptDataDoubleEscaped : State::kScriptDataEscaped;
    } else if (c >= 'A' && c <= 'Z') {
      temp_buffer_.push_back(static_cast<char>(c + 32));
      char_run_.push_back(c);
    } else if (c >= 'a' && c <= 'z') {
      temp_buffer_.push_back(c);
      char_run_.push_back(c);
    } else {
      ReconsumeIn(State::kScriptDataEscaped);
    }
    break;

  case State::kScriptDataDoubleEscaped:
    if (c == '-') {
      char_run_.push_back('-');
      state_ = State::kScriptDataDoubleEscapedDash;
    } else if (c == '<') {
      char_run_.push_back('<');
      state_ = State::kScriptDataDoubleEscapedLessThanSign;
    } else {
      char_run_.push_back(c);
    }
    break;

  case State::kScriptDataDoubleEscapedDash:
    if (c == '-') {
      char_run_.push_back('-');
      state_ = State::kScriptDataDoubleEscapedDashDash;
    } else if (c == '<') {
      char_run_.push_back('<');
      state_ = State::kScriptDataDoubleEscapedLessThanSign;
    } else {
      char_run_.push_back(c);
      state_ = State::kScriptDataDoubleEscaped;
    }
    break;

  case State::kScriptDataDoubleEscapedDashDash:
    if (c == '-') {
      char_run_.push_back('-');
    } else if (c == '<') {
      char_run_.push_back('<');
      state_ = State::kScriptDataDoubleEscapedLessThanSign;
    } else if (c == '>') {
      char_run_.push_back('>');
      state_ = State::kScriptData;
    } else {
      char_run_.push_back(c);
      state_ = State::kScriptDataDoubleEscaped;
    }
    break;

  case State::kScriptDataDoubleEscapedLessThanSign:
    if (c == '/') {
      temp_buffer_.clear();
      char_run_.push_back('/');
      state_ = State::kScriptDataDoubleEscapeEnd;
    } else {
      ReconsumeIn(State::kScriptDataDoubleEscaped);
    }
    break;

  case State::kScriptDataDoubleEscapeEnd:
    if (IsAsciiWhitespace(c) || c == '/' || c == '>') {
      char_run_.push_back(c);
      state_ =
          (temp_buffer_ == "script") ? State::kScriptDataEscaped : State::kScriptDataDoubleEscaped;
    } else if (c >= 'A' && c <= 'Z') {
      temp_buffer_.push_back(static_cast<char>(c + 32));
      char_run_.push_back(c);
    } else if (c >= 'a' && c <= 'z') {
      temp_buffer_.push_back(c);
      char_run_.push_back(c);
    } else {
      ReconsumeIn(State::kScriptDataDoubleEscaped);
    }
    break;

  case State::kRcdata:
    if (c == '<') {
      FlushCharacterRun();
      state_ = State::kRcdataLessThanSign;
    } else if (c == '&') {
      char_run_ += ConsumeCharacterReference(/*in_attribute=*/false);
    } else {
      char_run_.push_back(c);
    }
    break;

  case State::kRcdataLessThanSign:
    if (c == '/') {
      pending_text_.clear();
      pending_text_.push_back('<');
      pending_text_.push_back('/');
      end_tag_name_.clear();
      state_ = State::kRcdataEndTagOpen;
    } else {
      FlushCharacterRun();
      char_run_.push_back('<');
      ReconsumeIn(State::kRcdata);
    }
    break;

  case State::kRcdataEndTagOpen:
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      pending_text_.push_back(c);
      end_tag_name_.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
      state_ = State::kRcdataEndTagName;
    } else {
      FlushCharacterRun();
      char_run_ += pending_text_;
      pending_text_.clear();
      ReconsumeIn(State::kRcdata);
    }
    break;

  case State::kRcdataEndTagName:
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      pending_text_.push_back(c);
      end_tag_name_.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
    } else if (c == '>' || IsWhitespace(c) || c == '/') {
      // A matched end tag name is closed by '>', or by whitespace or '/'
      // (attributes / self-closing slash follow; the parser consumes the
      // tag).  An unmatched name is treated as text.
      if (end_tag_name_ == raw_text_tag_) {
        Emit(Token::MakeEndTag(std::move(end_tag_name_)));
        pending_text_.clear();
        state_ = State::kData;
      } else {
        FlushCharacterRun();
        char_run_ += pending_text_;
        char_run_.push_back(c);
        pending_text_.clear();
        state_ = State::kRcdata;
      }
    } else {
      FlushCharacterRun();
      char_run_ += pending_text_;
      char_run_.push_back(c);
      pending_text_.clear();
      state_ = State::kRcdata;
    }
    break;

  case State::kRawtext:
    if (c == '<') {
      FlushCharacterRun();
      state_ = State::kRawtextLessThanSign;
    } else {
      char_run_.push_back(c);
    }
    break;

  case State::kRawtextLessThanSign:
    if (c == '/') {
      pending_text_.clear();
      pending_text_.push_back('<');
      pending_text_.push_back('/');
      end_tag_name_.clear();
      state_ = State::kRawtextEndTagOpen;
    } else {
      FlushCharacterRun();
      char_run_.push_back('<');
      ReconsumeIn(State::kRawtext);
    }
    break;

  case State::kRawtextEndTagOpen:
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      pending_text_.push_back(c);
      end_tag_name_.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
      state_ = State::kRawtextEndTagName;
    } else {
      FlushCharacterRun();
      char_run_ += pending_text_;
      pending_text_.clear();
      ReconsumeIn(State::kRawtext);
    }
    break;

  case State::kRawtextEndTagName:
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      pending_text_.push_back(c);
      end_tag_name_.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
    } else if (c == '>' || IsWhitespace(c) || c == '/') {
      // A matched end tag name is closed by '>', or by whitespace or '/'
      // (attributes / self-closing slash follow).  Unmatched names are text.
      if (end_tag_name_ == raw_text_tag_) {
        Emit(Token::MakeEndTag(std::move(end_tag_name_)));
        pending_text_.clear();
        state_ = State::kData;
      } else {
        FlushCharacterRun();
        char_run_ += pending_text_;
        char_run_.push_back(c);
        pending_text_.clear();
        state_ = State::kRawtext;
      }
    } else {
      FlushCharacterRun();
      char_run_ += pending_text_;
      char_run_.push_back(c);
      pending_text_.clear();
      state_ = State::kRawtext;
    }
    break;
  }
}

} // namespace neko::html
