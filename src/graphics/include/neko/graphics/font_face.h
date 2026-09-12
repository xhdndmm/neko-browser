#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neko::graphics {

// A rasterized glyph: 8-bit alpha bitmap plus horizontal metrics.
//
// |data| points into the owning |RasterizedGlyph::storage| buffer, which keeps
// it alive for the caller's use (important now that glyph rendering may run on
// multiple threads: the process-wide cache can evict entries concurrently, so
// callers always receive an owned copy rather than a cache pointer).
struct GlyphBitmap
{
  int width = 0;     // bitmap width in px
  int height = 0;    // bitmap height in px
  int pitch = 0;     // bytes per row (>= width)
  int left = 0;      // bearing X: bitmap left edge relative to the pen position
  int top = 0;       // bearing Y: bitmap top relative to the baseline (up = +)
  float advance = 0; // horizontal advance in px
  const uint8_t* data = nullptr;
};

// An owned glyph: the bitmap's |data| points into |storage|, which the caller
// holds for as long as it uses the bitmap.
struct RasterizedGlyph
{
  GlyphBitmap glyph;
  std::vector<uint8_t> storage;
};

// One edge of a glyph outline, in a y-down pixel coordinate system with the
// origin at the pen position on the baseline (the same convention as
// GlyphBitmap's bearings, and the one SVG needs).  |p| holds the control and
// end points: 1 point for move/line, 2 for a quadratic, 3 for a cubic.
enum class OutlineKind : uint8_t
{
  kMove,
  kLine,
  kQuadratic,
  kCubic,
  kClose,
};

struct OutlineEdge
{
  OutlineKind kind = OutlineKind::kMove;
  float p[6] = {0, 0, 0, 0, 0, 0};
};

// Vector outline of a glyph, decomposed into move/line/quadratic/cubic/close
// edges.  FreeType's conic curves are kept as quadratics, so consumers can
// flatten them at their own tolerance.
struct GlyphOutline
{
  std::vector<OutlineEdge> edges;
  float advance = 0; // horizontal advance in px
};

// One font file (TrueType/OpenType), loaded through the shared FreeType
// library.  Glyph rasterization is memoized by (pixel size, code point) in the
// process-wide glyph cache.  All FreeType access is serialized per face (the
// face state is not thread-safe); distinct faces may be used concurrently.
class FontFace
{
public:
  // Opens |path|; valid() reports whether the file parsed successfully.
  explicit FontFace(std::string path);
  // Loads |data| (TTF/OTF/WOFF/WOFF2 per FreeType support) from memory; the
  // face owns the bytes because FreeType requires the buffer to outlive it.
  // |key| identifies the face for caches (e.g. a URL or registry key).
  FontFace(std::string key, std::vector<uint8_t> data);
  ~FontFace();

  FontFace(const FontFace&) = delete;
  FontFace& operator=(const FontFace&) = delete;

  bool valid() const;
  const std::string& path() const
  {
    return path_;
  }

  // True when |code_point| has a glyph in this face (fallback support).
  bool HasGlyph(uint32_t code_point) const;

  // Horizontal advance (px) of |code_point| at |px_size| (font size in px).
  float Advance(uint32_t code_point, float px_size) const;

  // Total horizontal advance (px) of |text| (UTF-8) at |px_size|.
  float TextWidth(std::string_view text, float px_size) const;

  // Rasterized glyph for |code_point| at |px_size| (cached), returned as an
  // owned copy.  Returns nullopt for an unusable face or invalid input.
  std::optional<RasterizedGlyph> RenderGlyph(uint32_t code_point, float px_size) const;

  // Vector outline for |code_point| at |px_size| (hinted off, so the outline is
  // the design shape).  Returns nullopt when the face is unusable or FreeType
  // cannot load the glyph.  Whitespace glyphs yield an empty edge list plus
  // their advance.
  std::optional<GlyphOutline> OutlineGlyph(uint32_t code_point, float px_size) const;

  // Ascent above the baseline (px) at |px_size|.
  float Ascent(float px_size) const;

  // Descent below the baseline (px, positive magnitude) at |px_size|.
  float Descent(float px_size) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string path_;
};

} // namespace neko::graphics
