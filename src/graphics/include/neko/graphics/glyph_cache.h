#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>
#include <vector>

#include "neko/graphics/font_face.h"

namespace neko::graphics {

class FontFace;

// Process-wide LRU cache of rasterized glyphs, keyed by
// (face, code point, pixel size).  A whole screen of CJK text reuses the same
// few hundred glyphs, so memoization avoids re-rasterizing per draw call.
//
// The rendering pipeline is single-threaded; a process-wide instance is a
// documented simplification (a per-page or per-thread cache can replace it if
// the pipeline ever goes multi-threaded).
class GlyphCache {
 public:
  static GlyphCache& Instance();

  // Cached glyph for (face, code_point, px), or nullptr on miss.  The returned
  // pointer stays valid until the entry is evicted.
  const GlyphBitmap* Find(const FontFace& face, uint32_t code_point, int px);

  // Stores |glyph| (whose |data| points into |storage|) and returns the stored
  // glyph pointer.  Evicts the least-recently-used entry when full.
  const GlyphBitmap* Insert(const FontFace& face, uint32_t code_point, int px,
                            GlyphBitmap glyph, std::vector<uint8_t> storage);

  void Clear();
  std::size_t size() const { return lru_.size(); }

 private:
  GlyphCache() = default;

  struct Key {
    const FontFace* face = nullptr;
    uint32_t code_point = 0;
    int px = 0;
    bool operator==(const Key&) const = default;
  };
  struct KeyHash {
    std::size_t operator()(const Key& key) const;
  };
  struct Entry {
    Key key;
    GlyphBitmap bitmap;
    std::vector<uint8_t> storage;
  };

  std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index_;
  std::list<Entry> lru_;
  static constexpr std::size_t kMaxEntries = 2048;
};

}  // namespace neko::graphics
