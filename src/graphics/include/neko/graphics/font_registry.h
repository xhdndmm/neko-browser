#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "neko/graphics/font_library.h"

namespace neko::graphics {

class FontSelector;

// Owns the font library and caches one FontSelector per font-family value, so
// repeated layout/paint passes do not re-resolve or reload fonts.
class FontRegistry {
 public:
  FontRegistry();
  ~FontRegistry();  // defined in .cpp (needs FontSelector to be complete)

  // Selector for |family| (a CSS font-family value) with the given weight /
  // italic style; never null.  Const-safe: the selector cache is a mutable
  // memo.
  const FontSelector* SelectorFor(const std::string& family, int weight = 400,
                                  bool italic = false) const;

  FontLibrary& library() { return library_; }
  const FontLibrary& library() const { return library_; }

 private:
  FontLibrary library_;
  mutable std::unordered_map<std::string, std::unique_ptr<FontSelector>> selectors_;
};

}  // namespace neko::graphics
