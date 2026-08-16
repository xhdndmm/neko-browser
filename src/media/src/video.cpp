// Video demuxing + decoding through FFmpeg, wrapped behind the media module
// seam (see video.h / ADR 0014).  FFmpeg types never cross this file.
#include "neko/media/video.h"

#include "neko/base/status.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace neko::media {
namespace {

// Reads from the caller's memory buffer (see MediaSource::Impl::buffer).
struct ByteSource
{
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t pos = 0;
};

int ReadPacket(void* opaque, uint8_t* buf, int buf_size)
{
  auto* source = static_cast<ByteSource*>(opaque);
  const size_t remaining = source->size - source->pos;
  const size_t n =
      static_cast<size_t>(buf_size) < remaining ? static_cast<size_t>(buf_size) : remaining;
  if (n == 0) {
    return AVERROR_EOF;
  }
  std::memcpy(buf, source->data + source->pos, n);
  source->pos += n;
  return static_cast<int>(n);
}

base::Error AvError(const char* what, int code)
{
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(code, buf, sizeof(buf));
  return base::Error::Parse(std::string(what) + ": " + buf);
}

} // namespace

struct MediaSource::Impl
{
  ~Impl()
  {
    // Release in an order that keeps every remaining FFmpeg object valid:
    // decoder first, then the format context, then the per-frame buffers and
    // finally the I/O context.  avformat_open_input auto-sets
    // AVFMT_FLAG_CUSTOM_IO for a preset pb, so closing the format context
    // leaves the AVIOContext to us (the buffer may have been reallocated
    // while probing; avio->buffer is the current one).
    if (codec_ctx != nullptr) {
      avcodec_free_context(&codec_ctx);
    }
    if (fmt != nullptr) {
      avformat_close_input(&fmt);
    }
    if (packet != nullptr) {
      av_packet_free(&packet);
    }
    if (frame != nullptr) {
      av_frame_free(&frame);
    }
    if (rgb_frame != nullptr) {
      // The pixel buffer is refcounted through frame->buf (allocated by
      // av_frame_get_buffer), so av_frame_free releases it.
      av_frame_free(&rgb_frame);
    }
    if (sws != nullptr) {
      sws_freeContext(sws);
    }
    if (avio != nullptr) {
      av_freep(&avio->buffer);
      avio_context_free(&avio);
    }
  }

  // Our own copy of the input bytes: the read callback serves from here, so
  // it stays valid no matter what FFmpeg does with its I/O buffer.
  std::vector<uint8_t> source_bytes;
  ByteSource source;
  AVFormatContext* fmt = nullptr;
  AVIOContext* avio = nullptr;
  AVCodecContext* codec_ctx = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  AVFrame* rgb_frame = nullptr;
  uint8_t* rgb_data = nullptr;
  SwsContext* sws = nullptr;
  int stream_index = -1;
  AVRational time_base{1, 1};
  int width = 0;
  int height = 0;
  double duration_seconds = 0;
  double frame_rate = 0;
  std::string format_name;
  std::string codec_name;
  bool flushed = false;
};

MediaSource::~MediaSource() = default;
MediaSource::MediaSource(MediaSource&&) noexcept = default;
MediaSource& MediaSource::operator=(MediaSource&&) noexcept = default;

