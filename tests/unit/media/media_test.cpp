// Unit tests for neko::media (WAV decoding).  Test inputs are produced by a
// tiny in-test WAV encoder, giving real format round trips.

#include "neko/base/status.h"
#include "neko/media/audio.h"
#include "neko/media/media_source.h"
#include "neko/media/video.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace neko::media {
namespace {

std::string Le16Str(uint16_t v)
{
  return {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
}
std::string Le32Str(uint32_t v)
{
  return {static_cast<char>(v & 0xFF),
          static_cast<char>((v >> 8) & 0xFF),
          static_cast<char>((v >> 16) & 0xFF),
          static_cast<char>((v >> 24) & 0xFF)};
}

// Builds a WAV file. |format| is the raw fmt code (1 PCM, 3 float, 0xFFFE
// extensible).  For extensible, |subformat| selects the actual codec and
// the extended chunk header is emitted.
std::string BuildWav(
    int rate, int channels, int bits, uint16_t format, std::string_view pcm, uint16_t subformat = 0)
{
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
    fmt += Le32Str(0);            // channel mask
    fmt += Le16Str(subformat);    // first two bytes of the GUID identify codec
    fmt += std::string(14, '\0'); // remainder of GUID
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

TEST(WavTest, Decodes16BitMono)
{
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

TEST(WavTest, Decodes16BitStereo)
{
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

TEST(WavTest, Decodes8BitUnsigned)
{
  std::string pcm = {static_cast<char>(128),
                     static_cast<char>(129),
                     static_cast<char>(127),
                     static_cast<char>(0),
                     static_cast<char>(255)};
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

TEST(WavTest, Decodes24Bit)
{
  // 0x123456 -> 0x1234 in 16-bit, and a negative value.
  std::string pcm = {
      '\x56',
      '\x34',
      '\x12', // 0x123456
      '\x00',
      '\x00',
      '\x80', // -8388608
  };
  const std::string wav = BuildWav(48000, 1, 24, 1, pcm);
  auto r = DecodeWav(wav);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  ASSERT_EQ(r.value().samples.size(), 2u);
  EXPECT_EQ(r.value().samples[0], 0x1234);
  EXPECT_EQ(r.value().samples[1], -32768);
}

TEST(WavTest, Decodes32BitInt)
{
  std::string pcm = Le32Str(0x7FFFFFFF) + Le32Str(0x80000000);
  const std::string wav = BuildWav(96000, 1, 32, 1, pcm);
  auto r = DecodeWav(wav);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  ASSERT_EQ(r.value().samples.size(), 2u);
  EXPECT_EQ(r.value().samples[0], 32767); // >>16
  EXPECT_EQ(r.value().samples[1], -32768);
}

TEST(WavTest, Decodes32BitFloat)
{
  const float f1 = 0.5f, f2 = -1.0f, f3 = 2.0f; // f3 clamped
  std::string pcm;
  pcm.append(reinterpret_cast<const char*>(&f1), 4);
  pcm.append(reinterpret_cast<const char*>(&f2), 4);
  pcm.append(reinterpret_cast<const char*>(&f3), 4);
  const std::string wav = BuildWav(44100, 1, 32, 3, pcm);
  auto r = DecodeWav(wav);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  ASSERT_EQ(r.value().samples.size(), 3u);
  EXPECT_NEAR(r.value().samples[0], 16384, 1);
  EXPECT_EQ(r.value().samples[1], -32767); // -1.0 * 32767
  EXPECT_EQ(r.value().samples[2], 32767);  // clamped
}

TEST(WavTest, DecodesExtensiblePcm)
{
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

TEST(WavTest, RejectsNonWav)
{
  auto r = DecodeWav("this is not audio");
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(r.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST(WavTest, RejectsTruncatedChunk)
{
  const std::string wav = BuildWav(44100, 1, 16, 1, std::string("\x00\x01\x00\x02", 4));
  auto r = DecodeWav(wav.substr(0, 20));
  EXPECT_FALSE(r.has_value());
}

TEST(WavTest, UnsupportedCodecIsNotImplemented)
{
  // Format 2 = ADPCM.
  const std::string wav = BuildWav(44100, 1, 16, 2, std::string("\x00\x00\x00\x00", 4));
  auto r = DecodeWav(wav);
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(r.error().category(), base::ErrorCategory::kNotImplemented);
}

TEST(WavTest, RejectsMissingDataChunk)
{
  // A WAV with a fmt chunk but no data chunk.
  std::string pcm;
  std::string wav = BuildWav(44100, 1, 16, 1, pcm);
  auto r = DecodeWav(wav);
  EXPECT_FALSE(r.has_value());
}

TEST(WavTest, RejectsDataSizeNotMultipleOfBlock)
{
  std::string pcm = std::string("\x01\x02\x03", 3); // 3 bytes for 16-bit mono
  const std::string wav = BuildWav(44100, 1, 16, 1, pcm);
  auto r = DecodeWav(wav);
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(r.error().category(), base::ErrorCategory::kParse);
}

// ---------------------------------------------------------------------------
// MediaSource (video) — invalid input must fail loudly, not pretend.
// ---------------------------------------------------------------------------

TEST(MediaSourceTest, VideoOpenRejectsInvalidInput)
{
  const std::string fake_mp4 = std::string("\x00\x00\x00\x18", 4) + "ftypmp42";
  auto r = MediaSource::Open(fake_mp4);
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(r.error().category(), base::ErrorCategory::kParse);
}

// ---------------------------------------------------------------------------
// Video (FFmpeg-backed demuxing + decoding).  Fixtures are tiny videos
// committed under tests/pages (sample_8x6_h264.mp4: 8x6 testsrc, 2 fps,
// 3 s; sample_4x4_vp9.webm: 4x4 testsrc, 1 fps, 2 s).
// ---------------------------------------------------------------------------

// Reads a committed test fixture (NEKO_TEST_PAGES_DIR is injected by CMake).
std::string ReadFixture(const char* name)
{
  const std::string path = std::string(NEKO_TEST_PAGES_DIR) + "/" + name;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

TEST(MediaVideoTest, DecodesH264Mp4)
{
  const std::string data = ReadFixture("sample_8x6_h264.mp4");
  ASSERT_FALSE(data.empty());
  base::Result<VideoClip> clip = DecodeVideo(data);
  ASSERT_TRUE(clip.has_value()) << clip.error().message();
  EXPECT_EQ(clip.value().width, 8);
  EXPECT_EQ(clip.value().height, 6);
  EXPECT_GT(clip.value().duration_seconds, 2.0);
  EXPECT_NEAR(clip.value().frame_rate, 2.0, 0.1);
  EXPECT_NE(clip.value().format_name.find("mp4"), std::string::npos);
  EXPECT_EQ(clip.value().codec_name, "h264");
  // 2 fps over 3 s: at least 5 frames decoded.
  EXPECT_GE(clip.value().frames.size(), 5u);
  for (const VideoFrame& frame : clip.value().frames) {
    EXPECT_EQ(frame.image.width, 8);
    EXPECT_EQ(frame.image.height, 6);
    EXPECT_EQ(frame.image.rgba.size(), 8u * 6u * 4u);
  }
  // testsrc frames differ from each other (the moving pattern).
  EXPECT_NE(clip.value().frames.front().image.rgba, clip.value().frames.back().image.rgba);
}

TEST(MediaVideoTest, DecodesVp9Webm)
{
  const std::string data = ReadFixture("sample_4x4_vp9.webm");
  ASSERT_FALSE(data.empty());
  base::Result<VideoClip> clip = DecodeVideo(data);
  ASSERT_TRUE(clip.has_value()) << clip.error().message();
  EXPECT_EQ(clip.value().width, 4);
  EXPECT_EQ(clip.value().height, 4);
  EXPECT_GT(clip.value().duration_seconds, 1.0);
  EXPECT_NEAR(clip.value().frame_rate, 1.0, 0.1);
  EXPECT_NE(clip.value().format_name.find("webm"), std::string::npos);
  EXPECT_EQ(clip.value().codec_name, "vp9");
  EXPECT_GE(clip.value().frames.size(), 1u);
}

TEST(MediaVideoTest, StreamingSourceIteratesAllFrames)
{
  const std::string data = ReadFixture("sample_8x6_h264.mp4");
  ASSERT_FALSE(data.empty());
  base::Result<MediaSource> source = MediaSource::Open(data);
  ASSERT_TRUE(source.has_value()) << source.error().message();
  EXPECT_EQ(source.value().width(), 8);
  EXPECT_EQ(source.value().height(), 6);
  EXPECT_EQ(source.value().codec_name(), "h264");
  int frames = 0;
  double last_pts = -1;
  for (;;) {
    base::Result<std::optional<VideoFrame>> frame = source.value().NextFrame();
    ASSERT_TRUE(frame.has_value()) << frame.error().message();
    if (!frame.value().has_value()) {
      break;
    }
    ++frames;
    EXPECT_GE(frame.value()->pts_seconds, last_pts);
    last_pts = frame.value()->pts_seconds;
  }
  EXPECT_GE(frames, 5);
  // EOF is sticky.
  base::Result<std::optional<VideoFrame>> again = source.value().NextFrame();
  ASSERT_TRUE(again.has_value());
  EXPECT_FALSE(again.value().has_value());
}

TEST(MediaVideoTest, RejectsNonVideoInput)
{
  EXPECT_FALSE(DecodeVideo("this is not a video").has_value());
  const std::string empty;
  EXPECT_FALSE(MediaSource::Open(empty).has_value());
}

TEST(MediaVideoTest, RespectsFrameBudget)
{
  const std::string data = ReadFixture("sample_8x6_h264.mp4");
  ASSERT_FALSE(data.empty());
  base::Result<VideoClip> clip = DecodeVideo(data, /*max_frames=*/2);
  ASSERT_TRUE(clip.has_value()) << clip.error().message();
  EXPECT_EQ(clip.value().frames.size(), 2u);
}

} // namespace
} // namespace neko::media
