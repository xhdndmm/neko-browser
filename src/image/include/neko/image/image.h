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

// One frame of an animated GIF.  Each frame is the complete composited
// canvas (RGBA, top-down) as it should appear on screen, not just the
// changed region.
struct GifFrame
{
  std::vector<uint8_t> rgba; // width * height * 4
  // Display duration in centiseconds (the GCE delay field, 1 = 10 ms).
  // Delays below 2 cs are clamped to 10 cs (100 ms) at decode time,
  // matching browser behavior (Blink/Gecko).
  int delay_cs = 0;
};

// A fully decoded animated GIF: every frame pre-composited onto the logical
// screen, in display order.
struct GifAnimation
{
  int width = 0;
  int height = 0;
  std::vector<GifFrame> frames; // in display order, always >= 1

  // NETSCAPE2.0/ANIMEXTS1.0 loop extension: the total number of animation
  // iterations (0 = loop forever).  Browsers default to 0 (infinite) when
  // the extension is absent.
  int loop_count = 0;

  bool empty() const
  {
    return width <= 0 || height <= 0 || frames.empty();
  }
};

// True when the buffer begins with the respective format's magic bytes.
bool IsPng(std::string_view data);
bool IsJpeg(std::string_view data);
bool IsGif(std::string_view data);
bool IsWebp(std::string_view data);
bool IsAvif(std::string_view data);

// Detects the format from magic bytes and decodes.  PNG, JPEG, GIF, WebP,
// AVIF and SVG are supported; anything else yields an error.
base::Result<Image> DecodeImage(std::string_view data);

// PNG: parsed and decoded in-house (chunks, CRC, filters, Adam7 interlace,
// bit depths 1/2/4/8/16, all color types); zlib is used only for the IDAT
// deflate stream.
base::Result<Image> DecodePng(std::string_view data);

// JPEG: baseline decode via libjpeg, wrapped behind the Image interface.
base::Result<Image> DecodeJpeg(std::string_view data);

// GIF (87a/89a): parsed and decoded in-house (LSD, global/local color tables,
// LZW decompression, interlace, graphic control extension transparency and
// disposal).  Returns the first frame composited onto the logical screen
// (used for still-image paths); DecodeGifAnimation returns every frame.
base::Result<Image> DecodeGif(std::string_view data);

// Decodes every frame of an animated GIF, compositing each onto the logical
// screen (GCE transparency, disposal methods 0-3 with 4 normalized to 3,
// NETSCAPE2.0 loop count).  The first frame is always kept; further frames
// are decoded until a 64 MiB total pixel budget (or 2048 frames) is reached,
// after which the animation is truncated and remaining frames are dropped.
base::Result<GifAnimation> DecodeGifAnimation(std::string_view data);

// WebP (lossy VP8 / lossless VP8L): decoded via libwebp, wrapped behind the
// Image interface.
base::Result<Image> DecodeWebp(std::string_view data);

// AVIF (AV1 Image File Format): decoded via libavif, wrapped behind the Image
// interface (8-bit RGBA).  Animated AVIF (avis) returns its first frame;
// animation is not supported.
base::Result<Image> DecodeAvif(std::string_view data);

} // namespace neko::image
