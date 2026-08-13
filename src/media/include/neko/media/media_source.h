#pragma once

#include <string>
#include <string_view>

#include "neko/base/status.h"

namespace neko::media {

// ---------------------------------------------------------------------------
// Video / container demuxing.
//
// STATUS: NOT IMPLEMENTED.
//
// Real video support requires a demuxer (MP4/WebM/...) plus codec decoders
// (H.264/VP9/...), each of which is a multi-thousand-line subsystem.  This
// class is the reserved architecture seam: a future MediaSource will own a
// demuxer + decoder pipeline and expose decoded frames.  Today every call
// returns an explicit NOT IMPLEMENTED error so callers can detect and
// report the limitation honestly.
// ---------------------------------------------------------------------------
class MediaSource {
 public:
  MediaSource() = default;

  // Opens a media container from raw bytes.
  static base::Result<MediaSource> Open(std::string_view /*data*/) {
    return base::Error::NotImplemented(
        "video demuxing/decoding is not implemented (media pipeline is "
        "reserved architecture; see src/media/README.md)");
  }
};

}  // namespace neko::media
