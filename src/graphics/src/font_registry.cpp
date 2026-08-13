#include "neko/graphics/font_registry.h"

#include <utility>

#include "neko/graphics/font_selector.h"

namespace neko::graphics {

FontRegistry::FontRegistry() = default;
FontRegistry::~FontRegistry() = default;

const FontSelector* FontRegistry::SelectorFor(const std::string& family) const {
  auto it = selectors_.find(family);
  if (it != selectors_.end()) {
    return it->second.get();
  }
  auto selector = std::make_unique<FontSelector>(library_, family);
  const FontSelector* raw = selector.get();
  selectors_.emplace(family, std::move(selector));
  return raw;
}

}  // namespace neko::graphics