base::Result<MediaSource> MediaSource::Open(std::string_view data)
{
  MediaSource out;
  auto impl = std::make_unique<Impl>();
  if (data.size() > static_cast<size_t>(INT32_MAX)) {
    return base::Error::InvalidArgument("video: input too large");
  }
  // Keep our own copy of the input for the read callback (see Impl), and
  // hand FFmpeg an av_malloc'd I/O buffer — libavformat may free/replace it
  // while probing, so it must not come from a std::vector.
  impl->source_bytes.assign(data.begin(), data.end());
  impl->source = ByteSource{impl->source_bytes.data(), data.size(), 0};
  uint8_t* io_buffer = static_cast<uint8_t*>(av_malloc(data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  if (io_buffer == nullptr) {
    return base::Error::Unknown("video: av_malloc failed");
  }
  std::memcpy(io_buffer, data.data(), data.size());
  std::memset(io_buffer + data.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);

  impl->avio = avio_alloc_context(
      io_buffer, static_cast<int>(data.size()), 0, &impl->source, &ReadPacket, nullptr, nullptr);
  if (impl->avio == nullptr) {
    av_free(io_buffer);
    return base::Error::Unknown("video: avio_alloc_context failed");
  }
  impl->fmt = avformat_alloc_context();
  if (impl->fmt == nullptr) {
    return base::Error::Unknown("video: avformat_alloc_context failed");
  }
  impl->fmt->pb = impl->avio;
  int rc = avformat_open_input(&impl->fmt, nullptr, nullptr, nullptr);
  if (rc < 0) {
    return AvError("video: avformat_open_input", rc);
  }
  rc = avformat_find_stream_info(impl->fmt, nullptr);
  if (rc < 0) {
    return AvError("video: avformat_find_stream_info", rc);
  }
  const AVCodec* codec = nullptr;
  impl->stream_index = av_find_best_stream(impl->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
  if (impl->stream_index < 0) {
    return base::Error::Parse("video: no video stream found");
  }
  const AVStream* stream = impl->fmt->streams[impl->stream_index];
  impl->time_base = stream->time_base;
  if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
    impl->frame_rate = av_q2d(stream->avg_frame_rate);
  }
  if (impl->fmt->duration != AV_NOPTS_VALUE && impl->fmt->duration > 0) {
    impl->duration_seconds =
        static_cast<double>(impl->fmt->duration) / static_cast<double>(AV_TIME_BASE);
  }
  if (impl->fmt->iformat != nullptr && impl->fmt->iformat->name != nullptr) {
    impl->format_name = impl->fmt->iformat->name;
  }

  impl->codec_ctx = avcodec_alloc_context3(codec);
  if (impl->codec_ctx == nullptr) {
    return base::Error::Unknown("video: avcodec_alloc_context3 failed");
  }
  rc = avcodec_parameters_to_context(impl->codec_ctx, stream->codecpar);
  if (rc < 0) {
    return AvError("video: avcodec_parameters_to_context", rc);
  }
  rc = avcodec_open2(impl->codec_ctx, codec, nullptr);
  if (rc < 0) {
    return AvError("video: avcodec_open2", rc);
  }
  impl->width = impl->codec_ctx->width;
  impl->height = impl->codec_ctx->height;
  if (impl->width <= 0 || impl->height <= 0) {
    return base::Error::Parse("video: stream has no usable dimensions");
  }
  if (codec->name != nullptr) {
    impl->codec_name = codec->name;
  }
  impl->packet = av_packet_alloc();
  impl->frame = av_frame_alloc();
  if (impl->packet == nullptr || impl->frame == nullptr) {
    return base::Error::Unknown("video: frame allocation failed");
  }

  out.impl_ = std::move(impl);
  return out;
}

int MediaSource::width() const
{
  return impl_ != nullptr ? impl_->width : 0;
}

int MediaSource::height() const
{
  return impl_ != nullptr ? impl_->height : 0;
}

double MediaSource::duration_seconds() const
{
  return impl_ != nullptr ? impl_->duration_seconds : 0;
}

double MediaSource::frame_rate() const
{
  return impl_ != nullptr ? impl_->frame_rate : 0;
}

const std::string& MediaSource::format_name() const
{
  static const std::string kEmpty;
  return impl_ != nullptr ? impl_->format_name : kEmpty;
}

const std::string& MediaSource::codec_name() const
{
  static const std::string kEmpty;
  return impl_ != nullptr ? impl_->codec_name : kEmpty;
}

// Lazily creates the RGB conversion frame/context (the codec's pixel format
// is known once the decoder is open).
base::Result<void> MediaSource::EnsureRgbFrame()
{
  Impl* impl = impl_.get();
  if (impl == nullptr) {
    return base::Error::InvalidArgument("video: source not opened");
  }
  if (impl->rgb_frame != nullptr) {
    return base::Ok();
  }
  impl->rgb_frame = av_frame_alloc();
  if (impl->rgb_frame == nullptr) {
    return base::Error::Unknown("video: rgb frame allocation failed");
  }
  impl->rgb_frame->format = AV_PIX_FMT_RGBA;
  impl->rgb_frame->width = impl->width;
  impl->rgb_frame->height = impl->height;
  // av_frame_get_buffer allocates refcounted, alignment-safe buffers
  // (sws_scale assumes SIMD-friendly alignment; tightly packed buffers
  // get written past their end).
  const int rc = av_frame_get_buffer(impl->rgb_frame, 32);
  if (rc < 0) {
    return AvError("video: av_frame_get_buffer", rc);
  }
  impl->rgb_data = impl->rgb_frame->data[0];
  impl->sws = sws_getContext(impl->width,
                             impl->height,
                             impl->codec_ctx->pix_fmt,
                             impl->width,
                             impl->height,
                             AV_PIX_FMT_RGBA,
                             SWS_BILINEAR,
                             nullptr,
                             nullptr,
                             nullptr);
  if (impl->sws == nullptr) {
    return base::Error::Unknown("video: sws_getContext failed");
  }
  return base::Ok();
}

// Converts |impl->frame| (codec pixel format) into an engine RGBA image.
base::Result<VideoFrame> MediaSource::ConvertFrame()
{
  Impl* impl = impl_.get();
  if (impl == nullptr) {
    return base::Error::InvalidArgument("video: source not opened");
  }
  base::Result<void> ready = EnsureRgbFrame();
  if (!ready) {
    return ready.error();
  }
  const int scaled = sws_scale(impl->sws,
                               impl->frame->data,
                               impl->frame->linesize,
                               0,
                               impl->height,
                               impl->rgb_frame->data,
                               impl->rgb_frame->linesize);
  if (scaled < 0) {
    return base::Error::Parse("video: sws_scale failed");
  }
  VideoFrame out;
  double pts = 0;
  const int64_t ts = impl->frame->best_effort_timestamp;
  if (ts != AV_NOPTS_VALUE) {
    pts = static_cast<double>(ts) * av_q2d(impl->time_base);
  }
  out.pts_seconds = pts;
  out.image.width = impl->width;
  out.image.height = impl->height;
  const size_t row_bytes = static_cast<size_t>(impl->width) * 4;
  out.image.rgba.resize(row_bytes * static_cast<size_t>(impl->height));
  // RGBA rows are contiguous in our layout; av_image_alloc may pad lines.
  for (int y = 0; y < impl->height; ++y) {
    std::memcpy(out.image.rgba.data() + static_cast<size_t>(y) * row_bytes,
                impl->rgb_data +
                    static_cast<size_t>(y) * static_cast<size_t>(impl->rgb_frame->linesize[0]),
                row_bytes);
  }
  return out;
}

base::Result<std::optional<VideoFrame>> MediaSource::NextFrame()
{
  if (impl_ == nullptr) {
    return base::Error::InvalidArgument("video: source not opened");
  }
  // Drain already-buffered frames from the decoder first.
  for (;;) {
    const int rc = avcodec_receive_frame(impl_->codec_ctx, impl_->frame);
    if (rc == 0) {
      base::Result<VideoFrame> converted = ConvertFrame();
      if (!converted) {
        return converted.error();
      }
      return std::optional<VideoFrame>(std::move(converted.value()));
    }
    if (rc == AVERROR(EAGAIN)) {
      break;
    }
    if (rc == AVERROR_EOF) {
      return std::optional<VideoFrame>();
    }
    return AvError("video: avcodec_receive_frame", rc);
  }
  if (impl_->flushed) {
    return std::optional<VideoFrame>();
  }
  for (;;) {
    const int rc = av_read_frame(impl_->fmt, impl_->packet);
    if (rc < 0) {
      if (rc == AVERROR_EOF) {
        impl_->flushed = true;
        // Flush the decoder: any buffered frames surface on receive.
        const int flush_rc = avcodec_send_packet(impl_->codec_ctx, nullptr);
        if (flush_rc < 0 && flush_rc != AVERROR_EOF) {
          return AvError("video: avcodec_send_packet (flush)", flush_rc);
        }
        break;
      }
      return AvError("video: av_read_frame", rc);
    }
    if (impl_->packet->stream_index != impl_->stream_index) {
      av_packet_unref(impl_->packet);
      continue;
    }
    int send_rc = avcodec_send_packet(impl_->codec_ctx, impl_->packet);
    av_packet_unref(impl_->packet);
    if (send_rc < 0 && send_rc != AVERROR(EAGAIN) && send_rc != AVERROR_EOF) {
      return AvError("video: avcodec_send_packet", send_rc);
    }
    const int recv_rc = avcodec_receive_frame(impl_->codec_ctx, impl_->frame);
    if (recv_rc == 0) {
      base::Result<VideoFrame> converted = ConvertFrame();
      if (!converted) {
        return converted.error();
      }
      return std::optional<VideoFrame>(std::move(converted.value()));
    }
    if (recv_rc == AVERROR(EAGAIN)) {
      continue;
    }
    if (recv_rc == AVERROR_EOF) {
      impl_->flushed = true;
      return std::optional<VideoFrame>();
    }
    return AvError("video: avcodec_receive_frame", recv_rc);
  }
  // Drain the flush.
  for (;;) {
    const int rc = avcodec_receive_frame(impl_->codec_ctx, impl_->frame);
    if (rc == 0) {
      base::Result<VideoFrame> converted = ConvertFrame();
      if (!converted) {
        return converted.error();
      }
      return std::optional<VideoFrame>(std::move(converted.value()));
    }
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
      return std::optional<VideoFrame>();
    }
    return AvError("video: avcodec_receive_frame (flush)", rc);
  }
}

base::Result<VideoClip> DecodeVideo(std::string_view data, int max_frames, int64_t max_total_bytes)
{
  base::Result<MediaSource> source = MediaSource::Open(data);
  if (!source) {
    return source.error();
  }
  VideoClip clip;
  clip.width = source.value().width();
  clip.height = source.value().height();
  clip.duration_seconds = source.value().duration_seconds();
  clip.frame_rate = source.value().frame_rate();
  clip.format_name = source.value().format_name();
  clip.codec_name = source.value().codec_name();
  const int64_t bytes_per_frame =
      static_cast<int64_t>(clip.width) * static_cast<int64_t>(clip.height) * 4;
  while (static_cast<int>(clip.frames.size()) < max_frames) {
    base::Result<std::optional<VideoFrame>> frame = source.value().NextFrame();
    if (!frame) {
      return frame.error();
    }
    if (!frame.value().has_value()) {
      break;
    }
    if (bytes_per_frame > max_total_bytes) {
      return base::Error::Parse("video: frame budget exceeded");
    }
    max_total_bytes -= bytes_per_frame;
    clip.frames.push_back(std::move(*frame.value()));
  }
  return clip;
}

} // namespace neko::media
