// Unit tests for neko::media (WAV decoding).  Test inputs are produced by a
// tiny in-test WAV encoder, giving real format round trips.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "neko/base/status.h"
#include "neko/media/audio.h"
#include "neko/media/media_source.h"

namespace neko::media {
namespace {

std::string Le16Str(uint16_t v) {
  return {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
}
std::string Le32Str(uint32_t v) {
  return {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
          static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
}

// Builds a WAV file. |format| is the raw fmt code (1 PCM, 3 float, 0xFFFE
// extensible).  For extensible, |subformat| selects the actual codec and
// the extended chunk header is emitted.
std::string BuildWav(int rate, int channels, int bits, uint16_t format,
                     std::string_view pcm, uint16_t subformat = 0) {
  const uint32_t byte_rate = static_cast<uint32_t>(rate) * static_cast<uint32_t>(channels) *
                            static_cast<uint32_t>(bits) / 8;
  const uint16_t block_align = static_cast<uint16_t>(channels * bits / 8);

  std::string fmt;
  const uint16_t written_format = (format == 0xFFFE) ? 0xFFFE : format;
  fmt += Le16Str(written_format);
  fmt += Le16Str(static_cast<uint16_t>(channels));
  fmt += Le32Str(static_cast<uint32_t>(rate));
  fmt += Le32Str(byte_rate);
  fmt += Le16Str(block_align);
  fmt += Le16Str(static_cast<uint16_t>(bits));
  if (format == 0xFFFE) {
    // cbSize + wValidBitsPerSample + dwChannelMask + SubFormat GUID.
    fmt += Le16Str(22);
    fmt += Le16Str(static_cast<uint16_t>(bits));
    fmt += Le32Str(0);  // channel mask
    fmt += Le16Str(subformat);  // first two bytes of the GUID identify codec
    fmt += std::string(14, '\0');  // remainder of GUID
  }

  std::string riff = "WAVE";
  riff += "fmt ";
  riff += Le32Str(static_cast<uint32_t>(fmt.size()));
  riff += fmt;
  riff += "data";
  riff += Le32Str(static_cast<uint32_t>(pcm.size()));
  riff.append(pcm);

  std::string out = "RIFF";
  out += Le32Str(static_cast<uint32_t>(riff.size()));
  out += riff;
  return out;
}

TEST(WavTest, Decodes16BitMono) {
  std::string pcm;
  for (int i = 0; i < 4; ++i) {
    const int16_t v = static_cast<int16_t>(i * 1000);
    pcm += Le16Str(static_cast<uint16_t>(v));
  }
  const std::string wav = BuildWav(44100, 1, 16, 1, pcm);
  ASSERT_TRUE(IsWav(wav));

  auto r = DecodeWav(wav);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  const AudioData& a = r.value();
  EXPECT_EQ(a.sample_rate, 44100);
  EXPECT_EQ(a.channels, 1);
  EXPECT_EQ(a.bits_per_sample, 16);
  ASSERT_EQ(a.samples.size(), 4u);
  EXPECT_EQ(a.samples[0], 0);
  EXPECT_EQ(a.samples[3], 3000);
  EXPECT_NEAR(a.duration_seconds(), 4.0 / 44100.0, 1e-9);
}

TEST(WavTest, Decodes16BitStereo) {
  std::string pcm;
  const int16_t l[2] = {100, -200};
  const int16_t rch[2] = {-300, 400};
  for (int i = 0; i < 2; ++i) {
    pcm += Le16Str(static_cast<uint16_t>(l[i]));
    pcm += Le16Str(static_cast<uint16_t>(rch[i]));
  }
  const std::string wav = BuildWav(22050, 2, 16, 1, pcm);
  auto r = DecodeWav(wav);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  EXPECT_EQ(r.value().channels, 2);
  ASSERT_EQ(r.value().samples.size(), 4u);
  EXPECT_EQ(r.value().samples[0], 100);
  EXPECT_EQ(r.value().samples[1], -300);
  EXPECT_EQ(r.value().samples[2], -200);
  EXPECT_EQ(r.value().samples[3], 400);
}

TEST(WavTest, Decodes8BitUnsigned) {
  std::string pcm = {static_cast<char>(128), static_cast<char>(129), static_cast<char>(127),
                     static_cast<char>(0), static_cast<char>(255)};
  const std::string wav = BuildWav(8000, 1, 8, 1, pcm);
  auto r = DecodeWav(wav);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  ASSERT_EQ(r.value().samples.size(), 5u);
  EXPECT_EQ(r.value().samples[0], 0);      // 128 -> 0
  EXPECT_EQ(r.value().samples[1], 256);    // 129 -> +1<<8
  EXPECT_EQ(r.value().samples[2], -256);   // 127 -> -1<<8
  EXPECT_EQ(r.value().samples[3], -32768); // 0 -> -128<<8
  EXPECT_EQ(r.value().samples[4], 32512);  // 255 -> +127<<8
}

TEST(WavTest, Decodes24Bit) {
  // 0x123456 -> 0x1234 in 16-bit, and a negative value.
  std::string pcm = {
      '\x56', '\x34', '\x12',   // 0x123456
      '\x00', '\x00', '\x80',   // -8388608
  };
  const std::string wav = BuildWav(48000, 1, 24, 1, pcm);
  auto r = DecodeWav(wav);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  ASSERT_EQ(r.value().samples.size(), 2u);
  EXPECT_EQ(r.value().samples[0], 0x1234);
  EXPECT_EQ(r.value().samples[1], -32768);
}

TEST(WavTest, Decodes32BitInt) {
  std::string pcm = Le32Str(0x7FFFFFFF) + Le32Str(0x80000000);
  const std::string wav = BuildWav(96000, 1, 32, 1, pcm);
  auto r = DecodeWav(wav);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  ASSERT_EQ(r.value().samples.size(), 2u);
  EXPECT_EQ(r.value().samples[0], 32767);    // >>16
  EXPECT_EQ(r.value().samples[1], -32768);
}

TEST(WavTest, Decodes32BitFloat) {
  const float f1 = 0.5f, f2 = -1.0f, f3 = 2.0f;  // f3 clamped
  std::string pcm;
  pcm.append(reinterpret_cast<const char*>(&f1), 4);
  pcm.append(reinterpret_cast<const char*>(&f2), 4);
  pcm.append(reinterpret_cast<const char*>(&f3), 4);
  const std::string wav = BuildWav(44100, 1, 32, 3, pcm);
  auto r = DecodeWav(wav);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  ASSERT_EQ(r.value().samples.size(), 3u);
  EXPECT_NEAR(r.value().samples[0], 16384, 1);
  EXPECT_EQ(r.value().samples[1], -32767);  // -1.0 * 32767
  EXPECT_EQ(r.value().samples[2], 32767);   // clamped
}

TEST(WavTest, DecodesExtensiblePcm) {
  std::string pcm;
  pcm += Le16Str(0x0001);
  pcm += Le16Str(0xFFFF);
  const std::string wav = BuildWav(44100, 1, 16, 0xFFFE, pcm, /*subformat=*/1);
  auto r = DecodeWav(wav);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  EXPECT_EQ(r.value().bits_per_sample, 16);
  ASSERT_EQ(r.value().samples.size(), 2u);
  EXPECT_EQ(r.value().samples[0], 1);
  EXPECT_EQ(r.value().samples[1], -1);
}

TEST(WavTest, RejectsNonWav) {
  auto r = DecodeWav("this is not audio");
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(r.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST(WavTest, RejectsTruncatedChunk) {
  const std::string wav = BuildWav(44100, 1, 16, 1, std::string("\x00\x01\x00\x02", 4));
  auto r = DecodeWav(wav.substr(0, 20));
  EXPECT_FALSE(r.has_value());
}

TEST(WavTest, UnsupportedCodecIsNotImplemented) {
  // Format 2 = ADPCM.
  const std::string wav = BuildWav(44100, 1, 16, 2, std::string("\x00\x00\x00\x00", 4));
  auto r = DecodeWav(wav);
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(r.error().category(), base::ErrorCategory::kNotImplemented);
}

TEST(WavTest, RejectsMissingDataChunk) {
  // A WAV with a fmt chunk but no data chunk.
  std::string pcm;
  std::string wav = BuildWav(44100, 1, 16, 1, pcm);
  auto r = DecodeWav(wav);
  EXPECT_FALSE(r.has_value());
}

TEST(WavTest, RejectsDataSizeNotMultipleOfBlock) {
  std::string pcm = std::string("\x01\x02\x03", 3);  // 3 bytes for 16-bit mono
  const std::string wav = BuildWav(44100, 1, 16, 1, pcm);
  auto r = DecodeWav(wav);
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(r.error().category(), base::ErrorCategory::kParse);
}

// ---------------------------------------------------------------------------
// MediaSource (video) — must fail loudly, not pretend.
// ---------------------------------------------------------------------------

TEST(MediaSourceTest, VideoOpenReturnsNotImplemented) {
  const std::string fake_mp4 = std::string("\x00\x00\x00\x18", 4) + "ftypmp42";
  auto r = MediaSource::Open(fake_mp4);
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(r.error().category(), base::ErrorCategory::kNotImplemented);
}

}  // namespace
}  // namespace neko::media
