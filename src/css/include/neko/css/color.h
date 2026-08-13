#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace neko::css {

// RGBA color.  Components are 0..255 (alpha 0..255).
struct Color {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t a = 255;

  bool operator==(const Color& other) const {
    return r == other.r && g == other.g && b == other.b && a == other.a;
  }
};

// Parses a CSS color value: #rgb, #rrggbb, #rrggbbaa, rgb()/rgba() (numbers
// or percentages) or a named color (common subset).  Returns nullopt for
// anything unrecognized (the value is then treated as invalid/transparent).
std::optional<Color> ParseColor(std::string_view text);

}  // namespace neko::css
