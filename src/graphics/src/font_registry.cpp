#include "neko/graphics/font_registry.h"

#include "neko/graphics/font_face.h"
#include "neko/graphics/font_selector.h"

#include <utility>

namespace neko::graphics {

FontRegistry::FontRegistry() = default;
FontRegistry::~FontRegistry() = default;

namespace {

// Unquotes and lowercases one comma-separated family segment.
std::string NormalizeFamilyName(std::string_view name)
{
  std::string out;
  bool quote = false;
  for (const char ch : name) {
    if (ch == '"' || ch == '\'') {
      quote = !quote;
      continue;
    }
    out.push_back(quote ? ch : static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  while (!out.empty() && (out.front() == ' ' || out.front() == '\t')) {
    out.erase(out.begin());
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) {
    out.pop_back();
  }
  return out;
}

} // namespace

const FontSelector*
FontRegistry::SelectorFor(const std::string& family, int weight, bool italic) const
{
  const std::string key = family + "\x1f" + std::to_string(weight) + "\x1f" + (italic ? "i" : "r");
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = selectors_.find(key);
    if (it != selectors_.end()) {
      return it->second.get();
    }
  }
  // Web fonts whose family name appears in |family|'s list go first: an
  // exact (name, weight, italic) registration wins, otherwise a regular
  // (400) registration of the same name.
  std::vector<const FontFace*> preseed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t start = 0; start <= family.size();) {
      const std::size_t comma = family.find(',', start);
      const std::string segment = NormalizeFamilyName(
          family.substr(start, comma == std::string::npos ? family.size() - start : comma - start));
      if (!segment.empty()) {
        WebFontKey exact{segment, weight, italic};
        WebFontKey regular{segment, 400, false};
        const auto e = webfonts_.find(exact);
        if (e != webfonts_.end()) {
          preseed.push_back(e->second);
        } else if (italic || weight != 400) {
          const auto g = webfonts_.find(regular);
          if (g != webfonts_.end()) {
            preseed.push_back(g->second);
          }
        }
      }
      if (comma == std::string::npos) {
        break;
      }
      start = comma + 1;
    }
  }
  auto selector = std::make_unique<FontSelector>(library_, family, weight, italic, preseed);
  const FontSelector* raw = selector.get();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    selectors_.emplace(key, std::move(selector));
  }
  return raw;
}

const FontFace* FontRegistry::RegisterWebFont(const std::string& family,
                                              int weight,
                                              bool italic,
                                              const std::string& key,
                                              std::vector<uint8_t> data)
{
  const FontFace* face = library_.LoadFaceFromMemory(key, std::move(data));
  if (face == nullptr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  WebFontKey k{NormalizeFamilyName(family), weight, italic};
  webfonts_[k] = face;
  // Cached selectors were built without this face; rebuild on next use.
  selectors_.clear();
  return face;
}

} // namespace neko::graphics
