#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace neko::css {

enum class CssTokenType
{
  kIdent,
  kAtKeyword,
  kHash,
  kNumber,
  kPercentage,
  kDimension,
  kString,
  kDelim,
  kColon,
  kSemicolon,
  kComma,
  kOpenBracket,
  kCloseBracket,
  kOpenParen,
  kCloseParen,
  kOpenBrace,
  kCloseBrace,
  kWhitespace,
  kEOF,
};

struct CssToken
{
  CssTokenType type = CssTokenType::kEOF;
  std::string text;  // identifier/hash/string/delim text, or the numeric part
  std::string unit;  // dimension unit
  double number = 0; // numeric value for kNumber/kPercentage/kDimension
};

// CSS tokenizer (Phase 4 scope).  Comments are skipped; whitespace is emitted
// as kWhitespace tokens.  See docs/css/README.md for scope notes.
class Tokenizer
{
public:
  explicit Tokenizer(std::string_view input) : input_(input) {}

  std::vector<CssToken> Tokenize();

private:
  std::string_view input_;
  std::size_t pos_ = 0;

  // Skips /* */ comments only; whitespace is emitted as kWhitespace tokens so
  // the parser can distinguish value components and descendant combinators.
  void SkipComments();
  CssToken NextToken();
};

} // namespace neko::css
