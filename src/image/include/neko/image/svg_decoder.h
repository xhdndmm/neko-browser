#pragma once

#include <string_view>

#include "neko/image/image.h"

namespace neko::image {

// True when the buffer appears to be an SVG document (starts with an XML
// declaration followed by <svg, or directly with <svg, after optional
// whitespace/BOM).
bool IsSvg(std::string_view data);

// Decodes a minimal SVG subset into a raster Image.
//
// Supported: <svg> (width/height/viewBox/preserveAspectRatio="xMidYMid meet"),
// <g>, <rect>, <circle>, <ellipse>, <line>, <polyline>, <polygon>, <path>
// (M/L/H/V/C/S/Q/T/Z and lowercase relative variants), fill / stroke /
// stroke-width / fill-opacity / stroke-opacity / opacity, and transform
// (translate/scale/rotate).  Colors: #rgb / #rrggbb / rgb(r,g,b) / a set of
// named colors / none.  Rasterized with 2x supersampling for edge smoothing.
// Anything outside this subset is skipped (not an error), matching how real
// pages degrade gracefully.  Malformed SVG yields a Parse error.
base::Result<Image> DecodeSvg(std::string_view data);

} // namespace neko::image
