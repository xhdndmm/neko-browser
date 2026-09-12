// Format detection and dispatch for the image decoder.

#include "neko/image/image.h"

#include "neko/base/status.h"
#include "neko/image/svg_decoder.h"

#include <string_view>

namespace neko::image {

base::Result<Image> DecodeImage(std::string_view data)
{
  return DecodeImage(data, SvgTextShaper{});
}

// SVG is the only format that can need an injected glyph provider (for
// <text>); the other decoders ignore |shaper|.
base::Result<Image> DecodeImage(std::string_view data, const SvgTextShaper& shaper)
{
  if (IsPng(data))
    return DecodePng(data);
  if (IsJpeg(data))
    return DecodeJpeg(data);
  if (IsGif(data))
    return DecodeGif(data);
  if (IsWebp(data))
    return DecodeWebp(data);
  if (IsAvif(data))
    return DecodeAvif(data);
  if (IsSvg(data))
    return DecodeSvg(data, shaper);
  return base::Error::NotImplemented(
      "unsupported image format (only PNG/JPEG/GIF/WebP/AVIF/SVG are supported)");
}

} // namespace neko::image
