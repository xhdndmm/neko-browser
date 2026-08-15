// Format detection and dispatch for the image decoder.

#include "neko/image/image.h"

#include "neko/base/status.h"
#include "neko/image/svg_decoder.h"

#include <string_view>

namespace neko::image {

base::Result<Image> DecodeImage(std::string_view data)
{
  if (IsPng(data))
    return DecodePng(data);
  if (IsJpeg(data))
    return DecodeJpeg(data);
  if (IsGif(data))
    return DecodeGif(data);
  if (IsSvg(data))
    return DecodeSvg(data);
  return base::Error::NotImplemented(
      "unsupported image format (only PNG/JPEG/GIF/SVG are supported)");
}

} // namespace neko::image
