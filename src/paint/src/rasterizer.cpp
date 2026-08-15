#include "neko/paint/rasterizer.h"

#include "neko/base/status.h"
#include "neko/graphics/font_face.h"
#include "neko/graphics/font_registry.h"
#include "neko/graphics/font_selector.h"
#include "neko/graphics/utf8.h"
#include "neko/image/image.h"
#include "neko/paint/font8x8.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace neko::paint {
namespace {

int Clamp(int value, int lo, int hi)
{
  return value < lo ? lo : (value > hi ? hi : value);
}

// Byte offset of a pixel in the RGBA buffer.
std::size_t PixelOffset(int x, int y, int width)
{
  return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
          static_cast<std::size_t>(x)) *
         4;
}

// Blends a source color onto a destination pixel.
void BlendPixel(uint8_t& dr, uint8_t& dg, uint8_t& db, uint8_t& da, css::Color src)
{
  const float sa = static_cast<float>(src.a) / 255.0f;
  const float da_float = static_cast<float>(da) / 255.0f;
  const float out_a = sa + da_float * (1.0f - sa);
  if (out_a <= 0.0f) {
    return;
  }
  auto blend = [&](uint8_t dst, uint8_t s) -> uint8_t {
    return static_cast<uint8_t>(
        (static_cast<float>(s) * sa + static_cast<float>(dst) * da_float * (1.0f - sa)) / out_a +
        0.5f);
  };
  dr = blend(dr, src.r);
  dg = blend(dg, src.g);
  db = blend(db, src.b);
  da = static_cast<uint8_t>(out_a * 255.0f + 0.5f);
}

} // namespace

Rasterizer::Rasterizer(int width, int height) : width_(width), height_(height)
{
  pixels_.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
}

void Rasterizer::Clear(css::Color color)
{
  for (int y = 0; y < height_; ++y) {
    for (int x = 0; x < width_; ++x) {
      const std::size_t offset = PixelOffset(x, y, width_);
      pixels_[offset] = color.r;
      pixels_[offset + 1] = color.g;
      pixels_[offset + 2] = color.b;
      pixels_[offset + 3] = color.a;
    }
  }
}

float Rasterizer::TextWidth(std::string_view text, float font_size)
{
  return static_cast<float>(text.size()) * font_size;
}

