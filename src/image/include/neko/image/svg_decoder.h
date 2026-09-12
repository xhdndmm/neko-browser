#pragma once

#include "neko/image/image.h"

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace neko::image {

// True when the buffer appears to be an SVG document (starts with an XML
// declaration followed by <svg, or directly with <svg, after optional
// whitespace/BOM).
bool IsSvg(std::string_view data);

// One edge of a glyph outline supplied by a SvgTextShaper, in pixels at the
// requested pixel size, with the origin at the pen position on the baseline and
// y growing downwards.  |kind| selects how many of |p| are meaningful:
// 0 = move (1 point), 1 = line (1), 2 = quadratic (2: control, end),
// 3 = cubic (3: control1, control2, end), 4 = close (0).
struct SvgOutlineEdge
{
  enum Kind : int
  {
    kMove = 0,
    kLine = 1,
    kQuadratic = 2,
    kCubic = 3,
    kClose = 4,
  };
  int kind = kMove;
  std::array<double, 6> p = {0, 0, 0, 0, 0, 0};
};

struct SvgGlyphOutline
{
  std::vector<SvgOutlineEdge> edges; // empty for whitespace
  double advance = 0;                // horizontal advance in px
};

// Glyph provider for SVG <text>.  The SVG decoder itself has no font stack (the
// image module must not depend on the font rasterizer), so callers that can
// resolve fonts inject one; without it <text> is skipped rather than faked.
using SvgTextShaper = std::function<bool(std::string_view family,
                                         bool bold,
                                         bool italic,
                                         double pixel_size,
                                         std::string_view text,
                                         std::vector<SvgGlyphOutline>& out)>;

// Decodes an SVG subset into a raster Image.
//
// Supported: <svg> (width/height/viewBox/preserveAspectRatio="xMidYMid meet"),
// <g>, <rect>, <circle>, <ellipse>, <line>, <polyline>, <polygon>, <path>
// (M/L/H/V/C/S/Q/T/Z and lowercase relative variants), <text>/<tspan> (when a
// shaper is provided), fill / stroke / stroke-width / fill-opacity /
// stroke-opacity / opacity (presentation attributes and the style attribute),
// linear/radial gradients (fill="url(#id)", objectBoundingBox and
// userSpaceOnUse, gradientTransform, xlink:href inheritance) and transform
// (translate/scale/rotate/matrix).  Colors: #rgb / #rrggbb / rgb(r,g,b) / a set
// of named colors / none.  Rasterized with 2x supersampling for edge smoothing.
// Anything outside this subset is skipped (not an error), matching how real
// pages degrade gracefully.  Malformed SVG yields a Parse error.
base::Result<Image> DecodeSvg(std::string_view data, const SvgTextShaper& shaper = {});

// DecodeImage() for callers that can supply SVG text glyphs: identical to
// image::DecodeImage(data) for every other format.
base::Result<Image> DecodeImage(std::string_view data, const SvgTextShaper& shaper);

} // namespace neko::image
