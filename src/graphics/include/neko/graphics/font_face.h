#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace neko::graphics {

// A rasterized glyph: 8-bit alpha bitmap plus horizontal metrics.
//
// |data| points into the glyph cache's storage (pitch bytes per row, width
// pixels per row) and stays valid until the entry is evicted.
struct GlyphBitmap {
  int width = 0;    // bitmap width in px
  int height = 0;   // bitmap height in px
  int pitch = 0;    // bytes per row (>= width)
  int left = 0;     // bearing X: bitmap left edge relative to the pen position
  int top = 0;      // bearing Y: bitmap top relative to the baseline (up = +)
  float advance = 0;  // horizontal advance in px
  const uint8_t* data = nullptr;
};

// One font file (TrueType/OpenType), loaded through the shared FreeType
// library.  Thread-confined like the rest of the rendering pipeline.  Glyph
// rasterization is memoized by (pixel size, code point) in the process-wide
// glyph cache.
class FontFace {
 public:
  // Opens |path|; valid() reports whether the file parsed successfully.
  explicit FontFace(std::string path);
  ~FontFace();

  FontFace(const FontFace&) = delete;
  FontFace& operator=(const FontFace&) = delete;

  bool valid() const;
  const std::string& path() const { return path_; }

  // Horizontal advance (px) of |code_point| at |px_size| (font size in px).
  float Advance(uint32_t code_point, float px_size) const;

  // Total horizontal advance (px) of |text| (UTF-8) at |px_size|.
  float TextWidth(std::string_view text, float px_size) const;

  // Rasterized glyph for |code_point| at |px_size| (cached).  Returns nullptr
  // for an unusable face or invalid input.
  const GlyphBitmap* RenderGlyph(uint32_t code_point, float px_size) const;

  // Ascent above the baseline (px) at |px_size|.
  float Ascent(float px_size) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string path_;
};

}  // namespace neko::graphics
