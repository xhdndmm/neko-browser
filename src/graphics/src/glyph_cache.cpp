#include "neko/graphics/glyph_cache.h"

#include <functional>

#include "neko/graphics/font_face.h"

namespace neko::graphics {

GlyphCache& GlyphCache::Instance() {
  static GlyphCache cache;
  return cache;
}

std::size_t GlyphCache::KeyHash::operator()(const Key& key) const {
  std::size_t hash = std::hash<const void*>{}(key.face);
  hash ^= std::hash<uint32_t>{}(key.code_point) + 0x9e3779b9U + (hash << 6) + (hash >> 2);
  hash ^= std::hash<int>{}(key.px) + 0x9e3779b9U + (hash << 6) + (hash >> 2);
  return hash;
}

const GlyphBitmap* GlyphCache::Find(const FontFace& face, uint32_t code_point, int px) {
  const auto it = index_.find(Key{&face, code_point, px});
  if (it == index_.end()) {
    return nullptr;
  }
  lru_.splice(lru_.begin(), lru_, it->second);  // LRU touch
  return &it->second->bitmap;
}

const GlyphBitmap* GlyphCache::Insert(const FontFace& face, uint32_t code_point, int px,
                                      GlyphBitmap glyph, std::vector<uint8_t> storage) {
  while (lru_.size() >= kMaxEntries) {
    const Key evict = lru_.back().key;
    index_.erase(evict);
    lru_.pop_back();
  }
  lru_.push_front(Entry{Key{&face, code_point, px}, std::move(glyph), std::move(storage)});
  auto it = lru_.begin();
  index_[it->key] = it;
  return &it->bitmap;
}

void GlyphCache::Clear() {
  index_.clear();
  lru_.clear();
}

}  // namespace neko::graphics
