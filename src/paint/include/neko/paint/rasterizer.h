#pragma once

#include "neko/base/status.h"
#include "neko/css/color.h"
#include "neko/paint/display_list.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace neko::base {
class ThreadPool;
}

namespace neko::graphics {
class FontRegistry;
struct GlyphBitmap;
} // namespace neko::graphics

namespace neko::paint {

// Software rasterizer for DisplayList commands.  Produces an RGBA8888 buffer.
//
// Phase 6 scope: axis-aligned fills, borders and text.  Text is drawn with a
// FreeType-backed font stack (neko::graphics FontRegistry, per-character
// fallback for CJK) when a registry is set; otherwise the embedded 8x8 bitmap
// font is used as a fallback so pages never lose text.
//
// Performance features (Phase "renderer perf"):
//   * The pixel buffer is reused across Resize() calls (capacity is kept), so
//     repaints do not reallocate the framebuffer.
//   * The whole viewport can be rasterized in parallel horizontal bands via
//     RasterizeParallel() (each pixel belongs to exactly one band, so bands
//     are independent; the font caches are internally thread-safe).
//   * SetVisibleBand() restricts drawing to a horizontal strip, used by the
//     UI's scroll blit (only the newly exposed rows are re-rasterized).
//   * Alpha blending is integer fixed-point (no per-pixel float ops).
class Rasterizer
{
public:
  Rasterizer(int width, int height);
  // Copies deep-copy the pixel buffer when owning (and re-point pixels_data_
  // at the new storage); non-owning band views keep their external pointer.
  // Moves transfer the buffer and fix up pixels_data_.
  Rasterizer(const Rasterizer& other);
  Rasterizer& operator=(const Rasterizer& other);
  Rasterizer(Rasterizer&& other) noexcept;
  Rasterizer& operator=(Rasterizer&& other) noexcept;
  ~Rasterizer() = default;

  // Re-sizes the buffer, reusing existing storage when the new size fits
  // (avoids a per-frame allocation in the GUI's paint path).
  void Resize(int width, int height);

  void Clear(css::Color color);

  // Clears only rows [y0, y1) to |color| (used for the exposed band after a
  // scroll blit).
  void ClearBand(int y0, int y1, css::Color color);

  // Rasterizes the whole list into the buffer (clipped to the viewport).
  void Rasterize(const DisplayList& list);

  // Like Rasterize but splits the viewport into horizontal bands processed on
  // a thread pool.  |min_band_height| keeps tiny viewports single-threaded
  // (parallelism overhead would dominate).  The font registry must be set.
  void RasterizeParallel(const DisplayList& list, base::ThreadPool& pool, int min_band_height = 64);

  // Restricts all subsequent drawing to screen rows [y0, y1) (in addition to
  // the viewport bounds and any overflow clips).  Used for scroll blits.
  void SetVisibleBand(int y0, int y1);
  void ResetVisibleBand()
  {
    band_y0_ = 0;
    band_y1_ = height_;
  }

  // Shifts the existing pixel content by |delta| rows (positive = content
  // moves down the screen).  Rows pushed out of the buffer are dropped; rows
  // revealed at the other edge are left untouched (the caller re-rasterizes
  // them via SetVisibleBand).  Used by the GUI's scroll blit.
  void ShiftRows(int delta);

  // Uses |registry| (owned by the caller, must outlive this rasterizer) for
  // text; pass nullptr to fall back to the embedded 8x8 bitmap font.
  void SetFontRegistry(const graphics::FontRegistry* registry)
  {
    registry_ = registry;
  }

  // Scrolls the visible region: all draw commands are shifted up by |offset|
  // pixels, so content below the viewport can be panned into view.  Content
  // scrolled out of the buffer is clipped.
  void SetScrollOffset(float offset);

  int width() const
  {
    return width_;
  }
  int height() const
  {
    return height_;
  }

  // RGBA8888, row-major, height*width*4 bytes.
  const std::vector<uint8_t>& pixels() const
  {
    return pixels_;
  }

  // Text metrics for the embedded font: every glyph advances by |font_size|.
  static float CharWidth(float font_size)
  {
    return font_size;
  }
  static float TextWidth(std::string_view text, float font_size);

private:
  // A non-owning view into a band of an existing buffer, used by the parallel
  // rasterizer: each worker rasterizes a disjoint horizontal slice into
  // pixels_ via its own view (own clip stack, shared font registry).
  // |band_y0| is the absolute first row of the band; |band_height| its row
  // count.  The view reasons in full-page coordinates but writes only into
  // the band's rows (translated by band_origin_y_).
  Rasterizer(uint8_t* pixels, int width, int full_height, int band_y0, int band_height);

  // An axis-aligned clip rectangle in document coordinates (before scroll).
  struct ClipRect
  {
    float x = 0;
    float y = 0;
    float x2 = 0;
    float y2 = 0;
  };

  void FillRect(float x, float y, float width, float height, css::Color color);
  void FillRoundRect(float x, float y, float width, float height, float radius, css::Color color);
  void DrawBorder(const DrawCommand& command);
  void DrawText(const DrawCommand& command);
  void DrawText8x8(const DrawCommand& command);
  void DrawTextFreetype(const DrawCommand& command);
  void DrawImage(const DrawCommand& command);
  void BlendGlyph(int x, int y, const graphics::GlyphBitmap& glyph, css::Color color);
  // Returns true when the rect (document coordinates) is inside the active
  // clip after intersection; updates the rect in place.
  bool ApplyClip(float& x, float& y, float& width, float& height) const;
  // Intersects a screen-space y range [y0, y1) with the active visible band.
  void ClampToBand(int& y0, int& y1) const;
  // Byte offset of pixel (x, y) in screen space, translated to the local
  // buffer (band views subtract their origin).
  std::size_t BandOffset(int x, int y) const;

  bool owns_pixels_ = true;
  uint8_t* pixels_data_ = nullptr; // == pixels_.data() when owns_pixels_
  int width_ = 0;
  int height_ = 0;
  int band_y0_ = 0;
  int band_y1_ = 0;
  // Absolute first row of the local buffer within the full viewport (0 for
  // owning rasterizers; the band's first row for parallel band views).
  int band_origin_y_ = 0;
  float scroll_offset_ = 0;
  std::vector<uint8_t> pixels_;
  const graphics::FontRegistry* registry_ = nullptr;
  std::vector<ClipRect> clips_; // active overflow clips (innermost last)
};

// Writes an RGBA buffer as a binary PPM (P6) file.  Alpha is composited over
// white.  Returns an error on I/O failure.
base::Result<void> WritePpm(std::string_view path, const Rasterizer& image);

} // namespace neko::paint
