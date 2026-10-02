#include "neko/paint/rasterizer.h"

#include "neko/base/status.h"
#include "neko/base/thread_pool.h"
#include "neko/graphics/font_face.h"
#include "neko/graphics/font_registry.h"
#include "neko/graphics/font_selector.h"
#include "neko/graphics/utf8.h"
#include "neko/image/image.h"
#include "neko/paint/font8x8.h"
#include "neko/paint/ppm_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <string>
#include <string_view>
#include <vector>

namespace neko::paint {
namespace {

int Clamp(int value, int lo, int hi)
{
  return value < lo ? lo : (value > hi ? hi : value);
}

// Blends a source color onto a destination pixel using integer fixed-point
// math (the floating-point reference algorithm produces the same result up to
// rounding, without per-pixel float conversions).
void BlendPixel(uint8_t& dr, uint8_t& dg, uint8_t& db, uint8_t& da, css::Color src)
{
  const unsigned sa = src.a;
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
  dr = blend(dr, src.r);
  dg = blend(dg, src.g);
  db = blend(db, src.b);
  da = static_cast<uint8_t>(out_a);
}

// 32-bit RGBA8888 pattern for a color (little-endian byte order r,g,b,a).
std::uint32_t RgbaPattern(css::Color c)
{
  return (static_cast<std::uint32_t>(c.a) << 24) | (static_cast<std::uint32_t>(c.b) << 16) |
         (static_cast<std::uint32_t>(c.g) << 8) | static_cast<std::uint32_t>(c.r);
}

} // namespace

Rasterizer::Rasterizer(int width, int height) : width_(width), height_(height), band_y1_(height)
{
  pixels_.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
  pixels_data_ = pixels_.data();
}

Rasterizer::Rasterizer(uint8_t* pixels, int width, int full_height, int band_y0, int band_height)
    : owns_pixels_(false), pixels_data_(pixels), width_(width), height_(full_height),
      band_y0_(band_y0), band_y1_(band_y0 + band_height), band_origin_y_(band_y0)
{}

Rasterizer::Rasterizer(const Rasterizer& other)
    : owns_pixels_(other.owns_pixels_), pixels_data_(other.pixels_data_), width_(other.width_),
      height_(other.height_), band_y0_(other.band_y0_), band_y1_(other.band_y1_),
      band_origin_y_(other.band_origin_y_), scroll_offset_(other.scroll_offset_),
      pixels_(other.pixels_), registry_(other.registry_), clips_(other.clips_)
{
  if (owns_pixels_) {
    pixels_data_ = pixels_.data();
  }
}

Rasterizer& Rasterizer::operator=(const Rasterizer& other)
{
  if (this == &other) {
    return *this;
  }
  owns_pixels_ = other.owns_pixels_;
  width_ = other.width_;
  height_ = other.height_;
  band_y0_ = other.band_y0_;
  band_y1_ = other.band_y1_;
  band_origin_y_ = other.band_origin_y_;
  scroll_offset_ = other.scroll_offset_;
  registry_ = other.registry_;
  clips_ = other.clips_;
  pixels_ = other.pixels_;
  pixels_data_ = owns_pixels_ ? pixels_.data() : other.pixels_data_;
  return *this;
}

Rasterizer::Rasterizer(Rasterizer&& other) noexcept
    : owns_pixels_(other.owns_pixels_), pixels_data_(other.pixels_data_), width_(other.width_),
      height_(other.height_), band_y0_(other.band_y0_), band_y1_(other.band_y1_),
      band_origin_y_(other.band_origin_y_), scroll_offset_(other.scroll_offset_),
      pixels_(std::move(other.pixels_)), registry_(other.registry_), clips_(std::move(other.clips_))
{
  if (owns_pixels_) {
    pixels_data_ = pixels_.data();
  }
  other.pixels_data_ = other.pixels_.data();
}

Rasterizer& Rasterizer::operator=(Rasterizer&& other) noexcept
{
  if (this == &other) {
    return *this;
  }
  owns_pixels_ = other.owns_pixels_;
  width_ = other.width_;
  height_ = other.height_;
  band_y0_ = other.band_y0_;
  band_y1_ = other.band_y1_;
  band_origin_y_ = other.band_origin_y_;
  scroll_offset_ = other.scroll_offset_;
  registry_ = other.registry_;
  clips_ = std::move(other.clips_);
  pixels_ = std::move(other.pixels_);
  pixels_data_ = owns_pixels_ ? pixels_.data() : other.pixels_data_;
  other.pixels_data_ = other.pixels_.data();
  return *this;
}

void Rasterizer::Resize(int width, int height)
{
  if (width == width_ && height == height_) {
    return;
  }
  if (width < 0) {
    width = 0;
  }
  if (height < 0) {
    height = 0;
  }
  width_ = width;
  height_ = height;
  band_y0_ = 0;
  band_y1_ = height;
  pixels_.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
  pixels_data_ = pixels_.data();
}

void Rasterizer::Clear(css::Color color)
{
  const std::size_t n = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
  const std::uint32_t pattern = RgbaPattern(color);
  std::uint32_t* dst = reinterpret_cast<std::uint32_t*>(pixels_data_);
  std::fill_n(dst, n, pattern);
}

void Rasterizer::ClearBand(int y0, int y1, css::Color color)
{
  y0 = Clamp(y0, 0, height_);
  y1 = Clamp(y1, 0, height_);
  if (y1 <= y0) {
    return;
  }
  const std::uint32_t pattern = RgbaPattern(color);
  std::uint32_t* base = reinterpret_cast<std::uint32_t*>(pixels_data_);
  for (int y = y0; y < y1; ++y) {
    std::fill_n(base + static_cast<std::size_t>(y) * static_cast<std::size_t>(width_),
                static_cast<std::size_t>(width_),
                pattern);
  }
}

void Rasterizer::SetVisibleBand(int y0, int y1)
{
  band_y0_ = Clamp(y0, 0, height_);
  band_y1_ = Clamp(y1, 0, height_);
}

void Rasterizer::ShiftRows(int delta)
{
  if (delta == 0) {
    return;
  }
  const std::size_t row_bytes = static_cast<std::size_t>(width_) * 4;
  if (delta > 0) {
    // Content moves down; copy from the top of the buffer to avoid clobbering
    // sources still needed further down.
    for (int y = height_ - 1; y >= delta; --y) {
      std::memcpy(pixels_data_ + static_cast<std::size_t>(y) * row_bytes,
                  pixels_data_ + static_cast<std::size_t>(y - delta) * row_bytes,
                  row_bytes);
    }
  } else {
    const int d = -delta;
    for (int y = 0; y + d < height_; ++y) {
      std::memcpy(pixels_data_ + static_cast<std::size_t>(y) * row_bytes,
                  pixels_data_ + static_cast<std::size_t>(y + d) * row_bytes,
                  row_bytes);
    }
  }
}

void Rasterizer::ClampToBand(int& y0, int& y1) const
{
  if (band_y0_ <= 0 && band_y1_ >= height_) {
    return;
  }
  y0 = std::max(y0, band_y0_);
  y1 = std::min(y1, band_y1_);
}

bool Rasterizer::RowRangeVisible(int doc_y0, int doc_y1) const
{
  const int scroll = static_cast<int>(std::round(scroll_offset_));
  int y0 = doc_y0 - scroll;
  int y1 = doc_y1 - scroll;
  y0 = std::max(y0, 0);
  y1 = std::min(y1, height_);
  ClampToBand(y0, y1);
  return y1 > y0;
}

std::size_t Rasterizer::BandOffset(int x, int y) const
{
  return (static_cast<std::size_t>(y - band_origin_y_) * static_cast<std::size_t>(width_) +
          static_cast<std::size_t>(x)) *
         4;
}

float Rasterizer::TextWidth(std::string_view text, float font_size)
{
  return static_cast<float>(text.size()) * font_size;
}

void Rasterizer::Rasterize(const DisplayList& list)
{
  for (const DrawCommand& command : list.commands()) {
    switch (command.type) {
    case CommandType::kPushClip: {
      ClipRect clip{command.x, command.y, command.x + command.width, command.y + command.height};
      // Clips nest: the new clip is the intersection with the enclosing one.
      if (!clips_.empty()) {
        const ClipRect& c = clips_.back();
        clip.x = std::max(clip.x, c.x);
        clip.y = std::max(clip.y, c.y);
        clip.x2 = std::min(clip.x2, c.x2);
        clip.y2 = std::min(clip.y2, c.y2);
      }
      clips_.push_back(clip);
      break;
    }
    case CommandType::kPopClip:
      if (!clips_.empty()) {
        clips_.pop_back();
      }
      break;
    case CommandType::kFillRect:
      FillRect(command.x, command.y, command.width, command.height, command.color);
      break;
    case CommandType::kFillRoundRect:
      FillRoundRect(
          command.x, command.y, command.width, command.height, command.radius, command.color);
      break;
    case CommandType::kBorderRect:
      DrawBorder(command);
      break;
    case CommandType::kDrawText:
      DrawText(command);
      break;
    case CommandType::kDrawImage:
      DrawImage(command);
      break;
    }
  }
}

void Rasterizer::RasterizeParallel(const DisplayList& list,
                                   base::ThreadPool& pool,
                                   int min_band_height)
{
  const std::size_t workers = pool.thread_count();
  if (workers <= 1 || height_ < min_band_height || !owns_pixels_) {
    Rasterize(list);
    return;
  }
  // Split the viewport into horizontal bands; each pixel belongs to exactly
  // one band, so the bands rasterize independently and write disjoint rows.
  const int num_bands =
      std::min<int>(static_cast<int>(workers), std::max(1, height_ / min_band_height));
  std::vector<int> band_starts(static_cast<std::size_t>(num_bands) + 1);
  for (int i = 0; i <= num_bands; ++i) {
    band_starts[static_cast<std::size_t>(i)] = height_ * i / num_bands;
  }
  // The lowest band runs on the calling thread: the caller is about to wait
  // anyway, and this keeps the submitted task count below the worker count.
  // That matters because callers may hold a lock the workers can block on
  // (the DOM lock: a pool task injecting a subresource waits for it while the
  // frame's bands wait for workers).  One caller-run band leaves one worker
  // spare for exactly that case.
  const auto run_band = [this, &list](int y0, int y1) {
    // A band view: full-page coordinate space, writing only rows [y0, y1)
    // of the shared buffer (translated by the band's origin).
    Rasterizer view(pixels_data_ +
                        static_cast<std::size_t>(y0) * static_cast<std::size_t>(width_) * 4,
                    width_,
                    height_,
                    y0,
                    y1 - y0);
    view.registry_ = registry_;
    view.scroll_offset_ = scroll_offset_;
    // A visible band set on this rasterizer (banded screenshot rows, a
    // scroll blit's exposed strip) restricts which rows may be drawn:
    // intersect it into the view so parallel output matches what the
    // serial path writes row for row.
    view.band_y0_ = std::max(view.band_y0_, band_y0_);
    view.band_y1_ = std::min(view.band_y1_, band_y1_);
    view.Rasterize(list);
  };
  std::vector<std::future<void>> futures;
  futures.reserve(static_cast<std::size_t>(num_bands));
  run_band(band_starts[0], band_starts[1]);
  for (int i = 1; i < num_bands; ++i) {
    const int y0 = band_starts[static_cast<std::size_t>(i)];
    const int y1 = band_starts[static_cast<std::size_t>(i) + 1];
    if (y1 <= y0) {
      continue;
    }
    futures.push_back(pool.Submit([&run_band, y0, y1] { run_band(y0, y1); }));
  }
  for (std::future<void>& f : futures) {
    f.wait();
  }
}

void Rasterizer::SetScrollOffset(float offset)
{
  scroll_offset_ = offset;
}

bool Rasterizer::ApplyClip(float& x, float& y, float& width, float& height) const
{
  if (clips_.empty()) {
    return true;
  }
  const ClipRect& c = clips_.back();
  const float nx = std::max(x, c.x);
  const float ny = std::max(y, c.y);
  const float nx2 = std::min(x + width, c.x2);
  const float ny2 = std::min(y + height, c.y2);
  if (nx2 <= nx || ny2 <= ny) {
    return false;
  }
  x = nx;
  y = ny;
  width = nx2 - nx;
  height = ny2 - ny;
  return true;
}

void Rasterizer::FillRect(float x, float y, float width, float height, css::Color color)
{
  // Intersect with the active overflow clip (document coordinates; the
  // scroll offset is applied after clipping).
  if (!ApplyClip(x, y, width, height)) {
    return;
  }
  // Shift up by the scroll offset, then clip the span to the buffer.  Note
  // this is a *clip*, not a clamp of both endpoints: content entirely below
  // the buffer (the common case for the banded screenshot path, where the
  // buffer is one band tall) must be discarded, not squashed onto the last
  // row.
  y -= scroll_offset_;
  const int x0 = std::max(0, static_cast<int>(std::floor(x)));
  const int y0 = std::max(0, static_cast<int>(std::floor(y)));
  const int x1 = std::min(width_, static_cast<int>(std::ceil(x + width)));
  const int y1 = std::min(height_, static_cast<int>(std::ceil(y + height)));
  if (x0 >= x1 || y0 >= y1) {
    return;
  }
  int band_y0 = y0;
  int band_y1 = y1;
  ClampToBand(band_y0, band_y1);
  for (int py = band_y0; py < band_y1; ++py) {
    for (int px = x0; px < x1; ++px) {
      const std::size_t offset = BandOffset(px, py);
      BlendPixel(pixels_data_[offset],
                 pixels_data_[offset + 1],
                 pixels_data_[offset + 2],
                 pixels_data_[offset + 3],
                 color);
    }
  }
}

void Rasterizer::FillRoundRect(
    float x, float y, float width, float height, float radius, css::Color color)
{
  if (radius <= 0) {
    FillRect(x, y, width, height, color);
    return;
  }
  // Shift up by the scroll offset, then clip the span to the buffer (a clip,
  // not an endpoint clamp -- see FillRect).
  y -= scroll_offset_;
  radius = std::min(radius, std::min(width, height) / 2.0f);
  const int x0 = std::max(0, static_cast<int>(std::floor(x)));
  const int y0 = std::max(0, static_cast<int>(std::floor(y)));
  const int x1 = std::min(width_, static_cast<int>(std::ceil(x + width)));
  const int y1 = std::min(height_, static_cast<int>(std::ceil(y + height)));
  if (x0 >= x1 || y0 >= y1) {
    return;
  }
  const float left = x + radius;
  const float right = x + width - radius;
  const float top = y + radius;
  const float bottom = y + height - radius;
  int band_y0 = y0;
  int band_y1 = y1;
  ClampToBand(band_y0, band_y1);
  for (int py = band_y0; py < band_y1; ++py) {
    for (int px = x0; px < x1; ++px) {
      // Pixel centre: inside the rounded region unless it falls in a corner
      // outside the corner's quarter circle.
      const float cx = static_cast<float>(px) + 0.5f;
      const float cy = static_cast<float>(py) + 0.5f;
      bool inside = true;
      if (cx < left && cy < top) {
        const float dx = cx - left;
        const float dy = cy - top;
        inside = dx * dx + dy * dy <= radius * radius;
      } else if (cx > right && cy < top) {
        const float dx = cx - right;
        const float dy = cy - top;
        inside = dx * dx + dy * dy <= radius * radius;
      } else if (cx < left && cy > bottom) {
        const float dx = cx - left;
        const float dy = cy - bottom;
        inside = dx * dx + dy * dy <= radius * radius;
      } else if (cx > right && cy > bottom) {
        const float dx = cx - right;
        const float dy = cy - bottom;
        inside = dx * dx + dy * dy <= radius * radius;
      }
      if (!inside) {
        continue;
      }
      const std::size_t offset = BandOffset(px, py);
      BlendPixel(pixels_data_[offset],
                 pixels_data_[offset + 1],
                 pixels_data_[offset + 2],
                 pixels_data_[offset + 3],
                 color);
    }
  }
}

void Rasterizer::DrawBorder(const DrawCommand& command)
{
  const float right = command.width - command.border_right;
  const float bottom = command.height - command.border_bottom;
  FillRect(command.x, command.y, command.width, command.border_top, command.color);
  FillRect(command.x,
           command.y + command.height - command.border_bottom,
           command.width,
           command.border_bottom,
           command.color);
  FillRect(command.x, command.y, command.border_left, command.height, command.color);
  FillRect(command.x + right, command.y, command.border_right, command.height, command.color);
  (void)bottom;
}

void Rasterizer::DrawText(const DrawCommand& command)
{
  if (registry_ == nullptr) {
    DrawText8x8(command);
    return;
  }
  DrawTextFreetype(command);
}

void Rasterizer::DrawText8x8(const DrawCommand& command)
{
  const float scale = command.font_size / 8.0f;
  const int start_x = static_cast<int>(std::round(command.x));
  const int start_y = static_cast<int>(std::round(command.y));
  const int cell = static_cast<int>(std::ceil(scale));
  const int step = static_cast<int>(std::round(command.font_size));
  if (!RowRangeVisible(start_y - 1, start_y + 8 * cell + 2)) {
    return;
  }

  for (std::size_t i = 0; i < command.text.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(command.text[i]);
    if (ch < 32 || ch > 126) {
      continue; // non-ASCII / control: not in the embedded font
    }
    const uint8_t* glyph = detail::kFont8x8[ch - 32];
    const int glyph_x = start_x + static_cast<int>(i) * step;
    for (int row = 0; row < 8; ++row) {
      for (int col = 0; col < 8; ++col) {
        if ((glyph[row] & (0x01 << col)) == 0) {
          continue;
        }
        FillRect(static_cast<float>(glyph_x + col * cell),
                 static_cast<float>(start_y + row * cell),
                 static_cast<float>(cell),
                 static_cast<float>(cell),
                 command.text_color);
      }
    }
  }

  if (command.underline) {
    const int text_width = step * static_cast<int>(command.text.size());
    const int thickness = std::max(1, cell / 2);
    FillRect(static_cast<float>(start_x),
             static_cast<float>(start_y + 8 * cell - thickness),
             static_cast<float>(text_width),
             static_cast<float>(thickness),
             command.text_color);
  }
}

void Rasterizer::DrawTextFreetype(const DrawCommand& command)
{
  // Skip runs that cannot write a row here, before any registry work: the
  // selector lookup and the font metrics each take a lock that is shared by
  // every pool thread, and a band view walks the whole display list although
  // it owns one strip.  Paying those locks per band serialized the pool
  // (measured: parallel raster of a text-heavy page burned ~10x the serial
  // CPU).  The bound uses only the font size so the check itself is free: the
  // em box extended by one more font size below the top covers ascent,
  // descent and the underline; an extra row above covers overshoot.
  const int doc_top = static_cast<int>(std::floor(command.y)) - 1;
  const int doc_bottom = static_cast<int>(std::ceil(command.y + 2.0f * command.font_size)) + 2;
  if (!RowRangeVisible(doc_top, doc_bottom)) {
    return;
  }
  const graphics::FontSelector* selector =
      registry_->SelectorFor(command.font_family, command.font_weight, command.font_italic);
  if (selector == nullptr) {
    return;
  }
  std::vector<uint32_t> code_points;
  graphics::DecodeUtf8(command.text, code_points);

  const float baseline = command.y + selector->Ascent(command.font_size);
  float pen_x = command.x;
  float total_width = 0;
  for (const uint32_t code_point : code_points) {
    const std::optional<graphics::RasterizedGlyph> glyph =
        selector->RenderGlyph(code_point, command.font_size);
    if (!glyph.has_value()) {
      pen_x += command.font_size * 0.5f;
      continue;
    }
    BlendGlyph(static_cast<int>(std::round(pen_x)) + glyph->glyph.left,
               static_cast<int>(std::round(baseline)) - glyph->glyph.top,
               glyph->glyph,
               command.text_color);
    pen_x += glyph->glyph.advance;
    total_width += glyph->glyph.advance;
  }

  if (command.underline) {
    const float thickness = std::max(1.0f, command.font_size / 12.0f);
    FillRect(command.x, baseline + 1.0f, total_width, thickness, command.text_color);
  }
}

void Rasterizer::DrawImage(const DrawCommand& command)
{
  const image::Image& img = *command.image;
  if (img.empty()) {
    return;
  }
  const float iw = static_cast<float>(img.width);
  const float ih = static_cast<float>(img.height);
  if (iw <= 0 || ih <= 0) {
    return;
  }
  const float box_w = command.width;
  const float box_h = command.height;
  // Concrete object size per object-fit (CSS Images 3 §4.5).
  float dst_w = 0;
  float dst_h = 0;
  switch (command.object_fit) {
  case style::ObjectFit::kFill:
    dst_w = box_w;
    dst_h = box_h;
    break;
  case style::ObjectFit::kContain: {
    const float scale = std::min(box_w / iw, box_h / ih);
    dst_w = iw * scale;
    dst_h = ih * scale;
    break;
  }
  case style::ObjectFit::kCover: {
    const float scale = std::max(box_w / iw, box_h / ih);
    dst_w = iw * scale;
    dst_h = ih * scale;
    break;
  }
  case style::ObjectFit::kNone:
    dst_w = iw;
    dst_h = ih;
    break;
  case style::ObjectFit::kScaleDown: {
    const float scale = std::min(1.0f, std::min(box_w / iw, box_h / ih));
    dst_w = iw * scale;
    dst_h = ih * scale;
    break;
  }
  }
  if (dst_w <= 0 || dst_h <= 0) {
    return;
  }
  const float dst_x = command.x + (box_w - dst_w) / 2.0f;
  const float dst_y = command.y + (box_h - dst_h) / 2.0f - scroll_offset_;
  // ceil so the sampled source coordinate (px - dst_x)/dst_w never goes
  // negative (fractional origins from layout otherwise index rgba at -1).
  int x0 = std::max(0, static_cast<int>(std::ceil(dst_x)));
  int y0 = std::max(0, static_cast<int>(std::ceil(dst_y)));
  int x1 = std::min(width_, static_cast<int>(std::ceil(dst_x + dst_w)));
  int y1 = std::min(height_, static_cast<int>(std::ceil(dst_y + dst_h)));
  // Clip the destination rect to the active overflow clip (document coords:
  // the pixel at row |py| sits at document y = py + scroll).
  if (!clips_.empty()) {
    const ClipRect& c = clips_.back();
    x0 = std::max(x0, static_cast<int>(std::ceil(c.x)));
    x1 = std::min(x1, static_cast<int>(std::ceil(c.x2)));
    y0 = std::max(y0, static_cast<int>(std::ceil(c.y - scroll_offset_)));
    y1 = std::min(y1, static_cast<int>(std::ceil(c.y2 - scroll_offset_)));
    if (x0 >= x1 || y0 >= y1) {
      return;
    }
  }
  ClampToBand(y0, y1);
  if (x0 >= x1 || y0 >= y1) {
    return;
  }
  for (int py = y0; py < y1; ++py) {
    const int src_y =
        std::min(img.height - 1, static_cast<int>((static_cast<float>(py) - dst_y) / dst_h * ih));
    const uint8_t* row =
        img.rgba.data() + static_cast<std::size_t>(src_y) * static_cast<std::size_t>(img.width) * 4;
    for (int px = x0; px < x1; ++px) {
      const int src_x =
          std::min(img.width - 1, static_cast<int>((static_cast<float>(px) - dst_x) / dst_w * iw));
      const std::size_t so = static_cast<std::size_t>(src_x) * 4;
      const css::Color pixel{row[so], row[so + 1], row[so + 2], row[so + 3]};
      const std::size_t offset = BandOffset(px, py);
      BlendPixel(pixels_data_[offset],
                 pixels_data_[offset + 1],
                 pixels_data_[offset + 2],
                 pixels_data_[offset + 3],
                 pixel);
    }
  }
}

void Rasterizer::BlendGlyph(int x, int y, const graphics::GlyphBitmap& glyph, css::Color color)
{
  const int scroll = static_cast<int>(std::round(scroll_offset_));
  // |y| is in document coordinates; convert to screen space for clipping.
  const int screen_top = y - scroll;
  int py0 = std::max(0, screen_top);
  int py1 = std::min(height_, screen_top + glyph.height);
  ClampToBand(py0, py1);
  for (int row = py0; row < py1; ++row) {
    const int py = row;
    const uint8_t* src = glyph.data + static_cast<std::size_t>(row - screen_top) *
                                          static_cast<std::size_t>(glyph.pitch);
    for (int col = 0; col < glyph.width; ++col) {
      const int px = x + col;
      if (px < 0 || px >= width_) {
        continue;
      }
      // Respect the active overflow clip: the pixel's document coordinates
      // are (px, py + scroll).
      if (!clips_.empty()) {
        const ClipRect& c = clips_.back();
        const float doc_x = static_cast<float>(px);
        const float doc_y = static_cast<float>(py) + scroll_offset_;
        if (doc_x < c.x || doc_x >= c.x2 || doc_y < c.y || doc_y >= c.y2) {
          continue;
        }
      }
      const uint8_t alpha = src[col];
      if (alpha == 0) {
        continue;
      }
      const css::Color shaded{
          color.r, color.g, color.b, static_cast<uint8_t>((color.a * alpha) / 255)};
      const std::size_t offset = BandOffset(px, py);
      BlendPixel(pixels_data_[offset],
                 pixels_data_[offset + 1],
                 pixels_data_[offset + 2],
                 pixels_data_[offset + 3],
                 shaded);
    }
  }
}

base::Result<void> WritePpm(std::string_view path, const Rasterizer& image)
{
  auto writer = PpmWriter::Create(path, image.width(), image.height());
  if (!writer.has_value()) {
    return base::Err(writer.error());
  }
  const auto written = writer.value().AppendRows(image.pixels().data(), image.height());
  if (!written.has_value()) {
    return written;
  }
  return writer.value().Finish();
}

} // namespace neko::paint
