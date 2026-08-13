#pragma once

#include <optional>
#include <string>

namespace neko::graphics {

// CSS generic font families (subset; full font-family matching is M3).
enum class GenericFamily { kSansSerif, kSerif, kMonospace, kCjkSans };

// Returns a path to the best available system font for |family|, or nullopt
// when none of the candidates exist.  Uses a small platform-aware candidate
// table (no fontconfig); M3 extends discovery with directory scans and
// font-family name matching.
std::optional<std::string> FindSystemFont(GenericFamily family);

}  // namespace neko::graphics
