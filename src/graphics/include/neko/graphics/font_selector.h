#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neko::graphics {

class FontFace;
class FontLibrary;
struct GlyphBitmap;

// Resolves a CSS font-family value into an ordered stack of font faces and
// provides per-character font fallback (WHATWG CSS Fonts / CSS Fonts 4
// fallback).  The stack always ends with a CJK-capable face when one is
// available, so Chinese characters render even when the requested family has
// no CJK glyphs.
//
// A concrete family name is matched by ResolveFamilyName(); generic families
// (sans-serif/serif/monospace; cursive/fantasy map to sans-serif) use the
// platform defaults.  Faces are loaded lazily through the FontLibrary.
class FontSelector {
 public:
  // |family| is a CSS font-family value (comma-separated, quoted names ok).
  // |weight| >= 600 requests a bold variant; |italic| an italic variant
  // (falling back to the regular face when the variant file is missing).
  // |library| must outlive this selector.
  FontSelector(const FontLibrary& library, std::string_view family, int weight = 400,
               bool italic = false);

  // First face in the stack that has a glyph for |code_point|, or nullptr.
  const FontFace* FaceForCodePoint(uint32_t code_point) const;

  // Primary face (first entry), or nullptr when nothing resolved.
  const FontFace* PrimaryFace() const { return faces_.empty() ? nullptr : faces_[0]; }

  // Text width with per-character fallback at |px_size|.
  float TextWidth(std::string_view text, float px_size) const;

  // Advance of |code_point| with per-character fallback at |px_size|.
  float Advance(uint32_t code_point, float px_size) const;

  // Rasterized glyph for |code_point| with per-character fallback, or nullptr.
  const GlyphBitmap* RenderGlyph(uint32_t code_point, float px_size) const;

  // Ascent of the primary face at |px_size| (0 when no face).
  float Ascent(float px_size) const;

  // Descent (positive) of the primary face at |px_size| (0 when no face).
  float Descent(float px_size) const;

  const std::vector<const FontFace*>& faces() const { return faces_; }

 private:
  void AddFamily(const FontLibrary& library, std::string_view family_name);

  const FontLibrary& library_;
  int weight_ = 400;
  bool italic_ = false;
  std::vector<const FontFace*> faces_;
};

}  // namespace neko::graphics
