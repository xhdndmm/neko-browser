#include "neko/base/string_util.h"

#include <string>
#include <string_view>
#include <vector>

namespace neko::base {
namespace {

char AsciiToLowerChar(char c)
{
  if (c >= 'A' && c <= 'Z') {
    return static_cast<char>(c + ('a' - 'A'));
  }
  return c;
}

char AsciiToUpperChar(char c)
{
  if (c >= 'a' && c <= 'z') {
    return static_cast<char>(c - ('a' - 'A'));
  }
  return c;
}

} // namespace

bool AsciiEqualsIgnoreCase(std::string_view lhs, std::string_view rhs)
{
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (AsciiToLowerChar(lhs[i]) != AsciiToLowerChar(rhs[i])) {
      return false;
    }
  }
  return true;
}

bool AsciiStartsWith(std::string_view text, std::string_view prefix)
{
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool AsciiEndsWith(std::string_view text, std::string_view suffix)
{
  return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

bool Contains(std::string_view text, std::string_view needle)
{
  return text.find(needle) != std::string_view::npos;
}

std::string_view TrimLeft(std::string_view text)
{
  const std::size_t start = text.find_first_not_of(" \t\r\n\f\v");
  return start == std::string_view::npos ? std::string_view{} : text.substr(start);
}

std::string_view TrimRight(std::string_view text)
{
  const std::size_t end = text.find_last_not_of(" \t\r\n\f\v");
  return end == std::string_view::npos ? std::string_view{} : text.substr(0, end + 1);
}

std::string_view Trim(std::string_view text)
{
  return TrimRight(TrimLeft(text));
}

std::string ToLower(std::string_view text)
{
  std::string result;
  result.reserve(text.size());
  for (const char c : text) {
    result.push_back(AsciiToLowerChar(c));
  }
  return result;
}

std::string ToUpper(std::string_view text)
{
  std::string result;
  result.reserve(text.size());
  for (const char c : text) {
    result.push_back(AsciiToUpperChar(c));
  }
  return result;
}

std::vector<std::string_view> Split(std::string_view text, char delimiter)
{
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  for (;;) {
    const std::size_t pos = text.find(delimiter, start);
    if (pos == std::string_view::npos) {
      parts.push_back(text.substr(start));
      break;
    }
    parts.push_back(text.substr(start, pos - start));
    start = pos + 1;
  }
  return parts;
}

std::vector<std::string_view> Split(std::string_view text, std::string_view delimiter)
{
  std::vector<std::string_view> parts;
  if (delimiter.empty()) {
    parts.push_back(text);
    return parts;
  }
  std::size_t start = 0;
  for (;;) {
    const std::size_t pos = text.find(delimiter, start);
    if (pos == std::string_view::npos) {
      parts.push_back(text.substr(start));
      break;
    }
    parts.push_back(text.substr(start, pos - start));
    start = pos + delimiter.size();
  }
  return parts;
}

std::string Join(const std::vector<std::string_view>& parts, std::string_view separator)
{
  std::string result;
  std::size_t total = 0;
  for (const std::string_view part : parts) {
    total += part.size();
  }
  if (!parts.empty()) {
    total += separator.size() * (parts.size() - 1);
  }
  result.reserve(total);
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      result.append(separator);
    }
    result.append(parts[i]);
  }
  return result;
}

std::string ReplaceAll(std::string_view text, std::string_view from, std::string_view to)
{
  if (from.empty()) {
    return std::string(text);
  }
  std::string result;
  result.reserve(text.size());
  std::size_t pos = 0;
  for (;;) {
    const std::size_t found = text.find(from, pos);
    if (found == std::string_view::npos) {
      result.append(text.substr(pos));
      break;
    }
    result.append(text.substr(pos, found - pos));
    result.append(to);
    pos = found + from.size();
  }
  return result;
}

} // namespace neko::base
