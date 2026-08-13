#include "neko/storage/field_codec.h"

#include <cctype>

namespace neko::storage {
namespace {

bool IsUnreserved(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' ||
         c == '~' || c == '-';
}

constexpr char kHex[] = "0123456789ABCDEF";

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::string EncodeField(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (unsigned char c : value) {
    if (IsUnreserved(static_cast<char>(c))) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

base::Result<std::string> DecodeField(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '%') {
      out.push_back(value[i]);
      continue;
    }
    if (i + 2 >= value.size()) {  // need two hex digits after '%'
      return base::Error::Parse("truncated percent-escape in storage field");
    }
    const int hi = HexValue(value[i + 1]);
    const int lo = HexValue(value[i + 2]);
    if (hi < 0 || lo < 0) {
      return base::Error::Parse("invalid percent-escape in storage field");
    }
    out.push_back(static_cast<char>((hi << 4) | lo));
    i += 2;
  }
  return out;
}

}  // namespace neko::storage
