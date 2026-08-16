#include "neko/html/token.h"

#include <string>
#include <string_view>

namespace neko::html {

Token Token::MakeDoctype(std::string name, bool force_quirks)
{
  Token token;
  token.type = TokenType::kDoctype;
  token.name = std::move(name);
  token.force_quirks = force_quirks;
  return token;
}

Token Token::MakeStartTag(std::string name, std::vector<Attribute> attributes, bool self_closing)
{
  Token token;
  token.type = TokenType::kStartTag;
  token.name = std::move(name);
  token.attributes = std::move(attributes);
  token.self_closing = self_closing;
  return token;
}

Token Token::MakeEndTag(std::string name)
{
  Token token;
  token.type = TokenType::kEndTag;
  token.name = std::move(name);
  return token;
}

Token Token::MakeComment(std::string data)
{
  Token token;
  token.type = TokenType::kComment;
  token.data = std::move(data);
  return token;
}

Token Token::MakeCharacter(std::string data)
{
  Token token;
  token.type = TokenType::kCharacter;
  token.data = std::move(data);
  return token;
}

Token Token::MakeEOF()
{
  Token token;
  token.type = TokenType::kEOF;
  return token;
}

bool IsAsciiWhitespace(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool IsAsciiAlnum(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

std::string ToLowerAscii(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
  }
  return out;
}

} // namespace neko::html
