#include "neko/compositor/surface.h"

#include <algorithm>
#include <cstring>

namespace neko::compositor {

namespace {

// Byte offset of a pixel in the RGBA buffer.
std::size_t PixelOffset(int x, int y, int width)
{
  return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
          static_cast<std::size_t>(x)) *
         4;
}

// Blends a source pixel (straight RGBA, already scaled by opacity) over a
// destination pixel.  Integer fixed-point math, identical to
// paint::Rasterizer::BlendPixel so composited output matches rasterized
// output up to rounding.
void BlendPixel(uint8_t& dr,
                uint8_t& dg,
                uint8_t& db,
                uint8_t& da,
                uint8_t sr,
                uint8_t sg,
                uint8_t sb,
                uint8_t sa)
{
  if (sa == 0) {
    return;
  }
  const unsigned da_val = da;
  const unsigned out_a = sa + ((da_val * (255 - sa) + 127) / 255);
  if (out_a == 0) {
    return;
  }
  auto blend = [&](uint8_t dst, uint8_t s) -> uint8_t {
    const unsigned s_part = static_cast<unsigned>(s) * sa;
    const unsigned d_part = (static_cast<unsigned>(dst) * da_val * (255 - sa) + 127) / 255;
    return static_cast<uint8_t>((s_part + d_part) / out_a);
  };
  dr = blend(dr, sr);
  dg = blend(dg, sg);
  db = blend(db, sb);
  da = static_cast<uint8_t>(out_a);
}

} // namespace

Surface::Surface(int width, int height)
{
  Resize(width, height);
}

void Surface::Resize(int width, int height)
{
  width_ = std::max(0, width);
  height_ = std::max(0, height);
  const std::size_t bytes =
      static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4;
  if (pixels_.size() < bytes) {
    pixels_.resize(bytes);
  }
}

void Surface::Clear(css::Color color)
{
  if (pixels_.empty()) {
    return;
  }
  const std::uint32_t pattern =
      (static_cast<std::uint32_t>(color.a) << 24) | (static_cast<std::uint32_t>(color.b) << 16) |
      (static_cast<std::uint32_t>(color.g) << 8) | static_cast<std::uint32_t>(color.r);
  const std::size_t pixel_count =
      static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
  std::uint32_t* words = reinterpret_cast<std::uint32_t*>(pixels_.data());
  for (std::size_t i = 0; i < pixel_count; ++i) {
    words[i] = pattern;
  }
}

void Surface::CopyFrom(const Surface& src, int dx, int dy)
{
  CopyRect(src, 0, 0, src.width_, src.height_, dx, dy);
}

void Surface::CopyBand(const Surface& src, int y0, int y1, int dx)
{
  if (src.width_ <= 0 || src.height_ <= 0 || width_ <= 0 || height_ <= 0) {
    return;
  }
  y0 = std::clamp(y0, 0, src.height_);
  y1 = std::clamp(y1, y0, src.height_);
  y1 = std::min(y1, height_);
  if (y1 <= y0) {
    return;
  }
  const int x0 = std::max(0, dx);
  const int x1 = std::min(width_, dx + src.width_);
  if (x1 <= x0) {
    return;
  }
  const std::size_t row_bytes = static_cast<std::size_t>(x1 - x0) * 4;
  for (int y = y0; y < y1; ++y) {
    std::memcpy(pixels_.data() + PixelOffset(x0, y, width_),
                src.pixels_.data() + PixelOffset(x0 - dx, y, src.width_),
                row_bytes);
  }
}

void Surface::CopyRect(const Surface& src, int sx, int sy, int w, int h, int dx, int dy)
{
  if (src.width_ <= 0 || src.height_ <= 0 || width_ <= 0 || height_ <= 0 || w <= 0 || h <= 0) {
    return;
  }
  // Clip the source rectangle to the source surface.
  const int src_x0 = std::max(0, sx);
  const int src_y0 = std::max(0, sy);
  const int src_x1 = std::min(src.width_, sx + w);
  const int src_y1 = std::min(src.height_, sy + h);
  if (src_x1 <= src_x0 || src_y1 <= src_y0) {
    return;
  }
  // The destination rectangle follows from the clipped source.
  const int dst_ox = dx + (src_x0 - sx);
  const int dst_oy = dy + (src_y0 - sy);
  const int dst_w = src_x1 - src_x0;
  const int dst_h = src_y1 - src_y0;
  // Clip the destination rectangle to this surface.
  const int dst_x0 = std::max(0, dst_ox);
  const int dst_y0 = std::max(0, dst_oy);
  const int dst_x1 = std::min(width_, dst_ox + dst_w);
  const int dst_y1 = std::min(height_, dst_oy + dst_h);
  if (dst_x1 <= dst_x0 || dst_y1 <= dst_y0) {
    return;
  }
  const int src_start_x = src_x0 + (dst_x0 - dst_ox);
  const int src_start_y = src_y0 + (dst_y0 - dst_oy);
  const std::size_t row_bytes = static_cast<std::size_t>(dst_x1 - dst_x0) * 4;
  for (int row = 0; row < dst_y1 - dst_y0; ++row) {
    std::memcpy(pixels_.data() + PixelOffset(dst_x0, dst_y0 + row, width_),
                src.pixels_.data() + PixelOffset(src_start_x, src_start_y + row, src.width_),
                row_bytes);
  }
}

