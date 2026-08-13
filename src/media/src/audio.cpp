// RIFF/WAVE decoder, implemented from scratch.
//
// Supported: PCM (format 1) and IEEE float (format 3) at 8/16/24/32 bits,
// including WAVE_FORMAT_EXTENSIBLE (0xFFFE) wrappers.  Output is converted
// to interleaved signed 16-bit PCM.  Other codecs (ADPCM, compressed
// formats) return NOT IMPLEMENTED.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "neko/base/status.h"
#include "neko/media/audio.h"

namespace neko::media {
namespace {

constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatIeeeFloat = 3;
constexpr uint16_t kFormatExtensible = 0xFFFE;

uint16_t Le16(std::string_view s, size_t off) {
  return static_cast<uint16_t>(static_cast<uint8_t>(s[off]) |
                               (static_cast<uint8_t>(s[off + 1]) << 8));
}

uint32_t Le32(std::string_view s, size_t off) {
  return static_cast<uint32_t>(static_cast<uint8_t>(s[off])) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[off + 1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[off + 2])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[off + 3])) << 24);
}

bool IsId(std::string_view s, size_t off, const char* id) {
  return s.compare(off, 4, id, 4) == 0;
}

}  // namespace

bool IsWav(std::string_view data) {
  return data.size() >= 12 && IsId(data, 0, "RIFF") && IsId(data, 8, "WAVE");
}

double AudioData::duration_seconds() const {
  if (sample_rate <= 0 || channels <= 0) return 0.0;
  return static_cast<double>(samples.size()) /
         static_cast<double>(static_cast<int64_t>(channels) * sample_rate);
}

base::Result<AudioData> DecodeWav(std::string_view data) {
  if (!IsWav(data)) {
    return base::Error::InvalidArgument("not a RIFF/WAVE file");
  }

  bool have_fmt = false;
  uint16_t format = 0;
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint16_t block_align = 0;
  uint16_t bits = 0;
  std::string_view pcm;

  size_t pos = 12;
  while (pos + 8 <= data.size()) {
    const uint32_t size = Le32(data, pos + 4);
    if (pos + 8 + size > data.size()) {
      return base::Error::Parse("wav: truncated chunk");
    }
    if (IsId(data, pos, "fmt ")) {
      if (size < 16) return base::Error::Parse("wav: fmt chunk too small");
      format = Le16(data, pos + 8);
      channels = Le16(data, pos + 10);
      sample_rate = Le32(data, pos + 12);
      block_align = Le16(data, pos + 20);
      bits = Le16(data, pos + 22);
      if (format == kFormatExtensible && size >= 40) {
        // WAVE_FORMAT_EXTENSIBLE: the actual codec is the first two bytes of
        // the subformat GUID (at offset 24 within the fmt payload).
        format = Le16(data, pos + 8 + 24);
      }
      have_fmt = true;
    } else if (IsId(data, pos, "data")) {
      pcm = data.substr(pos + 8, size);
    }
    pos += 8 + size + (size & 1);  // chunks are padded to even sizes
  }

  if (!have_fmt) return base::Error::Parse("wav: missing fmt chunk");
  if (pcm.empty()) return base::Error::Parse("wav: missing data chunk");
  if (sample_rate == 0 || channels == 0 || channels > 8) {
    return base::Error::Parse("wav: invalid sample rate or channel count");
  }
  if (bits != 8 && bits != 16 && bits != 24 && bits != 32) {
    return base::Error::NotImplemented("wav: unsupported bit depth " + std::to_string(bits));
  }
  if (format != kFormatPcm && format != kFormatIeeeFloat) {
    return base::Error::NotImplemented("wav: unsupported audio codec (format " +
                                       std::to_string(format) + ")");
  }
  const size_t bytes_per_sample = static_cast<size_t>(bits) / 8;
  const size_t expected_block = static_cast<size_t>(channels) * bytes_per_sample;
  if (block_align != 0 && block_align != expected_block) {
    return base::Error::Parse("wav: block align mismatch");
  }
  if (pcm.size() % expected_block != 0) {
    return base::Error::Parse("wav: data size is not a multiple of the block size");
  }

  AudioData out;
  out.sample_rate = static_cast<int>(sample_rate);
  out.channels = static_cast<int>(channels);
  out.bits_per_sample = bits;
  const size_t frame_count = pcm.size() / expected_block;
  out.samples.reserve(frame_count * static_cast<size_t>(channels));

  for (size_t frame = 0; frame < frame_count; ++frame) {
    const size_t base = frame * expected_block;
    for (int ch = 0; ch < channels; ++ch) {
      const size_t off = base + static_cast<size_t>(ch) * bytes_per_sample;
      int32_t value = 0;
      switch (bits) {
        case 8: {
          // 8-bit PCM is unsigned.
          value = (static_cast<int32_t>(static_cast<uint8_t>(pcm[off])) - 128) << 8;
          break;
        }
        case 16: {
          value = static_cast<int16_t>(Le16(pcm, off));
          break;
        }
        case 24: {
          int32_t v = static_cast<uint8_t>(pcm[off]) |
                      (static_cast<int32_t>(static_cast<uint8_t>(pcm[off + 1])) << 8) |
                      (static_cast<int32_t>(static_cast<uint8_t>(pcm[off + 2])) << 16);
          if (v & 0x800000) v -= 0x1000000;  // sign extend
          value = v >> 8;
          break;
        }
        case 32: {
          if (format == kFormatIeeeFloat) {
            float f;
            std::memcpy(&f, pcm.data() + off, 4);
            if (std::isnan(f) || f > 1.0f) f = 1.0f;
            if (f < -1.0f) f = -1.0f;
            value = static_cast<int32_t>(f * 32767.0f);
          } else {
            value = static_cast<int32_t>(Le32(pcm, off)) >> 16;
          }
          break;
        }
      }
      out.samples.push_back(static_cast<int16_t>(value));
    }
  }
  return out;
}

}  // namespace neko::media
