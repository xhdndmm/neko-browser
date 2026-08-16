// WebP decoding via libwebp, wrapped behind the Image interface.  Both the
// lossy (VP8) and lossless (VP8L) bitstreams are handled by libwebp's
// WebPDecodeRGBA; the result is top-down R,G,B,A like the other decoders.

#include "neko/base/status.h"
#include "neko/image/image.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <webp/decode.h>

namespace neko::image {

bool IsWebp(std::string_view data)
{
  return data.size() >= 12 && data.substr(0, 4) == "RIFF" && data.substr(8, 4) == "WEBP";
}

base::Result<Image> DecodeWebp(std::string_view data)
{
  if (!IsWebp(data)) {
    return base::Err(base::Error::Parse("webp: bad magic"));
  }
  const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
  int width = 0;
  int height = 0;
  if (WebPGetInfo(bytes, data.size(), &width, &height) != 1) {
    return base::Err(base::Error::Parse("webp: header decode failed"));
  }
  if (width <= 0 || height <= 0) {
    return base::Err(base::Error::Parse("webp: bad dimensions"));
  }
  uint8_t* rgba = WebPDecodeRGBA(bytes, data.size(), &width, &height);
  if (rgba == nullptr) {
    return base::Err(base::Error::Parse("webp: decode failed"));
  }
  Image out;
  out.width = width;
  out.height = height;
  out.rgba.assign(rgba,
                  rgba + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
  WebPFree(rgba);
  return base::Ok(std::move(out));
}

} // namespace neko::image
