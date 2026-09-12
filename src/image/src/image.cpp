// Format detection and dispatch for the image decoder.

#include "neko/image/image.h"

#include "neko/base/status.h"
#include "neko/image/svg_decoder.h"

#include <string_view>

namespace neko::image {

bool AdvanceGifFrame(const GifAnimation& animation,
                     double elapsed_ms,
                     std::size_t& frame,
                     std::size_t& loops,
                     bool& finished)
{
  if (finished || animation.frames.size() < 2 || frame >= animation.frames.size()) {
    return false;
  }
  const std::vector<GifFrame>& frames = animation.frames;
  std::size_t f = frame;
  double remaining = elapsed_ms;
  // Walk the frame schedule: each frame displays for its own delay.  At the
  // end of a pass the loop count decides between restarting (0 = forever) and
  // stopping on the last frame.
  for (;;) {
    const double duration = static_cast<double>(frames[f].delay_cs) * 10.0;
    if (duration <= 0 || remaining < duration) {
      break;
    }
    remaining -= duration;
    ++f;
    if (f >= frames.size()) {
      ++loops;
      const int loop_count = animation.loop_count;
      if (loop_count > 0 && loops >= static_cast<std::size_t>(loop_count)) {
        f = frames.size() - 1;
        finished = true;
        break;
      }
      f = 0;
    }
  }
  if (f == frame) {
    return false;
  }
  frame = f;
  return true;
}

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
