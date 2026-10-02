// Serializes an inline <svg> subtree back to standalone SVG markup so the
// image module's SVG rasterizer can draw it into the replaced box.  Inline
// SVG is a replaced element with no external resource to fetch: the renderer
// rasterizes it itself (see Page::RasterizeInlineSvgImagesLocked).
#pragma once

#include <string>

namespace neko::dom {
class Element;
} // namespace neko::dom

namespace neko::style {
class StyleEngine;
} // namespace neko::style

namespace neko::renderer {

// Writes |element| (an <svg> element) and its descendants as an SVG document
// with the root width/height replaced by |width| x |height| CSS pixels (the
// element's used box size: author CSS width/height and layout shrink-to-fit
// are not visible to the standalone rasterizer, which only reads the
// attributes).  `currentColor` in attributes and style strings is substituted
// with each element's computed color, since the standalone rasterizer has no
// cascade to inherit from.
std::string SerializeInlineSvg(const dom::Element& element,
                               const style::StyleEngine& styles,
                               float width,
                               float height);

} // namespace neko::renderer
