#include "neko/graphics/font_face.h"

#include <cstring>
#include <ft2build.h>
#include <vector>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include "neko/graphics/glyph_cache.h"
#include "neko/graphics/utf8.h"

#include "internal.h"

namespace neko::graphics {
namespace {

// Applies |px_size| to |face|; returns true on success.
bool SetPixelSize(FT_Face face, float px_size)
{
  if (px_size <= 0.0f) {
    return false;
  }
  const FT_UInt px = static_cast<FT_UInt>(px_size + 0.5f);
  return FT_Set_Pixel_Sizes(face, 0, px) == 0;
}

// FT_Outline_Decompose callbacks: they convert FreeType's y-up font units into
// the y-down pixel space used by GlyphBitmap bearers and by SVG.
struct OutlineSink
{
  std::vector<OutlineEdge>* edges = nullptr;
  double scale = 0; // pixels per font unit
};

int MoveTo(const FT_Vector* to, void* user)
{
  auto* sink = static_cast<OutlineSink*>(user);
  OutlineEdge edge;
  edge.kind = OutlineKind::kMove;
  edge.p[0] = static_cast<float>(static_cast<double>(to->x) * sink->scale);
  edge.p[1] = static_cast<float>(-static_cast<double>(to->y) * sink->scale);
  sink->edges->push_back(edge);
  return 0;
}

int LineTo(const FT_Vector* to, void* user)
{
  auto* sink = static_cast<OutlineSink*>(user);
  OutlineEdge edge;
  edge.kind = OutlineKind::kLine;
  edge.p[0] = static_cast<float>(static_cast<double>(to->x) * sink->scale);
  edge.p[1] = static_cast<float>(-static_cast<double>(to->y) * sink->scale);
  sink->edges->push_back(edge);
  return 0;
}

int ConicTo(const FT_Vector* control, const FT_Vector* to, void* user)
{
  auto* sink = static_cast<OutlineSink*>(user);
  OutlineEdge edge;
  edge.kind = OutlineKind::kQuadratic;
  edge.p[0] = static_cast<float>(static_cast<double>(control->x) * sink->scale);
  edge.p[1] = static_cast<float>(-static_cast<double>(control->y) * sink->scale);
  edge.p[2] = static_cast<float>(static_cast<double>(to->x) * sink->scale);
  edge.p[3] = static_cast<float>(-static_cast<double>(to->y) * sink->scale);
  sink->edges->push_back(edge);
  return 0;
}

int CubicTo(const FT_Vector* control1, const FT_Vector* control2, const FT_Vector* to, void* user)
{
  auto* sink = static_cast<OutlineSink*>(user);
  OutlineEdge edge;
  edge.kind = OutlineKind::kCubic;
  edge.p[0] = static_cast<float>(static_cast<double>(control1->x) * sink->scale);
  edge.p[1] = static_cast<float>(-static_cast<double>(control1->y) * sink->scale);
  edge.p[2] = static_cast<float>(static_cast<double>(control2->x) * sink->scale);
  edge.p[3] = static_cast<float>(-static_cast<double>(control2->y) * sink->scale);
  edge.p[4] = static_cast<float>(static_cast<double>(to->x) * sink->scale);
  edge.p[5] = static_cast<float>(-static_cast<double>(to->y) * sink->scale);
  sink->edges->push_back(edge);
  return 0;
}

} // namespace

struct FontFace::Impl
{
  FT_Face face = nullptr;
  // Owned font bytes for memory-loaded faces (FreeType reads lazily, so the
  // buffer must outlive the FT_Face).
  std::vector<uint8_t> owned_data;
  // Serializes FreeType access on this face.  The face stores mutable state
  // (current pixel size, glyph slot), so concurrent users must be serialized;
  // this also keeps the font caches' rasterization atomic per face.
  mutable std::mutex ft_mutex;
};

FontFace::FontFace(std::string path) : impl_(new Impl), path_(std::move(path))
{
  FT_Library library = SharedFreeTypeLibrary();
  if (library == nullptr) {
    return;
  }
  if (FT_New_Face(library, path_.c_str(), 0, &impl_->face) != 0) {
    impl_->face = nullptr;
  }
}

FontFace::FontFace(std::string key, std::vector<uint8_t> data)
    : impl_(new Impl), path_(std::move(key))
{
  FT_Library library = SharedFreeTypeLibrary();
  if (library == nullptr) {
    return;
  }
  impl_->owned_data = std::move(data);
  if (FT_New_Memory_Face(library,
                         reinterpret_cast<const FT_Byte*>(impl_->owned_data.data()),
                         static_cast<FT_Long>(impl_->owned_data.size()),
                         0,
                         &impl_->face) != 0) {
    impl_->face = nullptr;
  }
}

FontFace::~FontFace()
{
  if (impl_->face != nullptr) {
    FT_Done_Face(impl_->face);
  }
}

bool FontFace::valid() const
{
  return impl_->face != nullptr;
}

bool FontFace::HasGlyph(uint32_t code_point) const
{
  if (!valid()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->ft_mutex);
  return FT_Get_Char_Index(impl_->face, static_cast<FT_ULong>(code_point)) != 0;
}

float FontFace::Advance(uint32_t code_point, float px_size) const
{
  if (!valid()) {
    return 0.0f;
  }
  std::lock_guard<std::mutex> lock(impl_->ft_mutex);
  if (!SetPixelSize(impl_->face, px_size)) {
    return 0.0f;
  }
  const FT_UInt glyph_index = FT_Get_Char_Index(impl_->face, static_cast<FT_ULong>(code_point));
  // glyph_index 0 is the .notdef glyph; loading it yields the missing-glyph
  // advance so unknown characters still take horizontal space.
  if (FT_Load_Glyph(impl_->face, glyph_index, FT_LOAD_DEFAULT) != 0) {
    return 0.0f;
  }
  return static_cast<float>(impl_->face->glyph->advance.x) / 64.0f;
}

float FontFace::TextWidth(std::string_view text, float px_size) const
{
  std::vector<uint32_t> code_points;
  DecodeUtf8(text, code_points);
  float width = 0;
  for (const uint32_t code_point : code_points) {
    width += Advance(code_point, px_size);
  }
  return width;
}

std::optional<RasterizedGlyph> FontFace::RenderGlyph(uint32_t code_point, float px_size) const
{
  if (!valid() || px_size <= 0.0f) {
    return std::nullopt;
  }
  const int px = static_cast<int>(px_size + 0.5f);
  GlyphCache& cache = GlyphCache::Instance();

  // Cache hit: the cache returns an owned copy (data copied while its lock is
  // held), so concurrent eviction by parallel band workers cannot invalidate
  // what the caller uses.
  if (std::optional<RasterizedGlyph> hit = cache.GetOwned(*this, code_point, px)) {
    return hit;
  }

  // Rasterize under the face lock; the cache operations are internally
  // thread-safe.
  std::lock_guard<std::mutex> lock(impl_->ft_mutex);
  if (!SetPixelSize(impl_->face, px_size)) {
    return std::nullopt;
  }
  const FT_UInt glyph_index = FT_Get_Char_Index(impl_->face, static_cast<FT_ULong>(code_point));
  if (FT_Load_Glyph(impl_->face, glyph_index, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
    return std::nullopt;
  }
  const FT_GlyphSlot slot = impl_->face->glyph;
  if (slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
    return std::nullopt; // color/other bitmap formats are out of scope for now
  }
  const int width = static_cast<int>(slot->bitmap.width);
  const int height = static_cast<int>(slot->bitmap.rows);
  RasterizedGlyph out;
  if (width <= 0 || height <= 0 || slot->bitmap.buffer == nullptr) {
    // Whitespace glyphs have a zero-size bitmap but a valid advance.
    out.glyph.advance = static_cast<float>(slot->advance.x) / 64.0f;
    out.glyph.data = nullptr;
    cache.Insert(*this, code_point, px, out.glyph, {});
    return out;
  }
  out.storage.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
  for (int row = 0; row < height; ++row) {
    std::memcpy(out.storage.data() +
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(width),
                slot->bitmap.buffer + row * slot->bitmap.pitch,
                static_cast<std::size_t>(width));
  }
  out.glyph.width = width;
  out.glyph.height = height;
  out.glyph.pitch = width;
  out.glyph.left = slot->bitmap_left;
  out.glyph.top = slot->bitmap_top;
  out.glyph.advance = static_cast<float>(slot->advance.x) / 64.0f;
  out.glyph.data = out.storage.data();

  // Store a copy in the cache (the cache fixes up its own data pointer) and
  // return the caller's owned copy; no pointer into the cache escapes.
  cache.Insert(*this, code_point, px, out.glyph, out.storage);
  return out;
}

std::optional<GlyphOutline> FontFace::OutlineGlyph(uint32_t code_point, float px_size) const
{
  if (!valid() || px_size <= 0.0f) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(impl_->ft_mutex);
  if (!SetPixelSize(impl_->face, px_size) || impl_->face->size == nullptr) {
    return std::nullopt;
  }
  const FT_UInt glyph_index = FT_Get_Char_Index(impl_->face, static_cast<FT_ULong>(code_point));
  // FT_LOAD_NO_SCALE keeps the outline in font units (design shape, no hinting
  // and no rounding), which the sink scales by px_size / units_per_EM.  Relying
  // on the slot's own scaling is not enough: with hinting off the outline is
  // not guaranteed to be in 26.6 pixels, which silently mis-sized SVG text.
  if (FT_Load_Glyph(impl_->face, glyph_index, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) != 0) {
    return std::nullopt;
  }
  const FT_GlyphSlot slot = impl_->face->glyph;
  const double units_per_em =
      impl_->face->units_per_EM != 0 ? static_cast<double>(impl_->face->units_per_EM) : 0.0;
  if (units_per_em <= 0.0) {
    return std::nullopt;
  }
  const double unit_scale = static_cast<double>(px_size) / units_per_em;
  GlyphOutline out;
  out.advance = static_cast<float>(static_cast<double>(slot->advance.x) * unit_scale);
  if (slot->format != FT_GLYPH_FORMAT_OUTLINE || slot->outline.n_points == 0) {
    return out; // whitespace (or a bitmap-only glyph): advance, no shape
  }
  // Font units -> pixels.
  OutlineSink sink;
  sink.edges = &out.edges;
  sink.scale = unit_scale;
  FT_Outline_Funcs funcs{};
  funcs.move_to = MoveTo;
  funcs.line_to = LineTo;
  funcs.conic_to = ConicTo;
  funcs.cubic_to = CubicTo;
  if (FT_Outline_Decompose(&slot->outline, &funcs, &sink) != 0) {
    return std::nullopt;
  }
  return out;
}

float FontFace::Ascent(float px_size) const
{
  if (!valid()) {
    return 0.0f;
  }
  std::lock_guard<std::mutex> lock(impl_->ft_mutex);
  if (!SetPixelSize(impl_->face, px_size) || impl_->face->size == nullptr) {
    return 0.0f;
  }
  // face->size->metrics are scaled to the current pixel size (26.6 fixed
  // point); face->ascender alone is in unscaled font units.
  return static_cast<float>(impl_->face->size->metrics.ascender) / 64.0f;
}

float FontFace::Descent(float px_size) const
{
  if (!valid()) {
    return 0.0f;
  }
  std::lock_guard<std::mutex> lock(impl_->ft_mutex);
  if (!SetPixelSize(impl_->face, px_size) || impl_->face->size == nullptr) {
    return 0.0f;
  }
  // FreeType's descender is negative (below the baseline); report a positive
  // magnitude for line box / leading math.
  return -static_cast<float>(impl_->face->size->metrics.descender) / 64.0f;
}

} // namespace neko::graphics
