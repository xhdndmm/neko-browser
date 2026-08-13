// Format detection and dispatch for the image decoder.

#include <string_view>

#include "neko/base/status.h"
#include "neko/image/image.h"

namespace neko::image {

base::Result<Image> DecodeImage(std::string_view data) {
  if (IsPng(data)) return DecodePng(data);
  if (IsJpeg(data)) return DecodeJpeg(data);
  return base::Error::NotImplemented(
      "unsupported image format (only PNG/JPEG are supported)");
}

}  // namespace neko::image
