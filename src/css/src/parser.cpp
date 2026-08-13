#include "neko/css/parser.h"

#include <string>
#include <string_view>
#include <vector>

#include "neko/css/tokenizer.h"

namespace neko::css {
namespace {

struct TokenStream {
  explicit TokenStream(const std::vector<CssToken>& input) : tokens(input) {}

  const std::vector<CssToken>& tokens;
  std::size_t i = 0;

  bool AtEnd() const { return i >= tokens.size() || tokens[i].type == CssTokenType::kEOF; }
  const CssToken& Peek() const { return tokens[i]; }
  const CssToken& Next() { return tokens[i++]; }
  void SkipWhitespace() {
    while (!AtEnd() && Peek().type == CssTokenType::kWhitespace) {
      ++i;
    }
  }
};

std::string_view TrimView(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' ||
                           text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' ||
                           text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

void AppendTokenText(const CssToken& token, std::string& out) {
  switch (token.type) {
    case CssTokenType::kHash:
      out.push_back('#');
      out += token.text;
      break;
    case CssTokenType::kDimension:
      out += token.text;
      out += token.unit;
      break;
    case CssTokenType::kPercentage:
      out += token.text;
      out.push_back('%');
      break;
    case CssTokenType::kAtKeyword:
      out.push_back('@');
      out += token.text;
      break;
    case CssTokenType::kComma:
      out.push_back(',');
      break;
    case CssTokenType::kColon:
      out.push_back(':');
      break;
    case CssTokenType::kSemicolon:
      out.push_back(';');
      break;
    case CssTokenType::kOpenBracket:
      out.push_back('[');
      break;
    case CssTokenType::kCloseBracket:
      out.push_back(']');
      break;
    case CssTokenType::kOpenParen:
      out.push_back('(');
      break;
    case CssTokenType::kCloseParen:
      out.push_back(')');
      break;
    case CssTokenType::kWhitespace:
      if (!out.empty() && out.back() != ' ') {
        out.push_back(' ');
      }
      break;
    default:
      out += token.text;
      break;
  }
}

// Collects tokens until ';' or '}' or EOF, reconstructing the raw value text.
std::string CollectValueText(TokenStream& stream, bool& important) {
  std::string out;
  important = false;
  while (!stream.AtEnd() && stream.Peek().type != CssTokenType::kSemicolon &&
         stream.Peek().type != CssTokenType::kCloseBrace) {
    const CssToken& token = stream.Next();
    if (token.type == CssTokenType::kDelim && token.text == "!") {
      stream.SkipWhitespace();
      if (!stream.AtEnd() && stream.Peek().type == CssTokenType::kIdent &&
          stream.Peek().text == "important") {
        stream.Next();
        important = true;
      }
      continue;
    }
    AppendTokenText(token, out);
  }
  return std::string(TrimView(out));
}

// Parses declarations until '}' or EOF; consumes a trailing '}' if present.
std::vector<Declaration> ParseDeclarationBody(TokenStream& stream) {
  std::vector<Declaration> declarations;
  for (;;) {
    stream.SkipWhitespace();
    if (stream.AtEnd() || stream.Peek().type == CssTokenType::kCloseBrace) {
      break;
    }
    if (stream.Peek().type != CssTokenType::kIdent) {
      // Skip an unrecognized construct up to the next ';' or '}'.
      while (!stream.AtEnd() && stream.Peek().type != CssTokenType::kSemicolon &&
             stream.Peek().type != CssTokenType::kCloseBrace) {
        stream.Next();
      }
    } else {
      Declaration declaration;
      declaration.property = stream.Next().text;
      stream.SkipWhitespace();
      if (!stream.AtEnd() && stream.Peek().type == CssTokenType::kColon) {
        stream.Next();
        declaration.value = CollectValueText(stream, declaration.important);
        declarations.push_back(std::move(declaration));
      }
    }
    if (!stream.AtEnd() && stream.Peek().type == CssTokenType::kSemicolon) {
      stream.Next();
    }
  }
  if (!stream.AtEnd() && stream.Peek().type == CssTokenType::kCloseBrace) {
    stream.Next();
  }
  return declarations;
}

std::string CollectPrelude(TokenStream& stream) {
  std::string out;
  while (!stream.AtEnd() && stream.Peek().type != CssTokenType::kOpenBrace &&
         stream.Peek().type != CssTokenType::kSemicolon) {
    AppendTokenText(stream.Next(), out);
  }
  return std::string(TrimView(out));
}

void SkipNestedBlock(TokenStream& stream) {
  int depth = 0;
  while (!stream.AtEnd()) {
    const CssTokenType type = stream.Next().type;
    if (type == CssTokenType::kOpenBrace) {
      ++depth;
    } else if (type == CssTokenType::kCloseBrace) {
      if (--depth <= 0) {
        break;
      }
    }
  }
}

StyleRule ParseQualifiedRule(TokenStream& stream) {
  StyleRule rule;
  const std::string prelude = CollectPrelude(stream);
  rule.selectors = ParseSelectorList(prelude);
  if (!stream.AtEnd() && stream.Peek().type == CssTokenType::kOpenBrace) {
    stream.Next();
    rule.declarations = ParseDeclarationBody(stream);
  }
  return rule;
}

AtRule ParseAtRule(TokenStream& stream) {
  AtRule at_rule;
  at_rule.name = stream.Next().text;  // the at-keyword
  const std::string prelude = CollectPrelude(stream);
  at_rule.prelude = prelude;
  if (!stream.AtEnd() && stream.Peek().type == CssTokenType::kSemicolon) {
    stream.Next();
    return at_rule;
  }
  if (!stream.AtEnd() && stream.Peek().type == CssTokenType::kOpenBrace) {
    stream.Next();
    for (;;) {
      stream.SkipWhitespace();
      if (stream.AtEnd() || stream.Peek().type == CssTokenType::kCloseBrace) {
        break;
      }
      if (stream.Peek().type == CssTokenType::kAtKeyword) {
        SkipNestedBlock(stream);
      } else {
        at_rule.rules.push_back(ParseQualifiedRule(stream));
      }
    }
    if (!stream.AtEnd() && stream.Peek().type == CssTokenType::kCloseBrace) {
      stream.Next();
    }
  }
  return at_rule;
}

}  // namespace

StyleSheet ParseStyleSheet(std::string_view text) {
  Tokenizer tokenizer(text);
  const std::vector<CssToken> tokens = tokenizer.Tokenize();
  TokenStream stream(tokens);

  StyleSheet sheet;
  for (;;) {
    stream.SkipWhitespace();
    if (stream.AtEnd()) {
      break;
    }
    if (stream.Peek().type == CssTokenType::kAtKeyword) {
      sheet.at_rules.push_back(ParseAtRule(stream));
    } else {
      sheet.rules.push_back(ParseQualifiedRule(stream));
    }
  }
  return sheet;
}

std::vector<Declaration> ParseDeclarationBlock(std::string_view text) {
  Tokenizer tokenizer(text);
  const std::vector<CssToken> tokens = tokenizer.Tokenize();
  TokenStream stream(tokens);
  return ParseDeclarationBody(stream);
}

}  // namespace neko::css
