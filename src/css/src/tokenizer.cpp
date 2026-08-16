#include "neko/css/tokenizer.h"

#include <cstdlib>
#include <string>

namespace neko::css {
namespace {

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

  auto consume_name = [&]() {
    const std::size_t start = pos_;
    while (pos_ < input_.size() && IsNameChar(input_[pos_])) {
      ++pos_;
    }
    return std::string(input_.substr(start, pos_ - start));
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
      if (ch == '\\' && pos_ < input_.size()) {
        const char next = input_[pos_++];
        if (next != '\n') {
          token.text.push_back(next);
        }
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
