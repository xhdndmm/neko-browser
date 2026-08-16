#include "neko/storage/json_value.h"

#include "neko/base/utf8.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace neko::storage {
namespace {

constexpr std::size_t kMaxDepth = 128;
constexpr std::size_t kMaxInput = 64u * 1024u * 1024u;

bool IsWs(char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void SkipWs(std::string_view s, std::size_t& pos)
{
  while (pos < s.size() && IsWs(s[pos])) {
    ++pos;
  }
}

// Parses a \uXXXX escape; handles UTF-16 surrogate pairs by decoding to the
// code point first.  Returns false on malformed escapes.
bool ParseHex4(std::string_view s, std::size_t& pos, uint32_t& out)
{
  if (pos + 4 > s.size()) {
    return false;
  }
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    const char c = s[pos++];
    v <<= 4;
    if (c >= '0' && c <= '9') {
      v |= static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      v |= static_cast<uint32_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      v |= static_cast<uint32_t>(c - 'A' + 10);
    } else {
      return false;
    }
  }
  out = v;
  return true;
}

base::Result<std::string> ParseString(std::string_view s, std::size_t& pos)
{
  ++pos; // opening quote
  std::string out;
  while (pos < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[pos]);
    if (c == '"') {
      ++pos;
      return out;
    }
    if (c < 0x20) {
      return base::Error::Parse("json: control byte inside string");
    }
    if (c == '\\') {
      ++pos;
      if (pos >= s.size()) {
        return base::Error::Parse("json: trailing escape");
      }
      switch (s[pos]) {
      case '"':
        out.push_back('"');
        ++pos;
        break;
      case '\\':
        out.push_back('\\');
        ++pos;
        break;
      case '/':
        out.push_back('/');
        ++pos;
        break;
      case 'b':
        out.push_back('\b');
        ++pos;
        break;
      case 'f':
        out.push_back('\f');
        ++pos;
        break;
      case 'n':
        out.push_back('\n');
        ++pos;
        break;
      case 'r':
        out.push_back('\r');
        ++pos;
        break;
      case 't':
        out.push_back('\t');
        ++pos;
        break;
      case 'u': {
        ++pos;
        uint32_t cp = 0;
        if (!ParseHex4(s, pos, cp)) {
          return base::Error::Parse("json: bad \\u escape");
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          // High surrogate: expect a low surrogate.
          if (pos + 2 <= s.size() && s[pos] == '\\' && s[pos + 1] == 'u') {
            pos += 2;
            uint32_t low = 0;
            if (!ParseHex4(s, pos, low)) {
              return base::Error::Parse("json: bad \\u escape");
            }
            if (low >= 0xDC00 && low <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else {
              return base::Error::Parse("json: lone surrogate");
            }
          } else {
            return base::Error::Parse("json: lone surrogate");
          }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          return base::Error::Parse("json: lone surrogate");
        }
        out += base::EncodeUtf8(static_cast<char32_t>(cp));
        break;
      }
      default:
        return base::Error::Parse("json: bad escape");
      }
      continue;
    }
    // Raw UTF-8 passes through (JSON is UTF-8 text).
    out.push_back(s[pos]);
    ++pos;
  }
  return base::Error::Parse("json: unterminated string");
}

base::Result<JsonValue> ParseValue(std::string_view s, std::size_t& pos, std::size_t depth);

base::Result<JsonValue> ParseArray(std::string_view s, std::size_t& pos, std::size_t depth)
{
  ++pos; // '['
  JsonArray items;
  SkipWs(s, pos);
  if (pos < s.size() && s[pos] == ']') {
    ++pos;
    return JsonValue(std::move(items));
  }
  for (;;) {
    SkipWs(s, pos);
    base::Result<JsonValue> item = ParseValue(s, pos, depth);
    if (!item.has_value()) {
      return item.error();
    }
    items.push_back(std::move(item.value()));
    SkipWs(s, pos);
    if (pos >= s.size()) {
      return base::Error::Parse("json: unterminated array");
    }
    if (s[pos] == ']') {
      ++pos;
      return JsonValue(std::move(items));
    }
    if (s[pos] != ',') {
      return base::Error::Parse("json: expected ',' in array");
    }
    ++pos;
  }
}

base::Result<JsonValue> ParseObject(std::string_view s, std::size_t& pos, std::size_t depth)
{
  ++pos; // '{'
  JsonObject members;
  SkipWs(s, pos);
  if (pos < s.size() && s[pos] == '}') {
    ++pos;
    return JsonValue(std::move(members));
  }
  for (;;) {
    SkipWs(s, pos);
    if (pos >= s.size() || s[pos] != '"') {
      return base::Error::Parse("json: expected object key");
    }
    base::Result<std::string> key = ParseString(s, pos);
    if (!key.has_value()) {
      return key.error();
    }
    SkipWs(s, pos);
    if (pos >= s.size() || s[pos] != ':') {
      return base::Error::Parse("json: expected ':'");
    }
    ++pos;
    base::Result<JsonValue> member = ParseValue(s, pos, depth);
    if (!member.has_value()) {
      return member.error();
    }
    members.insert_or_assign(std::move(key.value()), std::move(member.value()));
    SkipWs(s, pos);
    if (pos >= s.size()) {
      return base::Error::Parse("json: unterminated object");
    }
    if (s[pos] == '}') {
      ++pos;
      return JsonValue(std::move(members));
    }
    if (s[pos] != ',') {
      return base::Error::Parse("json: expected ',' in object");
    }
    ++pos;
  }
}

base::Result<JsonValue> ParseValue(std::string_view s, std::size_t& pos, std::size_t depth)
{
  if (depth > kMaxDepth) {
    return base::Error::Parse("json: nesting too deep");
  }
  SkipWs(s, pos);
  if (pos >= s.size()) {
    return base::Error::Parse("json: unexpected end of input");
  }
  const char c = s[pos];
  if (c == '"') {
    base::Result<std::string> str = ParseString(s, pos);
    if (!str.has_value()) {
      return str.error();
    }
    return JsonValue(std::move(str.value()));
  }
  if (c == '[') {
    return ParseArray(s, pos, depth + 1);
  }
  if (c == '{') {
    return ParseObject(s, pos, depth + 1);
  }
  if (c == 't' && s.substr(pos, 4) == "true") {
    pos += 4;
    return JsonValue(true);
  }
  if (c == 'f' && s.substr(pos, 5) == "false") {
    pos += 5;
    return JsonValue(false);
  }
  if (c == 'n' && s.substr(pos, 4) == "null") {
    pos += 4;
    return JsonValue(nullptr);
  }
  // Number: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
  const std::size_t start = pos;
  if (c == '-') {
    ++pos;
  }
  if (pos >= s.size()) {
    return base::Error::Parse("json: bad number");
  }
  if (s[pos] == '0') {
    ++pos;
    if (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
      return base::Error::Parse("json: leading zero in number");
    }
  } else if (s[pos] >= '1' && s[pos] <= '9') {
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
      ++pos;
    }
  } else {
    return base::Error::Parse("json: bad number");
  }
  if (pos < s.size() && s[pos] == '.') {
    ++pos;
    const std::size_t frac = pos;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
      ++pos;
    }
    if (pos == frac) {
      return base::Error::Parse("json: bad fraction");
    }
  }
  if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
    ++pos;
    if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
      ++pos;
    }
    const std::size_t exp = pos;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
      ++pos;
    }
    if (pos == exp) {
      return base::Error::Parse("json: bad exponent");
    }
  }
  const std::string token(s.substr(start, pos - start));
  char* end = nullptr;
  const double v = std::strtod(token.c_str(), &end);
  if (end != token.c_str() + token.size()) {
    return base::Error::Parse("json: bad number");
  }
  if (!std::isfinite(v)) {
    return base::Error::Parse("json: number out of range");
  }
  return JsonValue(v);
}

