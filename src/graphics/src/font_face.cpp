#include "neko/graphics/font_face.h"

#include <cstring>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "internal.h"
#include "neko/graphics/glyph_cache.h"
#include "neko/graphics/utf8.h"

namespace neko::graphics {
namespace {

// Applies |px_size| to |face|; returns true on success.
bool SetPixelSize(FT_Face face, float px_size) {
  if (px_size <= 0.0f) {
    return false;
  }
  const FT_UInt px = static_cast<FT_UInt>(px_size + 0.5f);
  return FT_Set_Pixel_Sizes(face, 0, px) == 0;
}

}  // namespace

struct FontFace::Impl {
  FT_Face face = nullptr;
};

FontFace::FontFace(std::string path) : impl_(new Impl), path_(std::move(path)) {
  FT_Library library = SharedFreeTypeLibrary();
  if (library == nullptr) {
    return;
  }
  if (FT_New_Face(library, path_.c_str(), 0, &impl_->face) != 0) {
    impl_->face = nullptr;
  }
}

FontFace::~FontFace() {
  if (impl_->face != nullptr) {
    FT_Done_Face(impl_->face);
  }
}

bool FontFace::valid() const { return impl_->face != nullptr; }

bool FontFace::HasGlyph(uint32_t code_point) const {
  if (!valid()) {
    return false;
  }
  return FT_Get_Char_Index(impl_->face, static_cast<FT_ULong>(code_point)) != 0;
}

float FontFace::Advance(uint32_t code_point, float px_size) const {
  if (!valid() || !SetPixelSize(impl_->face, px_size)) {
    return 0.0f;
  }
  const FT_UInt glyph_index =
      FT_Get_Char_Index(impl_->face, static_cast<FT_ULong>(code_point));
  // glyph_index 0 is the .notdef glyph; loading it yields the missing-glyph
  // advance so unknown characters still take horizontal space.
  if (FT_Load_Glyph(impl_->face, glyph_index, FT_LOAD_DEFAULT) != 0) {
    return 0.0f;
  }
  return static_cast<float>(impl_->face->glyph->advance.x) / 64.0f;
}

float FontFace::TextWidth(std::string_view text, float px_size) const {
  std::vector<uint32_t> code_points;
  DecodeUtf8(text, code_points);
  float width = 0;
  for (const uint32_t code_point : code_points) {
    width += Advance(code_point, px_size);
  }
  return width;
}

const GlyphBitmap* FontFace::RenderGlyph(uint32_t code_point, float px_size) const {
  if (!valid() || px_size <= 0.0f) {
    return nullptr;
  }
  const int px = static_cast<int>(px_size + 0.5f);
  GlyphCache& cache = GlyphCache::Instance();
  if (const GlyphBitmap* hit = cache.Find(*this, code_point, px)) {
    return hit;
  }
  if (!SetPixelSize(impl_->face, px_size)) {
    return nullptr;
  }
  const FT_UInt glyph_index =
      FT_Get_Char_Index(impl_->face, static_cast<FT_ULong>(code_point));
  if (FT_Load_Glyph(impl_->face, glyph_index, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
    return nullptr;
  }
  const FT_GlyphSlot slot = impl_->face->glyph;
  if (slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
    return nullptr;  // color/other bitmap formats are out of scope for now
  }
  const int width = static_cast<int>(slot->bitmap.width);
  const int height = static_cast<int>(slot->bitmap.rows);
  if (width <= 0 || height <= 0 || slot->bitmap.buffer == nullptr) {
    // Whitespace glyphs have a zero-size bitmap but a valid advance.
    GlyphBitmap empty;
    empty.advance = static_cast<float>(slot->advance.x) / 64.0f;
    return cache.Insert(*this, code_point, px, empty, {});
  }
  std::vector<uint8_t> storage(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
  for (int row = 0; row < height; ++row) {
    std::memcpy(storage.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width),
                slot->bitmap.buffer + row * slot->bitmap.pitch,
                static_cast<std::size_t>(width));
  }
  GlyphBitmap glyph;
  glyph.width = width;
  glyph.height = height;
  glyph.pitch = width;
  glyph.left = slot->bitmap_left;
  glyph.top = slot->bitmap_top;
  glyph.advance = static_cast<float>(slot->advance.x) / 64.0f;
  glyph.data = storage.data();
  return cache.Insert(*this, code_point, px, glyph, std::move(storage));
}

float FontFace::Ascent(float px_size) const {
  if (!valid() || !SetPixelSize(impl_->face, px_size) || impl_->face->size == nullptr) {
    return 0.0f;
  }
  // face->size->metrics are scaled to the current pixel size (26.6 fixed
  // point); face->ascender alone is in unscaled font units.
  return static_cast<float>(impl_->face->size->metrics.ascender) / 64.0f;
}

}  // namespace neko::graphics
