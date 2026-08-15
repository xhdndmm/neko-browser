#pragma once

#include "neko/graphics/font_face.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace neko::graphics {

class FontFace;

// Process-wide LRU cache of rasterized glyphs, keyed by
// (face, code point, pixel size).  A whole screen of CJK text reuses the same
// few hundred glyphs, so memoization avoids re-rasterizing per draw call.
//
// The cache is internally thread-safe: the rendering pipeline rasterizes
// viewport bands on a thread pool, so Find/Insert are serialized.  Callers
// receive owned copies of glyphs (the storage can be evicted concurrently).
class GlyphCache
{
public:
  static GlyphCache& Instance();

  // Returns an owned copy of the cached glyph for (face, code_point, px), or
  // nullopt on a miss.  The pixel data is copied while the cache lock is held,
  // so the caller's copy stays valid even if another thread evicts the entry
  // right after (parallel band rasterization).
  std::optional<RasterizedGlyph> GetOwned(const FontFace& face, uint32_t code_point, int px);

  // Stores a copy of |glyph| (whose |data| points into |storage|) keyed by
  // (face, code_point, px), evicting the least-recently-used entry when full.
  // The stored entry's data pointer is fixed up to point at the cache's own
  // storage, so the cache is always internally consistent.  Thread-safe.
  void Insert(const FontFace& face,
              uint32_t code_point,
              int px,
              GlyphBitmap glyph,
              std::vector<uint8_t> storage);

  void Clear();
  std::size_t size() const;

private:
  GlyphCache() = default;

  struct Key
  {
    const FontFace* face = nullptr;
    uint32_t code_point = 0;
    int px = 0;
    bool operator==(const Key&) const = default;
  };
  struct KeyHash
  {
    std::size_t operator()(const Key& key) const;
  };
  struct Entry
  {
    Key key;
    GlyphBitmap bitmap;
    std::vector<uint8_t> storage;
  };

  mutable std::mutex mutex_;
  std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index_;
  std::list<Entry> lru_;
  static constexpr std::size_t kMaxEntries = 2048;
};

} // namespace neko::graphics