void AppendEscaped(std::string& out, std::string_view s)
{
  static constexpr char kHex[] = "0123456789abcdef";
  for (const char raw : s) {
    const unsigned char c = static_cast<unsigned char>(raw);
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20) {
        out += "\\u00";
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0xF]);
      } else {
        out.push_back(static_cast<char>(c));
      }
      break;
    }
  }
}

void SerializeInto(const JsonValue& value, std::string& out)
{
  if (std::holds_alternative<std::nullptr_t>(value.value)) {
    out += "null";
  } else if (const auto* b = std::get_if<bool>(&value.value)) {
    out += *b ? "true" : "false";
  } else if (const auto* n = std::get_if<double>(&value.value)) {
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof(buf), *n);
    // Integer-valued doubles serialize without a fraction ("2", not "2.0"),
    // which matters for IndexedDB keys.
    out.append(buf, res.ptr);
  } else if (const auto* str = std::get_if<std::string>(&value.value)) {
    out.push_back('"');
    AppendEscaped(out, *str);
    out.push_back('"');
  } else if (const auto* arr = std::get_if<JsonArray>(&value.value)) {
    out.push_back('[');
    for (std::size_t i = 0; i < arr->size(); ++i) {
      if (i != 0) {
        out.push_back(',');
      }
      SerializeInto((*arr)[i], out);
    }
    out.push_back(']');
  } else if (const auto* obj = std::get_if<JsonObject>(&value.value)) {
    out.push_back('{');
    bool first = true;
    for (const auto& [key, member] : *obj) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      out.push_back('"');
      AppendEscaped(out, key);
      out += "\":";
      SerializeInto(member, out);
    }
    out.push_back('}');
  }
}

} // namespace

base::Result<JsonValue> ParseJson(std::string_view text)
{
  if (text.size() > kMaxInput) {
    return base::Error::Parse("json: input too large");
  }
  std::size_t pos = 0;
  base::Result<JsonValue> value = ParseValue(text, pos, 0);
  if (!value.has_value()) {
    return value.error();
  }
  SkipWs(text, pos);
  if (pos != text.size()) {
    return base::Error::Parse("json: trailing data");
  }
  return value;
}

std::string SerializeJson(const JsonValue& value)
{
  std::string out;
  SerializeInto(value, out);
  return out;
}

const JsonValue* JsonFind(const JsonValue& value, std::string_view key)
{
  const auto* obj = std::get_if<JsonObject>(&value.value);
  if (obj == nullptr) {
    return nullptr;
  }
  const auto it = obj->find(std::string(key));
  return it == obj->end() ? nullptr : &it->second;
}

} // namespace neko::storage
