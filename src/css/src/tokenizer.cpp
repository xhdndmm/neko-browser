#include "neko/css/tokenizer.h"

#include "neko/base/utf8.h"

#include <cstdlib>
#include <string>

namespace neko::css {
namespace {

bool IsHexDigit(char c)
{
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool IsDigit(char c)
{
  return c >= '0' && c <= '9';
}

bool IsNameStart(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
         static_cast<unsigned char>(c) >= 0x80;
}

bool IsNameChar(char c)
{
  return IsNameStart(c) || IsDigit(c) || c == '-';
}

bool IsWhitespace(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

} // namespace

std::vector<CssToken> Tokenizer::Tokenize()
{
  std::vector<CssToken> tokens;
  for (;;) {
    SkipComments();
    CssToken token = NextToken();
    tokens.push_back(std::move(token));
    if (tokens.back().type == CssTokenType::kEOF) {
      break;
    }
  }
  return tokens;
}

// Skips /* */ comments.  Whitespace is NOT skipped here: NextToken emits it
// as kWhitespace tokens so value components and descendant combinators stay
// distinguishable.
void Tokenizer::SkipComments()
{
  while (pos_ + 1 < input_.size() && input_[pos_] == '/' && input_[pos_ + 1] == '*') {
    pos_ += 2;
    while (pos_ + 1 < input_.size() && !(input_[pos_] == '*' && input_[pos_ + 1] == '/')) {
      ++pos_;
    }
    pos_ += 2;
  }
}

CssToken Tokenizer::NextToken()
{
  CssToken token;
  if (pos_ >= input_.size()) {
    token.type = CssTokenType::kEOF;
    return token;
  }
  const char c = input_[pos_];

  if (IsWhitespace(c)) {
    token.type = CssTokenType::kWhitespace;
    while (pos_ < input_.size() && IsWhitespace(input_[pos_])) {
      ++pos_;
    }
    return token;
  }

  // CSS Syntax §4.3.3 "consume an escaped code point": entered with pos_
  // just past the backslash.  \ + 1-6 hex digits (plus one optional
  // trailing whitespace) is that code point; \ + anything else is the
  // character itself; a trailing backslash is U+FFFD.
  auto consume_escape = [&]() -> std::string {
    if (pos_ >= input_.size()) {
      return base::EncodeUtf8(0xFFFD);
    }
    if (IsHexDigit(input_[pos_])) {
      std::string hex;
      while (pos_ < input_.size() && hex.size() < 6 && IsHexDigit(input_[pos_])) {
        hex.push_back(input_[pos_++]);
      }
      if (pos_ < input_.size() && IsWhitespace(input_[pos_])) {
        ++pos_; // the whitespace is part of the escape, not the name
      }
      const char32_t code_point =
          hex.empty() ? 0 : static_cast<char32_t>(std::strtoul(hex.c_str(), nullptr, 16));
      // Code point 0 and invalid values decode to U+FFFD (§4.3.3).
      return base::EncodeUtf8(code_point == 0 ? 0xFFFD : code_point);
    }
    return std::string(1, input_[pos_++]);
  };

  auto consume_name = [&]() {
    // Ident sequences include escapes (§4.3.4): a valid escape contributes
    // its decoded characters; a backslash before a newline (or at EOF) is an
    // invalid escape and ends the name.
    std::string out;
    while (pos_ < input_.size()) {
      const char ch = input_[pos_];
      if (ch == '\\') {
        if (pos_ + 1 >= input_.size() || input_[pos_ + 1] == '\n') {
          break;
        }
        ++pos_; // step past the backslash
        out += consume_escape();
        continue;
      }
      if (!IsNameChar(ch)) {
        break;
      }
      out.push_back(ch);
      ++pos_;
    }
    return out;
  };

  auto consume_number = [&]() -> CssToken {
    CssToken t;
    const std::size_t start = pos_;
    if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
      ++pos_;
    }
    while (pos_ < input_.size() && IsDigit(input_[pos_])) {
      ++pos_;
    }
    if (pos_ < input_.size() && input_[pos_] == '.') {
      ++pos_;
      while (pos_ < input_.size() && IsDigit(input_[pos_])) {
        ++pos_;
      }
    }
    if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
      const std::size_t save = pos_++;
      if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
        ++pos_;
      }
      if (pos_ < input_.size() && IsDigit(input_[pos_])) {
        while (pos_ < input_.size() && IsDigit(input_[pos_])) {
          ++pos_;
        }
      } else {
        pos_ = save;
      }
    }
    t.text = std::string(input_.substr(start, pos_ - start));
    t.number = std::strtod(t.text.c_str(), nullptr);
    t.type = CssTokenType::kNumber;
    if (pos_ < input_.size() && input_[pos_] == '%') {
      ++pos_;
      t.type = CssTokenType::kPercentage;
    } else if (pos_ < input_.size() && IsNameStart(input_[pos_])) {
      t.unit = consume_name();
      t.type = CssTokenType::kDimension;
    }
    return t;
  };

  // Numbers: digit, '.'digit, or sign followed by digit/'.'digit.
  if (IsDigit(c) || (c == '.' && pos_ + 1 < input_.size() && IsDigit(input_[pos_ + 1])) ||
      ((c == '+' || c == '-') && pos_ + 1 < input_.size() &&
       (IsDigit(input_[pos_ + 1]) ||
        (input_[pos_ + 1] == '.' && pos_ + 2 < input_.size() && IsDigit(input_[pos_ + 2]))))) {
    return consume_number();
  }

  if (c == '@') {
    ++pos_;
    token.type = CssTokenType::kAtKeyword;
    token.text = consume_name();
    return token;
  }
  if (c == '#') {
    ++pos_;
    token.type = CssTokenType::kHash;
    token.text = consume_name();
    return token;
  }
  if (c == '"' || c == '\'') {
    const char quote = c;
    ++pos_;
    token.type = CssTokenType::kString;
    while (pos_ < input_.size()) {
      const char ch = input_[pos_++];
      if (ch == quote) {
        return token;
      }
      if (ch == '\\') {
        if (pos_ < input_.size() && input_[pos_] == '\n') {
          ++pos_; // line continuation produces nothing
          continue;
        }
        token.text += consume_escape();
        continue;
      }
      token.text.push_back(ch);
    }
    return token;
  }
  if (IsNameStart(c) || c == '-') {
    token.type = CssTokenType::kIdent;
    token.text = consume_name();
    return token;
  }

  ++pos_;
  switch (c) {
  case ':':
    token.type = CssTokenType::kColon;
    break;
  case ';':
    token.type = CssTokenType::kSemicolon;
    break;
  case ',':
    token.type = CssTokenType::kComma;
    break;
  case '[':
    token.type = CssTokenType::kOpenBracket;
    break;
  case ']':
    token.type = CssTokenType::kCloseBracket;
    break;
  case '(':
    token.type = CssTokenType::kOpenParen;
    break;
  case ')':
    token.type = CssTokenType::kCloseParen;
    break;
  case '{':
    token.type = CssTokenType::kOpenBrace;
    break;
  case '}':
    token.type = CssTokenType::kCloseBrace;
    break;
  default:
    token.type = CssTokenType::kDelim;
    token.text.push_back(c);
    break;
  }
  return token;
}

} // namespace neko::css
