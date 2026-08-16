#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace neko::graphics {

// CSS generic font families (subset; cursive/fantasy map to sans-serif).
enum class GenericFamily
{
  kSansSerif,
  kSerif,
  kMonospace,
  kCjkSans
};

// Returns the available system font paths for |family| in priority order (the
// first entry is the best match).  Empty when none of the candidates exist.
// Uses a small platform-aware candidate table (no fontconfig).
std::vector<std::string> FindSystemFonts(GenericFamily family);

// Best match for a concrete CSS font-family name (e.g. "Noto Sans CJK SC"),
// or empty when unknown.  Covers the common CJK/Latin names via a built-in
// table plus filename matching; full font-name scanning is future work.
std::string ResolveFamilyName(std::string_view name);

// Returns a bold/italic variant of |base_path| when such a file exists next to
// it (e.g. .../LiberationSans-Regular.ttf -> .../LiberationSans-BoldItalic.ttf),
// otherwise returns |base_path| unchanged.  |weight| >= 600 requests bold.
std::string FindFontVariant(const std::string& base_path, int weight, bool italic);

} // namespace neko::graphics
