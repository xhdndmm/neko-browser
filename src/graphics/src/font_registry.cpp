#include "neko/graphics/font_registry.h"

#include <utility>

#include "neko/graphics/font_selector.h"

namespace neko::graphics {

FontRegistry::FontRegistry() = default;
FontRegistry::~FontRegistry() = default;

const FontSelector* FontRegistry::SelectorFor(const std::string& family, int weight,
                                              bool italic) const {
  const std::string key =
      family + "\x1f" + std::to_string(weight) + "\x1f" + (italic ? "i" : "r");
  auto it = selectors_.find(key);
  if (it != selectors_.end()) {
    return it->second.get();
  }
  auto selector = std::make_unique<FontSelector>(library_, family, weight, italic);
  const FontSelector* raw = selector.get();
  selectors_.emplace(key, std::move(selector));
  return raw;
}

}  // namespace neko::graphics
