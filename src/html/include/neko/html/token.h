#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace neko::html {

enum class TokenType
{
  kDoctype,
  kStartTag,
  kEndTag,
  kComment,
  kCharacter,
  kEOF,
};

struct Attribute
{
  std::string name; // lowercased
  std::string value;
};

// A single token produced by the tokenizer.
//
// Plain struct with public fields: the tokenizer/parser pair is the only
// consumer, so accessor boilerplate is deliberately avoided.
struct Token
{
  TokenType type = TokenType::kEOF;

  // Start/end tag and doctype name (lowercased for tags).
  std::string name;
  std::vector<Attribute> attributes;
  bool self_closing = false;

  // Character run or comment data (UTF-8).
  std::string data;

  // Doctype fields.
  std::string public_id;
  std::string system_id;
  bool force_quirks = false;

  static Token MakeDoctype(std::string name, bool force_quirks);
  static Token MakeStartTag(std::string name, std::vector<Attribute> attributes, bool self_closing);
  static Token MakeEndTag(std::string name);
  static Token MakeComment(std::string data);
  static Token MakeCharacter(std::string data);
  static Token MakeEOF();
};

// True when |c| is an ASCII whitespace character (WHATWG).
bool IsAsciiWhitespace(char c);

// True when |c| is an ASCII letter or digit.
bool IsAsciiAlnum(char c);

// Lowercases an ASCII string.
std::string ToLowerAscii(std::string_view s);

} // namespace neko::html
