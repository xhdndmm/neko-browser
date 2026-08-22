#pragma once

#include "neko/graphics/font_face.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace neko::graphics {

class FontFace;
class FontLibrary;

// Resolves a CSS font-family value into an ordered stack of font faces and
// provides per-character font fallback (WHATWG CSS Fonts / CSS Fonts 4
// fallback).  The stack always ends with a CJK-capable face when one is
// available, so Chinese characters render even when the requested family has
// no CJK glyphs.
//
// A concrete family name is matched by ResolveFamilyName(); generic families
// (sans-serif/serif/monospace; cursive/fantasy map to sans-serif) use the
// platform defaults.  Faces are loaded lazily through the FontLibrary.
//
// Instances are immutable after construction and safe to use from multiple
// threads (TextWidth memoizes results behind a mutex).
class FontSelector
{
public:
  // |family| is a CSS font-family value (comma-separated, quoted names ok).
  // |weight| >= 600 requests a bold variant; |italic| an italic variant
  // (falling back to the regular face when the variant file is missing).
  // |library| must outlive this selector.  |preseed_faces| are put ahead of
  // every system face (web fonts registered via @font-face).
  FontSelector(const FontLibrary& library,
               std::string_view family,
               int weight = 400,
               bool italic = false,
               const std::vector<const FontFace*>& preseed_faces = {});

  // First face in the stack that has a glyph for |code_point|, or nullptr.
  const FontFace* FaceForCodePoint(uint32_t code_point) const;

  // Primary face (first entry), or nullptr when nothing resolved.
  const FontFace* PrimaryFace() const
  {
    return faces_.empty() ? nullptr : faces_[0];
  }

  // Text width with per-character fallback at |px_size|.  Results are
  // memoized per (text, size): layout measures the same strings repeatedly
  // (menu items, headings, repeated words), so this avoids re-decoding UTF-8
  // and re-summing per-glyph advances on every pass.
  float TextWidth(std::string_view text, float px_size) const;

  // Advance of |code_point| with per-character fallback at |px_size|.
  float Advance(uint32_t code_point, float px_size) const;

  // Rasterized glyph for |code_point| with per-character fallback, or
  // nullopt.  Returns an owned copy (thread-safe).
  std::optional<RasterizedGlyph> RenderGlyph(uint32_t code_point, float px_size) const;

  // Ascent of the primary face at |px_size| (0 when no face).
  float Ascent(float px_size) const;

  // Descent (positive) of the primary face at |px_size| (0 when no face).
  float Descent(float px_size) const;

  const std::vector<const FontFace*>& faces() const
  {
    return faces_;
  }

private:
  void AddFamily(const FontLibrary& library, std::string_view family_name);

  const FontLibrary& library_;
  int weight_ = 400;
  bool italic_ = false;
  std::vector<const FontFace*> faces_;

  // Memoized TextWidth results; bounded (cleared when over the cap).  Guarded
  // for concurrent layout/paint (worker thread + raster pool).
  static constexpr std::size_t kWidthCacheCap = 4096;
  mutable std::mutex width_mutex_;
  mutable std::unordered_map<std::string, float> width_cache_;
};

} // namespace neko::graphics
