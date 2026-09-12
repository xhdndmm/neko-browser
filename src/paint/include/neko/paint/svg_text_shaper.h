#pragma once

#include "neko/image/svg_decoder.h"

namespace neko::graphics {
class FontRegistry;
}

namespace neko::paint {

// Builds the glyph-outline provider used when rasterizing SVG <text>.
//
// The image module decodes SVG but owns no font stack (ADR 0009: only
// neko::graphics touches FreeType), so the caller injects one.  Families are
// resolved through |fonts| (the same registry the page uses for web fonts), with
// per-character fallback, and glyph outlines come from the hinted-off FreeType
// outline API.
//
// The returned shaper is safe to call from multiple threads and caches the
// resolved selector per (family, weight, italic).  |fonts| must outlive it.
image::SvgTextShaper CreateSvgTextShaper(const graphics::FontRegistry& fonts);

} // namespace neko::paint
