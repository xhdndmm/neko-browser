#include "neko/graphics/utf8.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace neko::graphics {

void DecodeUtf8(std::string_view text, std::vector<uint32_t>& out) {
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    uint32_t code_point = 0;
    std::size_t len = 0;
    if (lead < 0x80) {
      code_point = lead;
      len = 1;
    } else if ((lead & 0xE0) == 0xC0) {
      code_point = lead & 0x1F;
      len = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      code_point = lead & 0x0F;
      len = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      code_point = lead & 0x07;
      len = 4;
    } else {
      out.push_back(0xFFFDU);
      ++i;
      continue;
    }
    if (i + len > text.size()) {
      out.push_back(0xFFFDU);
      break;
    }
    bool valid = true;
    for (std::size_t k = 1; k < len; ++k) {
      const unsigned char cc = static_cast<unsigned char>(text[i + k]);
      if ((cc & 0xC0) != 0x80) {
        valid = false;
        break;
      }
      code_point = (code_point << 6) | (cc & 0x3F);
    }
    if (!valid) {
      out.push_back(0xFFFDU);
      ++i;
      continue;
    }
    out.push_back(code_point);
    i += len;
  }
}

}  // namespace neko::graphics
