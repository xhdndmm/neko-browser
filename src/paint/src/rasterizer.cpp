#include "neko/paint/rasterizer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "neko/base/status.h"
#include "neko/paint/font8x8.h"

namespace neko::paint {
namespace {

int Clamp(int value, int lo, int hi) { return value < lo ? lo : (value > hi ? hi : value); }

// Byte offset of a pixel in the RGBA buffer.
std::size_t PixelOffset(int x, int y, int width) {
  return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
          static_cast<std::size_t>(x)) *
         4;
}

// Blends a source color onto a destination pixel.
void BlendPixel(uint8_t& dr, uint8_t& dg, uint8_t& db, uint8_t& da, css::Color src) {
  const float sa = static_cast<float>(src.a) / 255.0f;
  const float da_float = static_cast<float>(da) / 255.0f;
  const float out_a = sa + da_float * (1.0f - sa);
  if (out_a <= 0.0f) {
    return;
  }
  auto blend = [&](uint8_t dst, uint8_t s) -> uint8_t {
    return static_cast<uint8_t>((static_cast<float>(s) * sa +
                                 static_cast<float>(dst) * da_float * (1.0f - sa)) /
                                    out_a +
                                0.5f);
  };
  dr = blend(dr, src.r);
  dg = blend(dg, src.g);
  db = blend(db, src.b);
  da = static_cast<uint8_t>(out_a * 255.0f + 0.5f);
}

}  // namespace

Rasterizer::Rasterizer(int width, int height) : width_(width), height_(height) {
  pixels_.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
}

void Rasterizer::Clear(css::Color color) {
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

float Rasterizer::TextWidth(std::string_view text, float font_size) {
  return static_cast<float>(text.size()) * font_size;
}

void Rasterizer::Rasterize(const DisplayList& list) {
  for (const DrawCommand& command : list.commands()) {
    switch (command.type) {
      case CommandType::kFillRect:
        FillRect(command.x, command.y, command.width, command.height, command.color);
        break;
      case CommandType::kBorderRect:
        DrawBorder(command);
        break;
      case CommandType::kDrawText:
        DrawText(command);
        break;
    }
  }
}

void Rasterizer::SetScrollOffset(float offset) { scroll_offset_ = offset; }

void Rasterizer::FillRect(float x, float y, float width, float height, css::Color color) {
  // Shift up by the scroll offset; scrolled-out content is clipped by Clamp.
  y -= scroll_offset_;
  const int x0 = Clamp(static_cast<int>(std::floor(x)), 0, width_ - 1);
  const int y0 = Clamp(static_cast<int>(std::floor(y)), 0, height_ - 1);
  const int x1 = Clamp(static_cast<int>(std::ceil(x + width)), 0, width_);
  const int y1 = Clamp(static_cast<int>(std::ceil(y + height)), 0, height_);
  for (int py = y0; py < y1; ++py) {
    for (int px = x0; px < x1; ++px) {
      const std::size_t offset = PixelOffset(px, py, width_);
      BlendPixel(pixels_[offset], pixels_[offset + 1], pixels_[offset + 2], pixels_[offset + 3],
                 color);
    }
  }
}

void Rasterizer::DrawBorder(const DrawCommand& command) {
  const float right = command.width - command.border_right;
  const float bottom = command.height - command.border_bottom;
  FillRect(command.x, command.y, command.width, command.border_top, command.color);
  FillRect(command.x, command.y + command.height - command.border_bottom, command.width,
           command.border_bottom, command.color);
  FillRect(command.x, command.y, command.border_left, command.height, command.color);
  FillRect(command.x + right, command.y, command.border_right, command.height, command.color);
  (void)bottom;
}

void Rasterizer::DrawText(const DrawCommand& command) {
  const float scale = command.font_size / 8.0f;
  const int start_x = static_cast<int>(std::round(command.x));
  const int start_y = static_cast<int>(std::round(command.y));
  const int cell = static_cast<int>(std::ceil(scale));
  const int step = static_cast<int>(std::round(command.font_size));

  for (std::size_t i = 0; i < command.text.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(command.text[i]);
    if (ch < 32 || ch > 126) {
      continue;  // non-ASCII / control: not in the embedded font yet
    }
    const uint8_t* glyph = detail::kFont8x8[ch - 32];
    const int glyph_x = start_x + static_cast<int>(i) * step;
    for (int row = 0; row < 8; ++row) {
      for (int col = 0; col < 8; ++col) {
        if ((glyph[row] & (0x01 << col)) == 0) {
          continue;
        }
        FillRect(static_cast<float>(glyph_x + col * cell),
                 static_cast<float>(start_y + row * cell), static_cast<float>(cell),
                 static_cast<float>(cell), command.text_color);
      }
    }
  }
}

base::Result<void> WritePpm(std::string_view path, const Rasterizer& image) {
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

}  // namespace neko::paint
