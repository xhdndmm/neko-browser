#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace neko::graphics {

// CSS generic font families (subset; cursive/fantasy map to sans-serif).
enum class GenericFamily { kSansSerif, kSerif, kMonospace, kCjkSans };

// Returns the available system font paths for |family| in priority order (the
// first entry is the best match).  Empty when none of the candidates exist.
// Uses a small platform-aware candidate table (no fontconfig).
std::vector<std::string> FindSystemFonts(GenericFamily family);

// Best match for a concrete CSS font-family name (e.g. "Noto Sans CJK SC"),
// or empty when unknown.  Covers the common CJK/Latin names via a built-in
// table plus filename matching; full font-name scanning is future work.
std::string ResolveFamilyName(std::string_view name);

}  // namespace neko::graphics
