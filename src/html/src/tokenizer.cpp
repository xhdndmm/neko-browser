#include "neko/html/tokenizer.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "neko/base/utf8.h"

namespace neko::html {
namespace {

struct NamedEntity {
  std::string_view name;
  char32_t code_point;
};

// Common named character references.  This is a practical subset of the
// WHATWG table (~2200 entries); see docs/html/README.md for the scope.
constexpr NamedEntity kNamedEntities[] = {
    {"AElig", 0x00C6},   {"Aacute", 0x00C1},  {"Acirc", 0x00C2},  {"Agrave", 0x00C0},
    {"Aring", 0x00C5},   {"Atilde", 0x00C3},  {"Auml", 0x00C4},   {"Ccedil", 0x00C7},
    {"ETH", 0x00D0},     {"Eacute", 0x00C9},  {"Ecirc", 0x00CA},  {"Egrave", 0x00C8},
    {"Euml", 0x00CB},    {"Iacute", 0x00CD},  {"Icirc", 0x00CE},  {"Igrave", 0x00CC},
    {"Iuml", 0x00CF},    {"Ntilde", 0x00D1}, {"Oacute", 0x00D3}, {"Ocirc", 0x00D4},
    {"Ograve", 0x00D2},  {"Oslash", 0x00D8}, {"Otilde", 0x00D5}, {"Ouml", 0x00D6},
    {"THORN", 0x00DE},   {"Uacute", 0x00DA}, {"Ucirc", 0x00DB},  {"Ugrave", 0x00D9},
    {"Uuml", 0x00DC},    {"Yacute", 0x00DD}, {"aacute", 0x00E1}, {"acirc", 0x00E2},
    {"acute", 0x00B4},   {"aelig", 0x00E6},  {"agrave", 0x00E0}, {"amp", 0x0026},
    {"aring", 0x00E5},   {"atilde", 0x00E3}, {"auml", 0x00E4},   {"bdquo", 0x201E},
    {"brvbar", 0x00A6},  {"bull", 0x2022},   {"ccedil", 0x00E7}, {"cedil", 0x00B8},
    {"cent", 0x00A2},    {"copy", 0x00A9},   {"curren", 0x00A4}, {"deg", 0x00B0},
    {"divide", 0x00F7},  {"eacute", 0x00E9}, {"ecirc", 0x00EA},  {"egrave", 0x00E8},
    {"eth", 0x00F0},     {"euml", 0x00EB},   {"frac12", 0x00BD}, {"frac14", 0x00BC},
    {"frac34", 0x00BE},  {"gt", 0x003E},     {"hellip", 0x2026}, {"iacute", 0x00ED},
    {"icirc", 0x00EE},   {"iexcl", 0x00A1},  {"igrave", 0x00EC}, {"iquest", 0x00BF},
    {"iuml", 0x00EF},    {"laquo", 0x00AB},  {"ldquo", 0x201C},  {"lsquo", 0x2018},
    {"lt", 0x003C},      {"macr", 0x00AF},   {"mdash", 0x2014},  {"micro", 0x00B5},
    {"middot", 0x00B7},  {"nbsp", 0x00A0},   {"ndash", 0x2013},  {"not", 0x00AC},
    {"ntilde", 0x00F1},  {"oacute", 0x00F3}, {"ocirc", 0x00F4},  {"ograve", 0x00F2},
    {"ordf", 0x00AA},    {"ordm", 0x00BA},   {"oslash", 0x00F8}, {"otilde", 0x00F5},
    {"ouml", 0x00F6},    {"para", 0x00B6},   {"plusmn", 0x00B1}, {"pound", 0x00A3},
    {"quot", 0x0022},    {"raquo", 0x00BB},  {"rdquo", 0x201D},  {"reg", 0x00AE},
    {"rsquo", 0x2019},   {"sect", 0x00A7},   {"shy", 0x00AD},    {"sup1", 0x00B9},
    {"sup2", 0x00B2},    {"sup3", 0x00B3},   {"szlig", 0x00DF},  {"thorn", 0x00FE},
    {"times", 0x00D7},   {"trade", 0x2122},  {"uacute", 0x00FA}, {"ucirc", 0x00FB},
    {"ugrave", 0x00F9},  {"uml", 0x00A8},    {"uuml", 0x00FC},   {"yacute", 0x00FD},
    {"yen", 0x00A5},     {"yuml", 0x00FF},
};

// Entities that may omit the trailing semicolon (WHATWG legacy set, subset).
bool IsLegacyNoSemicolon(std::string_view name) {
  return name == "amp" || name == "lt" || name == "gt" || name == "quot" || name == "apos" ||
         name == "nbsp" || name == "copy" || name == "reg";
}

const char32_t* LookupNamedEntity(std::string_view name) {
  for (const NamedEntity& entry : kNamedEntities) {
    if (entry.name == name) {
      return &entry.code_point;
    }
  }
  return nullptr;
}

int HexDigitValue(char c) {
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

}  // namespace

Tokenizer::Tokenizer(std::string_view input) : input_(input) {}

void Tokenizer::StartRawText(std::string_view tag_name) {
  raw_text_tag_ = std::string(tag_name);
  if (tag_name == "title" || tag_name == "textarea") {
    state_ = State::kRcdata;
  } else if (tag_name == "script") {
    state_ = State::kScriptData;
  } else {
    state_ = State::kRawtext;
  }
}

std::string_view Tokenizer::Peek(std::size_t offset) const {
  return (pos_ + offset < input_.size()) ? input_.substr(pos_ + offset) : std::string_view{};
}

void Tokenizer::Skip(std::size_t count) { pos_ += count; }

bool Tokenizer::MatchIgnoreCase(std::string_view text) const {
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

void Tokenizer::ReconsumeIn(State state) {
  state_ = state;
  reconsume_ = true;
}

void Tokenizer::Emit(Token token) { out_.push_back(std::move(token)); }

void Tokenizer::FlushCharacterRun() {
  if (char_run_.empty()) {
    return;
  }
  Emit(Token::MakeCharacter(std::move(char_run_)));
  char_run_.clear();
}

std::string Tokenizer::ConsumeCharacterReference() {
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
  const std::size_t start = pos_;
  while (pos_ < input_.size() && IsAsciiAlnum(input_[pos_])) {
    ++pos_;
  }
  const std::string_view scanned = input_.substr(start, pos_ - start);
  for (std::size_t len = scanned.size(); len > 0; --len) {
    const std::string_view candidate = scanned.substr(0, len);
    const char32_t* code_point = LookupNamedEntity(candidate);
    if (code_point == nullptr) {
      continue;
    }
    const bool followed_by_semicolon = pos_ < input_.size() && input_[pos_] == ';';
    const bool followed_by_alnum = pos_ < input_.size() && IsAsciiAlnum(input_[pos_]);
    if (followed_by_semicolon) {
      ++pos_;
      return base::EncodeUtf8(*code_point);
    }
    if (IsLegacyNoSemicolon(candidate) && !followed_by_alnum) {
      return base::EncodeUtf8(*code_point);
    }
    // Matched a name but it is not a valid reference (no ';', not legacy, or
    // followed by more name characters).  Rewind and emit a literal '&'.
    pos_ = start;
    return "&";
  }
  pos_ = start;
  return "&";
}

void Tokenizer::ProcessEof() {
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
      // Emit whatever tag we have (start or end) then move on.
      if (!tag_name_.empty() || state_ == State::kTagName) {
        // Start-tag unless we are in an end-tag context; for milestone we
        // always emit a start tag here.
        Emit(Token::MakeStartTag(std::move(tag_name_), std::move(attributes_), self_closing_));
      }
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
      Emit(Token::MakeDoctype(std::move(doctype_name_), doctype_name_ != "html"));
      break;
    default:
      break;
  }
  // Ensure a single EOF token afterwards.
  state_ = State::kData;
}

Token Tokenizer::Next() {
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

void Tokenizer::ProcessChar(char c) {
  switch (state_) {
    case State::kData:
      if (c == '<') {
        FlushCharacterRun();
        state_ = State::kTagOpen;
      } else if (c == '&') {
        char_run_ += ConsumeCharacterReference();
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
      } else if (c == '>' || c == '/' || c == '<' || c == '=') {
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
      } else if (c == '<' || c == '"' || c == '\'' || c == '=' ) {
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
        attribute_value_ += ConsumeCharacterReference();
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
        attribute_value_ += ConsumeCharacterReference();
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
        Skip(1);  // the second '-'
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
        Emit(Token::MakeDoctype(std::string(), /*force_quirks=*/true));
        state_ = State::kData;
      } else {
        doctype_name_.clear();
        doctype_name_.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
        state_ = State::kDoctypeName;
      }
      break;

    case State::kDoctypeName:
      if (IsAsciiWhitespace(c)) {
        state_ = State::kAfterDoctypeName;
      } else if (c == '>') {
        Emit(Token::MakeDoctype(std::move(doctype_name_), doctype_name_ != "html"));
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
        Emit(Token::MakeDoctype(std::move(doctype_name_), doctype_name_ != "html"));
        doctype_name_.clear();
        state_ = State::kData;
      } else {
        // Public/system identifiers are not parsed yet; skip to '>'.
        state_ = State::kAfterDoctypeName;
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
        state_ = (temp_buffer_ == "script") ? State::kScriptDataDoubleEscaped
                                            : State::kScriptDataEscaped;
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
        state_ = (temp_buffer_ == "script") ? State::kScriptDataEscaped
                                            : State::kScriptDataDoubleEscaped;
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
        char_run_ += ConsumeCharacterReference();
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

}  // namespace neko::html
