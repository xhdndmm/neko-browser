// AVIF (AV1 Image File Format) decoding via libavif, wrapped behind the
// Image interface.  AV1 bitstream decoding is delegated to libavif (which
// uses dav1d/aom); the container (ISO-BMFF "ftypavif"/"ftypavis") is
// detected by magic bytes and the first frame is converted to 8-bit RGBA,
// top-down like the other decoders.
//
// Animated AVIF (avis) decodes its first frame only; animation is documented
// as not supported (like direct GIF navigation).

#include "neko/base/status.h"
#include "neko/image/image.h"

#include <avif/avif.h>

#include <cstdint>
#include <string_view>

namespace neko::image {

namespace {

// Decoded-pixel budget: a crafted file must not be able to balloon memory
// (same cap as the GIF decoder's canvas).
constexpr std::size_t kMaxCanvasBytes = 128u * 1024u * 1024u;

// RAII guard for avifDecoder.
struct DecoderGuard
{
  avifDecoder* decoder = nullptr;
  ~DecoderGuard()
  {
    if (decoder != nullptr) {
      avifDecoderDestroy(decoder);
    }
  }
};

} // namespace

bool IsAvif(std::string_view data)
{
  // ISO-BMFF: [size(4)]["ftyp"][brand(4)].  "avif" = still image,
  // "avis" = image sequence.
  if (data.size() < 12 || data.substr(4, 4) != "ftyp") {
    return false;
  }
  return data.substr(8, 4) == "avif" || data.substr(8, 4) == "avis";
}

base::Result<Image> DecodeAvif(std::string_view data)
{
  if (!IsAvif(data)) {
    return base::Err(base::Error::Parse("avif: bad magic"));
  }
  DecoderGuard guard;
  guard.decoder = avifDecoderCreate();
  if (guard.decoder == nullptr) {
    return base::Err(base::Error::Unknown("avif: decoder allocation failed"));
  }
  avifResult result = avifDecoderSetIOMemory(
      guard.decoder, reinterpret_cast<const uint8_t*>(data.data()), data.size());
  if (result != AVIF_RESULT_OK) {
    return base::Err(base::Error::Parse("avif: set IO failed"));
  }
  result = avifDecoderParse(guard.decoder);
  if (result != AVIF_RESULT_OK) {
    return base::Err(base::Error::Parse("avif: parse failed"));
  }
  result = avifDecoderNextImage(guard.decoder);
  if (result != AVIF_RESULT_OK) {
    return base::Err(base::Error::Parse("avif: no decodable frame"));
  }
  const avifImage* source = guard.decoder->image;
  if (source == nullptr || source->width <= 0 || source->height <= 0) {
    return base::Err(base::Error::Parse("avif: bad dimensions"));
  }
  const std::size_t pixel_bytes = static_cast<std::size_t>(source->width) *
                                  static_cast<std::size_t>(source->height) * 4;
  if (pixel_bytes > kMaxCanvasBytes) {
    return base::Err(base::Error::Parse("avif: image too large"));
  }

  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, source);
  rgb.format = AVIF_RGB_FORMAT_RGBA;
  rgb.depth = 8;
  Image out;
  out.width = static_cast<int>(source->width);
  out.height = static_cast<int>(source->height);
  out.rgba.resize(pixel_bytes);
  rgb.pixels = out.rgba.data();
  rgb.rowBytes = static_cast<uint32_t>(source->width) * 4;
  result = avifImageYUVToRGB(source, &rgb);
  if (result != AVIF_RESULT_OK) {
    return base::Err(base::Error::Parse("avif: pixel conversion failed"));
  }
  return out;
}

} // namespace neko::image
