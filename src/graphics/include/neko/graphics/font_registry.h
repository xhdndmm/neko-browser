#pragma once

#include "neko/graphics/font_library.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace neko::graphics {

class FontSelector;

// Owns the font library and caches one FontSelector per font-family value, so
// repeated layout/paint passes do not re-resolve or reload fonts.
class FontRegistry
{
public:
  FontRegistry();
  ~FontRegistry(); // defined in .cpp (needs FontSelector to be complete)

  // Selector for |family| (a CSS font-family value) with the given weight /
  // italic style; never null.  Thread-safe: the selector cache is a guarded
  // memo shared by the worker thread (layout) and raster-pool threads (paint).
  const FontSelector*
  SelectorFor(const std::string& family, int weight = 400, bool italic = false) const;

  // Registers an in-memory font (TTF/OTF/WOFF/WOFF2) as a web font for a
  // single CSS family name (from @font-face).  Later SelectorFor() calls
  // whose family list contains |family| resolve to this face first.
  // Thread-safe; invalidates cached selectors.  Returns nullptr when the
  // data does not parse.
  const FontFace* RegisterWebFont(const std::string& family,
                                  int weight,
                                  bool italic,
                                  const std::string& key,
                                  std::vector<uint8_t> data);

  FontLibrary& library()
  {
    return library_;
  }
  const FontLibrary& library() const
  {
    return library_;
  }

private:
  FontLibrary library_;

  struct WebFontKey
  {
    std::string family; // unquoted, lowercased single name
    int weight = 400;
    bool italic = false;
    bool operator<(const WebFontKey& other) const
    {
      if (family != other.family) {
        return family < other.family;
      }
      if (weight != other.weight) {
        return weight < other.weight;
      }
      return italic < other.italic;
    }
  };
  std::map<WebFontKey, const FontFace*> webfonts_; // guarded by mutex_

  mutable std::mutex mutex_;
  mutable std::unordered_map<std::string, std::unique_ptr<FontSelector>> selectors_;
};

} // namespace neko::graphics
