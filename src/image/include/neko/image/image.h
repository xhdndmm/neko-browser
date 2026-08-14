#pragma once

#include "neko/base/status.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace neko::image {

// A decoded raster image, always stored as 4 bytes per pixel (R,G,B,A),
// row-major and top-down.
struct Image
{
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba; // width * height * 4

  bool empty() const
  {
    return width <= 0 || height <= 0 || rgba.empty();
  }
};

// True when the buffer begins with the respective format's magic bytes.
bool IsPng(std::string_view data);
bool IsJpeg(std::string_view data);
bool IsGif(std::string_view data);

// Detects the format from magic bytes and decodes.  PNG, JPEG and GIF are
// supported; anything else yields a Parse error.
base::Result<Image> DecodeImage(std::string_view data);

// PNG: parsed and decoded in-house (chunks, CRC, filters, Adam7 interlace,
// bit depths 1/2/4/8/16, all color types); zlib is used only for the IDAT
// deflate stream.
base::Result<Image> DecodePng(std::string_view data);

// JPEG: baseline decode via libjpeg, wrapped behind the Image interface.
base::Result<Image> DecodeJpeg(std::string_view data);

// GIF (87a/89a): parsed and decoded in-house (LSD, global/local color tables,
// LZW decompression, interlace, graphic control extension transparency and
// disposal).  The first frame is composited onto the logical screen and
// returned; animation is not yet supported.
base::Result<Image> DecodeGif(std::string_view data);

} // namespace neko::image
