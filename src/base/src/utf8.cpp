#include "neko/base/utf8.h"

namespace neko::base {

std::string EncodeUtf8(char32_t code_point)
{
  if (code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
    code_point = 0xFFFD;
  }
  std::string out;
  if (code_point <= 0x7F) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
  return out;
}

bool DecodeUtf8Next(std::string_view input, std::size_t& pos, char32_t& out)
{
  if (pos >= input.size()) {
    return false;
  }
  const unsigned char b0 = static_cast<unsigned char>(input[pos]);
  if (b0 < 0x80) {
    out = b0;
    ++pos;
    return true;
  }
  int extra = 0;
  char32_t code_point = 0;
  if ((b0 & 0xE0) == 0xC0) {
    extra = 1;
    code_point = b0 & 0x1F;
  } else if ((b0 & 0xF0) == 0xE0) {
    extra = 2;
    code_point = b0 & 0x0F;
  } else if ((b0 & 0xF8) == 0xF0) {
    extra = 3;
    code_point = b0 & 0x07;
  } else {
    out = 0xFFFD;
    ++pos;
    return true;
  }
  if (pos + static_cast<std::size_t>(extra) >= input.size()) {
    out = 0xFFFD;
    ++pos;
    return true;
  }
  for (int i = 1; i <= extra; ++i) {
    const unsigned char b = static_cast<unsigned char>(input[pos + static_cast<std::size_t>(i)]);
    if ((b & 0xC0) != 0x80) {
      out = 0xFFFD;
      ++pos;
      return true;
    }
    code_point = (code_point << 6) | (b & 0x3F);
  }
  // Overlong encodings and surrogates are malformed.
  const bool overlong = (extra == 1 && code_point < 0x80) || (extra == 2 && code_point < 0x800) ||
                        (extra == 3 && code_point < 0x10000);
  if (overlong || code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
    out = 0xFFFD;
    pos += static_cast<std::size_t>(extra) + 1;
    return true;
  }
  out = code_point;
  pos += static_cast<std::size_t>(extra) + 1;
  return true;
}

} // namespace neko::base
