#pragma once

#include "neko/css/color.h"

#include <cstdint>
#include <vector>

namespace neko::compositor {

// An owned RGBA8888 pixel buffer with blit/scroll operations.  Surfaces are
// the currency of the compositor: rasterized content is copied into layer
// surfaces, layers are blended over each other, and the compositor's output
// surface is what the UI presents.
//
// Blending mirrors paint::Rasterizer's integer fixed-point math (straight
// alpha, "over" operator) so composited pixels match rasterized ones up to
// rounding.
//
// Threading: confined to a single thread (the GUI thread in practice).  The
// buffer is reused across Resize() calls (capacity kept) so repaints do not
// reallocate.
class Surface
{
public:
  Surface() = default;
  Surface(int width, int height);

  // Re-sizes the buffer, reusing existing storage when the new size fits.
  void Resize(int width, int height);

  // Opaque overwrite of the whole buffer (no blending).
  void Clear(css::Color color);

  // Straight (non-blended) copies.  All are clipped to both surfaces.
  void CopyFrom(const Surface& src, int dx = 0, int dy = 0);
  void CopyBand(const Surface& src, int y0, int y1, int dx = 0);
  void CopyRect(const Surface& src, int sx, int sy, int w, int h, int dx, int dy);

  // Raw-buffer variants used to ingest rasterizer output directly (avoids an
  // intermediate Surface).  |rgba| is an RGBA8888 row-major buffer of
  // src_width x src_height pixels.
  void CopyPixels(const uint8_t* rgba, int src_width, int src_height, int dx = 0, int dy = 0);
  void CopyPixelsBand(const uint8_t* rgba, int src_width, int y0, int y1, int dx = 0);

  // Alpha-blends |src| over this surface at (dx, dy).  |opacity| (0..1)
  // scales the source alpha.  When the clip rect is narrower than this
  // surface, only pixels inside [cx, cx+cw) x [cy, cy+ch) are touched.
  void BlendOver(const Surface& src,
                 int dx,
                 int dy,
                 float opacity = 1.0f,
                 int cx = 0,
                 int cy = 0,
                 int cw = -1,
                 int ch = -1);

  // Scroll blit: shifts rows by |delta| (positive = content moves down the
  // screen).  Rows pushed out are dropped; revealed rows are left untouched
  // (the caller refills them from the layer below).
  void ShiftRows(int delta);

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
  std::vector<uint8_t>& pixels()
  {
    return pixels_;
  }

private:
  int width_ = 0;
  int height_ = 0;
  std::vector<uint8_t> pixels_;
};

} // namespace neko::compositor
