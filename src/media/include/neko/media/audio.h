#pragma once

#include "neko/base/status.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace neko::media {

// Decoded PCM audio.  Samples are always interleaved signed 16-bit,
// regardless of the source bit depth.
struct AudioData
{
  int sample_rate = 0;
  int channels = 0;
  int bits_per_sample = 0; // source bit depth (8/16/24/32)
  std::vector<int16_t> samples;

  bool empty() const
  {
    return sample_rate <= 0 || channels <= 0 || samples.empty();
  }
  double duration_seconds() const;
};

// Decodes a RIFF/WAVE file (PCM and IEEE-float, 8/16/24/32-bit, any channel
// count).  Unsupported codecs (ADPCM, MP3-in-WAV, ...) return a
// NOT IMPLEMENTED error rather than garbage audio.
base::Result<AudioData> DecodeWav(std::string_view data);

// True when the buffer begins with the RIFF/WAVE signature.
bool IsWav(std::string_view data);

} // namespace neko::media
