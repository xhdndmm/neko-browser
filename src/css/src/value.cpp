#include "neko/css/value.h"

#include <cstdlib>
#include <string>
#include <string_view>

namespace neko::css {
namespace {

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

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

std::string ToLower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
  }
  return out;
}

}  // namespace

CssValue ParseCssValue(std::string_view text) {
  CssValue out;
  const std::string_view t = TrimView(text);
  if (t.empty()) {
    return out;
  }

  // Colors.
  if (const std::optional<Color> color = ParseColor(t)) {
    out.type = CssValue::Type::kColor;
    out.color = color;
    return out;
  }

  // Numbers with optional units: [+-]?digits[.digits]
  std::size_t i = 0;
  if (t[i] == '+' || t[i] == '-') {
    ++i;
  }
  bool has_digits = false;
  while (i < t.size() && IsDigit(t[i])) {
    has_digits = true;
    ++i;
  }
  if (i < t.size() && t[i] == '.') {
    ++i;
    while (i < t.size() && IsDigit(t[i])) {
      has_digits = true;
      ++i;
    }
  }
  if (has_digits) {
    const std::string number_text(t.substr(0, i));
    const float number = std::strtof(number_text.c_str(), nullptr);
    const std::string_view rest = t.substr(i);
    if (rest.empty()) {
      out.type = CssValue::Type::kNumber;
      out.number = number;
      return out;
    }
    const std::string unit = ToLower(rest);
    if (unit == "px" || unit == "em" || unit == "rem") {
      out.type = CssValue::Type::kLength;
      out.value = number;
      out.unit = unit;
      return out;
    }
    if (unit == "%") {
      out.type = CssValue::Type::kLength;
      out.value = number;
      out.is_percent = true;
      out.unit = "%";
      return out;
    }
    // Unknown unit: keep as unparsed (e.g. 'auto' handled elsewhere).
  }

  // Everything else is a keyword (or an unparsed multi-word value).
  out.text = ToLower(t);
  if (out.text.find(' ') == std::string::npos && out.text.find('\t') == std::string::npos) {
    out.type = CssValue::Type::kKeyword;
  } else {
    out.type = CssValue::Type::kUnparsed;
  }
  return out;
}

bool IsKeyword(const CssValue& value, std::string_view keyword) {
  return value.type == CssValue::Type::kKeyword && value.text == keyword;
}

}  // namespace neko::css
