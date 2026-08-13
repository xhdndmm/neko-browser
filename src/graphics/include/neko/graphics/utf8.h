#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace neko::graphics {

// Decodes |text| (UTF-8) into code points appended to |out|.  Invalid
// sequences produce U+FFFD, matching HTML's replacement behavior.
void DecodeUtf8(std::string_view text, std::vector<uint32_t>& out);

}  // namespace neko::graphics
