#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>

namespace neko::base {

// UTF-8 helpers used by the HTML tokenizer and text pipeline.
//
// Malformed input never crashes or throws: invalid code points are replaced
// with U+FFFD (following the WHATWG encoding "best fit" replacement model).

// Encodes one Unicode code point to UTF-8.  Invalid code points (surrogates,
// values above U+10FFFF) become U+FFFD.
std::string EncodeUtf8(char32_t code_point);

// Decodes the code point starting at |pos|, advancing |pos| past it.  On
// malformed input, writes U+FFFD and advances by one byte.  Returns false
// only when |pos| was already at the end of |input|.
bool DecodeUtf8Next(std::string_view input, std::size_t& pos, char32_t& out);

}  // namespace neko::base