void Surface::CopyPixels(const uint8_t* rgba, int src_width, int src_height, int dx, int dy)
{
  if (rgba == nullptr || src_width <= 0 || src_height <= 0 || width_ <= 0 || height_ <= 0) {
    return;
  }
  const int dst_x0 = std::max(0, dx);
  const int dst_y0 = std::max(0, dy);
  const int dst_x1 = std::min(width_, dx + src_width);
  const int dst_y1 = std::min(height_, dy + src_height);
  if (dst_x1 <= dst_x0 || dst_y1 <= dst_y0) {
    return;
  }
  const std::size_t row_bytes = static_cast<std::size_t>(dst_x1 - dst_x0) * 4;
  for (int row = 0; row < dst_y1 - dst_y0; ++row) {
    const std::size_t src_row =
        (static_cast<std::size_t>(dst_y0 + row - dy) * static_cast<std::size_t>(src_width) +
         static_cast<std::size_t>(dst_x0 - dx)) *
        4;
    std::memcpy(
        pixels_.data() + PixelOffset(dst_x0, dst_y0 + row, width_), rgba + src_row, row_bytes);
  }
}

void Surface::CopyPixelsBand(const uint8_t* rgba, int src_width, int y0, int y1, int dx)
{
  if (rgba == nullptr || src_width <= 0 || width_ <= 0 || height_ <= 0) {
    return;
  }
  y0 = std::clamp(y0, 0, height_);
  y1 = std::clamp(y1, y0, height_);
  if (y1 <= y0) {
    return;
  }
  const int x0 = std::max(0, dx);
  const int x1 = std::min(width_, dx + src_width);
  if (x1 <= x0) {
    return;
  }
  const std::size_t row_bytes = static_cast<std::size_t>(x1 - x0) * 4;
  for (int y = y0; y < y1; ++y) {
    const std::size_t src_row = (static_cast<std::size_t>(y) * static_cast<std::size_t>(src_width) +
                                 static_cast<std::size_t>(x0 - dx)) *
                                4;
    std::memcpy(pixels_.data() + PixelOffset(x0, y, width_), rgba + src_row, row_bytes);
  }
}

void Surface::BlendOver(
    const Surface& src, int dx, int dy, float opacity, int cx, int cy, int cw, int ch)
{
  if (src.width_ <= 0 || src.height_ <= 0 || width_ <= 0 || height_ <= 0) {
    return;
  }
  // Resolve the clip rect (defaults to the whole destination surface).
  if (cw < 0) {
    cx = 0;
    cw = width_;
  }
  if (ch < 0) {
    cy = 0;
    ch = height_;
  }
  const int clip_x0 = std::clamp(cx, 0, width_);
  const int clip_y0 = std::clamp(cy, 0, height_);
  const int clip_x1 = std::clamp(cx + cw, 0, width_);
  const int clip_y1 = std::clamp(cy + ch, 0, height_);
  // Intersection of the placed source and the clip rect.
  const int x0 = std::max(clip_x0, dx);
  const int y0 = std::max(clip_y0, dy);
  const int x1 = std::min(clip_x1, dx + src.width_);
  const int y1 = std::min(clip_y1, dy + src.height_);
  if (x1 <= x0 || y1 <= y0) {
    return;
  }
  // Pre-scale the opacity (0..1) into the 0..255 source-alpha range.
  const unsigned opacity_a = static_cast<unsigned>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f);
  if (opacity_a == 0) {
    return;
  }
  for (int y = y0; y < y1; ++y) {
    const std::size_t dst_base = PixelOffset(x0, y, width_);
    const std::size_t src_base = PixelOffset(x0 - dx, y - dy, src.width_);
    for (int x = x0; x < x1; ++x) {
      const std::size_t src_i = src_base + static_cast<std::size_t>(x - x0) * 4;
      const std::size_t dst_i = dst_base + static_cast<std::size_t>(x - x0) * 4;
      const uint8_t sa =
          static_cast<uint8_t>((static_cast<unsigned>(src.pixels_[src_i + 3]) * opacity_a) / 255);
      BlendPixel(pixels_[dst_i],
                 pixels_[dst_i + 1],
                 pixels_[dst_i + 2],
                 pixels_[dst_i + 3],
                 src.pixels_[src_i],
                 src.pixels_[src_i + 1],
                 src.pixels_[src_i + 2],
                 sa);
    }
  }
}

void Surface::ShiftRows(int delta)
{
  if (delta == 0 || width_ <= 0 || height_ <= 0) {
    return;
  }
  const std::size_t row_bytes = static_cast<std::size_t>(width_) * 4;
  if (delta > 0) {
    // Content moves down; copy from the top of the buffer to avoid
    // clobbering sources still needed further down.
    for (int y = height_ - 1; y >= delta; --y) {
      std::memcpy(pixels_.data() + static_cast<std::size_t>(y) * row_bytes,
                  pixels_.data() + static_cast<std::size_t>(y - delta) * row_bytes,
                  row_bytes);
    }
  } else {
    const int d = -delta;
    for (int y = 0; y + d < height_; ++y) {
      std::memcpy(pixels_.data() + static_cast<std::size_t>(y) * row_bytes,
                  pixels_.data() + static_cast<std::size_t>(y + d) * row_bytes,
                  row_bytes);
    }
  }
}

} // namespace neko::compositor
