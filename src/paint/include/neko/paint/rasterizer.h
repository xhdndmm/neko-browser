#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "neko/base/status.h"
#include "neko/css/color.h"
#include "neko/paint/display_list.h"

namespace neko::graphics {
class FontRegistry;
struct GlyphBitmap;
}

namespace neko::paint {

// Software rasterizer for DisplayList commands.  Produces an RGBA8888 buffer.
//
// Phase 6 scope: axis-aligned fills, borders and text.  Text is drawn with a
// FreeType-backed font stack (neko::graphics FontRegistry, per-character
// fallback for CJK) when a registry is set; otherwise the embedded 8x8 bitmap
// font is used as a fallback so pages never lose text.  No transforms,
// gradients, images or alpha compositing layers.
class Rasterizer {
 public:
  Rasterizer(int width, int height);

  void Clear(css::Color color);
  void Rasterize(const DisplayList& list);

  // Uses |registry| (owned by the caller, must outlive this rasterizer) for
  // text; pass nullptr to fall back to the embedded 8x8 bitmap font.
  void SetFontRegistry(const graphics::FontRegistry* registry) { registry_ = registry; }

  // Scrolls the visible region: all draw commands are shifted up by |offset|
  // pixels, so content below the viewport can be panned into view.  Content
  // scrolled out of the buffer is clipped.
  void SetScrollOffset(float offset);

  int width() const { return width_; }
  int height() const { return height_; }

  // RGBA8888, row-major, height*width*4 bytes.
  const std::vector<uint8_t>& pixels() const { return pixels_; }

  // Text metrics for the embedded font: every glyph advances by |font_size|.
  // (Layout still uses the monospace model; real advances arrive in M2.)
  static float CharWidth(float font_size) { return font_size; }
  static float TextWidth(std::string_view text, float font_size);

 private:
  void FillRect(float x, float y, float width, float height, css::Color color);
  void DrawBorder(const DrawCommand& command);
  void DrawText(const DrawCommand& command);
  void DrawText8x8(const DrawCommand& command);
  void DrawTextFreetype(const DrawCommand& command);
  void BlendGlyph(int x, int y, const graphics::GlyphBitmap& glyph, css::Color color);

  int width_;
  int height_;
  float scroll_offset_ = 0;
  std::vector<uint8_t> pixels_;
  const graphics::FontRegistry* registry_ = nullptr;
};

// Writes an RGBA buffer as a binary PPM (P6) file.  Alpha is composited over
// white.  Returns an error on I/O failure.
base::Result<void> WritePpm(std::string_view path, const Rasterizer& image);

}  // namespace neko::paint
