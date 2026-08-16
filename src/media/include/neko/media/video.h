#pragma once

#include "neko/base/status.h"
#include "neko/image/image.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neko::media {

// ---------------------------------------------------------------------------
// Video demuxing + decoding (FFmpeg, wrapped behind this seam — see
// ADR 0014).  The engine never exposes FFmpeg types; decoded frames arrive
// as engine-owned image::Image (RGBA, top-down rows).
//
// STATUS: PARTIALLY IMPLEMENTED.  Supports the containers/codecs FFmpeg's
// demuxers and decoders cover (MP4/H.264, WebM/VP8-VP9, ...); decoded frames
// are converted to 8-bit RGBA.  Audio tracks are ignored (not extracted).
// ---------------------------------------------------------------------------

// One decoded video frame in presentation order.
struct VideoFrame
{
  double pts_seconds = 0; // presentation timestamp (seconds)
  image::Image image;     // RGBA, top-down
};

// A fully decoded video (bounded; see DecodeVideo).
struct VideoClip
{
  int width = 0;
  int height = 0;
  double duration_seconds = 0;
  double frame_rate = 0;
  std::string format_name; // container (e.g. "mov,mp4,m4a,3gp,3g2,mj2")
  std::string codec_name;  // decoder (e.g. "h264", "vp9")
  std::vector<VideoFrame> frames;
};

// Decodes every video frame of an in-memory media container, bounded by
// |max_frames| and |max_total_bytes| (decoded RGBA bytes) so a pathological
// input cannot exhaust memory.  Audio tracks are ignored.
base::Result<VideoClip> DecodeVideo(std::string_view data,
                                    int max_frames = 2048,
                                    int64_t max_total_bytes = 128LL * 1024 * 1024);

// Streaming interface over the same pipeline: a demuxer + decoder that hands
// out frames one at a time in presentation order (intended for <video>
// playback).  Returns nullopt at end of stream.
class MediaSource
{
public:
  MediaSource() = default;
  ~MediaSource();

  MediaSource(MediaSource&&) noexcept;
  MediaSource& operator=(MediaSource&&) noexcept;
  MediaSource(const MediaSource&) = delete;
  MediaSource& operator=(const MediaSource&) = delete;

  // Opens a media container from raw bytes (demuxes, locates the video
  // stream and opens its decoder).
  static base::Result<MediaSource> Open(std::string_view data);

  int width() const;
  int height() const;
  double duration_seconds() const;
  double frame_rate() const;
  const std::string& format_name() const;
  const std::string& codec_name() const;

  // Decodes and converts the next frame.  EOF yields nullopt; decoder errors
  // are surfaced as an Err result.
  base::Result<std::optional<VideoFrame>> NextFrame();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  // Lazily allocates the RGB conversion frame/context (codec pixel format
  // known once the decoder is open); converts |impl_->frame| to RGBA.
  base::Result<void> EnsureRgbFrame();
  base::Result<VideoFrame> ConvertFrame();
};

} // namespace neko::media
