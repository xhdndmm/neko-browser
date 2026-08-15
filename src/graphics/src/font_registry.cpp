#include "neko/graphics/font_registry.h"

#include "neko/graphics/font_selector.h"

#include <utility>

namespace neko::graphics {

FontRegistry::FontRegistry() = default;
FontRegistry::~FontRegistry() = default;

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
  auto selector = std::make_unique<FontSelector>(library_, family, weight, italic);
  const FontSelector* raw = selector.get();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    selectors_.emplace(key, std::move(selector));
  }
  return raw;
}

} // namespace neko::graphics