void Rasterizer::Rasterize(const DisplayList& list)
{
  for (const DrawCommand& command : list.commands()) {
    switch (command.type) {
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

void Rasterizer::SetScrollOffset(float offset)
{
  scroll_offset_ = offset;
}

void Rasterizer::FillRect(float x, float y, float width, float height, css::Color color)
{
  // Shift up by the scroll offset; scrolled-out content is clipped by Clamp.
  y -= scroll_offset_;
  const int x0 = Clamp(static_cast<int>(std::floor(x)), 0, width_ - 1);
  const int y0 = Clamp(static_cast<int>(std::floor(y)), 0, height_ - 1);
  const int x1 = Clamp(static_cast<int>(std::ceil(x + width)), 0, width_);
  const int y1 = Clamp(static_cast<int>(std::ceil(y + height)), 0, height_);
  for (int py = y0; py < y1; ++py) {
    for (int px = x0; px < x1; ++px) {
      const std::size_t offset = PixelOffset(px, py, width_);
      BlendPixel(
          pixels_[offset], pixels_[offset + 1], pixels_[offset + 2], pixels_[offset + 3], color);
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
  // Shift up by the scroll offset; scrolled-out content is clipped by Clamp.
  y -= scroll_offset_;
  radius = std::min(radius, std::min(width, height) / 2.0f);
  const int x0 = Clamp(static_cast<int>(std::floor(x)), 0, width_ - 1);
  const int y0 = Clamp(static_cast<int>(std::floor(y)), 0, height_ - 1);
  const int x1 = Clamp(static_cast<int>(std::ceil(x + width)), 0, width_);
  const int y1 = Clamp(static_cast<int>(std::ceil(y + height)), 0, height_);
  const float left = x + radius;
  const float right = x + width - radius;
  const float top = y + radius;
  const float bottom = y + height - radius;
  for (int py = y0; py < y1; ++py) {
    for (int px = x0; px < x1; ++px) {
      // Pixel centre: inside the rounded region unless it falls in a corner
      // outside the corner's quarter circle.
      const float cx = px + 0.5f;
      const float cy = py + 0.5f;
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
      const std::size_t offset = PixelOffset(px, py, width_);
      BlendPixel(
          pixels_[offset], pixels_[offset + 1], pixels_[offset + 2], pixels_[offset + 3], color);
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
    const graphics::GlyphBitmap* glyph = selector->RenderGlyph(code_point, command.font_size);
    if (glyph == nullptr) {
      pen_x += command.font_size * 0.5f;
      continue;
    }
    BlendGlyph(static_cast<int>(std::round(pen_x)) + glyph->left,
               static_cast<int>(std::round(baseline)) - glyph->top,
               *glyph,
               command.text_color);
    pen_x += glyph->advance;
    total_width += glyph->advance;
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
  const int x0 = std::max(0, static_cast<int>(std::ceil(dst_x)));
  const int y0 = std::max(0, static_cast<int>(std::ceil(dst_y)));
  const int x1 = std::min(width_, static_cast<int>(std::ceil(dst_x + dst_w)));
  const int y1 = std::min(height_, static_cast<int>(std::ceil(dst_y + dst_h)));
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
      const std::size_t offset = PixelOffset(px, py, width_);
      BlendPixel(
          pixels_[offset], pixels_[offset + 1], pixels_[offset + 2], pixels_[offset + 3], pixel);
    }
  }
}

void Rasterizer::BlendGlyph(int x, int y, const graphics::GlyphBitmap& glyph, css::Color color)
{
  const int scroll = static_cast<int>(std::round(scroll_offset_));
  for (int row = 0; row < glyph.height; ++row) {
    const int py = y + row - scroll;
    if (py < 0 || py >= height_) {
      continue;
    }
    const uint8_t* src =
        glyph.data + static_cast<std::size_t>(row) * static_cast<std::size_t>(glyph.pitch);
    for (int col = 0; col < glyph.width; ++col) {
      const int px = x + col;
      if (px < 0 || px >= width_) {
        continue;
      }
      const uint8_t alpha = src[col];
      if (alpha == 0) {
        continue;
      }
      const css::Color shaded{
          color.r, color.g, color.b, static_cast<uint8_t>((color.a * alpha) / 255)};
      const std::size_t offset = PixelOffset(px, py, width_);
      BlendPixel(
          pixels_[offset], pixels_[offset + 1], pixels_[offset + 2], pixels_[offset + 3], shaded);
    }
  }
}

base::Result<void> WritePpm(std::string_view path, const Rasterizer& image)
{
  std::ofstream out(std::string(path), std::ios::binary);
  if (!out.is_open()) {
    return base::Err(base::Error::Io("cannot open output file: " + std::string(path)));
  }
  out << "P6\n" << image.width() << " " << image.height() << "\n255\n";
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const std::size_t offset = PixelOffset(x, y, image.width());
      const uint8_t r = image.pixels()[offset];
      const uint8_t g = image.pixels()[offset + 1];
      const uint8_t b = image.pixels()[offset + 2];
      const uint8_t a = image.pixels()[offset + 3];
      // Composite over white.
      const float alpha = static_cast<float>(a) / 255.0f;
      auto composite = [alpha](uint8_t c) -> uint8_t {
        return static_cast<uint8_t>(static_cast<float>(c) * alpha + 255.0f * (1.0f - alpha) + 0.5f);
      };
      out.put(static_cast<char>(composite(r)));
      out.put(static_cast<char>(composite(g)));
      out.put(static_cast<char>(composite(b)));
    }
  }
  if (!out.good()) {
    return base::Err(base::Error::Io("write failed: " + std::string(path)));
  }
  return base::Ok();
}

} // namespace neko::paint
