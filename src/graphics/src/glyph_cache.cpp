#include "neko/graphics/glyph_cache.h"

#include "neko/graphics/font_face.h"

#include <functional>

namespace neko::graphics {

GlyphCache& GlyphCache::Instance()
{
  static GlyphCache cache;
  return cache;
}

std::size_t GlyphCache::KeyHash::operator()(const Key& key) const
{
  std::size_t hash = std::hash<const void*>{}(key.face);
  hash ^= std::hash<uint32_t>{}(key.code_point) + 0x9e3779b9U + (hash << 6) + (hash >> 2);
  hash ^= std::hash<int>{}(key.px) + 0x9e3779b9U + (hash << 6) + (hash >> 2);
  return hash;
}

std::optional<RasterizedGlyph>
GlyphCache::GetOwned(const FontFace& face, uint32_t code_point, int px)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = index_.find(Key{&face, code_point, px});
  if (it == index_.end()) {
    return std::nullopt;
  }
  lru_.splice(lru_.begin(), lru_, it->second); // LRU touch
  const Entry& entry = *it->second;
  RasterizedGlyph out;
  out.glyph = entry.bitmap;
  if (entry.bitmap.data != nullptr && entry.bitmap.height > 0) {
    out.storage.assign(entry.bitmap.data,
                       entry.bitmap.data + static_cast<std::size_t>(entry.bitmap.pitch) *
                                               static_cast<std::size_t>(entry.bitmap.height));
  }
  out.glyph.data = out.storage.empty() ? nullptr : out.storage.data();
  return out;
}

void GlyphCache::Insert(const FontFace& face,
                        uint32_t code_point,
                        int px,
                        GlyphBitmap glyph,
                        std::vector<uint8_t> storage)
{
  std::lock_guard<std::mutex> lock(mutex_);
  while (lru_.size() >= kMaxEntries) {
    const Key evict = lru_.back().key;
    index_.erase(evict);
    lru_.pop_back();
  }
  lru_.push_front(Entry{Key{&face, code_point, px}, std::move(glyph), std::move(storage)});
  auto it = lru_.begin();
  // The stored entry must be self-consistent: its data pointer points at the
  // cache-owned storage (the caller passed a glyph whose data pointed at its
  // own buffer, which is a separate allocation after the move).
  it->bitmap.data = it->storage.empty() ? nullptr : it->storage.data();
  index_[it->key] = it;
}

void GlyphCache::Clear()
{
  std::lock_guard<std::mutex> lock(mutex_);
  index_.clear();
  lru_.clear();
}

std::size_t GlyphCache::size() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return lru_.size();
}

} // namespace neko::graphics
