// Unit tests for neko::image.
//
// PNG test inputs are produced by a tiny in-test PNG encoder (zlib compress
// for IDAT, crc32 for chunk CRCs); JPEG test inputs are produced by libjpeg
// itself, giving a real encode -> decode round trip.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <jpeglib.h>
#include <zlib.h>

#include <gtest/gtest.h>

#include "neko/image/image.h"

namespace neko::image {
namespace {

// ---------------------------------------------------------------------------
// Minimal PNG encoder (test-only)
// ---------------------------------------------------------------------------

std::string Be32(uint32_t v) {
  std::string out(4, '\0');
  out[0] = static_cast<char>((v >> 24) & 0xFF);
  out[1] = static_cast<char>((v >> 16) & 0xFF);
  out[2] = static_cast<char>((v >> 8) & 0xFF);
  out[3] = static_cast<char>(v & 0xFF);
  return out;
}

void AppendChunk(std::string& out, const char* type, std::string_view data) {
  out += Be32(static_cast<uint32_t>(data.size()));
  out.append(type, 4);
  const size_t crc_start = out.size();
  out.append(data);
  uLong crc = crc32(0L, reinterpret_cast<const Bytef*>(out.data() + crc_start - 4),
                    static_cast<uInt>(4 + data.size()));
  out += Be32(static_cast<uint32_t>(crc));
}

// Builds a PNG file from already-filtered packed scanlines.
std::string EncodePng(int width, int height, int bit_depth, int color_type,
                      std::string_view filtered_scanlines, bool interlace = false,
                      std::string_view palette = {}, std::string_view trns = {}) {
  std::string ihdr;
  ihdr += Be32(static_cast<uint32_t>(width));
  ihdr += Be32(static_cast<uint32_t>(height));
  ihdr.push_back(static_cast<char>(bit_depth));
  ihdr.push_back(static_cast<char>(color_type));
  ihdr.push_back(0);  // compression
  ihdr.push_back(0);  // filter
  ihdr.push_back(static_cast<char>(interlace ? 1 : 0));

  std::string out = "\x89PNG\r\n\x1a\n";
  AppendChunk(out, "IHDR", ihdr);
  if (!palette.empty()) AppendChunk(out, "PLTE", palette);
  if (!trns.empty()) AppendChunk(out, "tRNS", trns);
  // Deflate the scanlines.
  uLongf bound = compressBound(static_cast<uLong>(filtered_scanlines.size()));
  std::vector<Bytef> compressed(bound);
  uLongf compressed_size = bound;
  if (compress2(compressed.data(), &compressed_size,
                reinterpret_cast<const Bytef*>(filtered_scanlines.data()),
                static_cast<uLong>(filtered_scanlines.size()), 9) != Z_OK) {
    return {};
  }
  AppendChunk(out, "IDAT",
              std::string_view(reinterpret_cast<const char*>(compressed.data()),
                               compressed_size));
  AppendChunk(out, "IEND", "");
  return out;
}

// Prefixes each packed row with a filter byte (|filter| 0..4).
std::string FilterRows(std::string_view packed_rows, int row_bytes, int bpp,
                       int filter) {
  std::string out;
  const int height = static_cast<int>(packed_rows.size()) / row_bytes;
  std::vector<uint8_t> prev(static_cast<size_t>(row_bytes), 0);
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = reinterpret_cast<const uint8_t*>(packed_rows.data()) +
                         static_cast<size_t>(y) * static_cast<size_t>(row_bytes);
    out.push_back(static_cast<char>(filter));
    for (int i = 0; i < row_bytes; ++i) {
      const size_t ui = static_cast<size_t>(i);
      const bool has_left = ui >= static_cast<size_t>(bpp);
      const int a = has_left ? row[ui - static_cast<size_t>(bpp)] : 0;
      const int b = prev[ui];
      const int c = has_left ? prev[ui - static_cast<size_t>(bpp)] : 0;
      int value = 0;
      switch (filter) {
        case 0: value = row[ui]; break;
        case 1: value = row[ui] - a; break;
        case 2: value = row[ui] - b; break;
        case 3: value = row[ui] - (a + b) / 2; break;
        case 4: {
          const int p = a + b - c;
          const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
          const int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
          value = row[ui] - pr;
          break;
        }
      }
      out.push_back(static_cast<char>(value & 0xFF));
    }
    std::copy(row, row + row_bytes, prev.begin());
  }
  return out;
}

// ---------------------------------------------------------------------------
// Adam7 interlace helper (8-bit RGBA only; enough to exercise the decoder)
// ---------------------------------------------------------------------------

struct Adam7PassGeom {
  int xs, ys, xst, yst;
};
constexpr Adam7PassGeom kPasses[7] = {
    {0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
    {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2},
};

int PassSize(int total, int start, int step) {
  if (total <= start) return 0;
  return (total - start + step - 1) / step;
}

// Builds the concatenated, filtered scanline stream for an Adam7-interlaced
// 8-bit RGBA image (filter 0 everywhere).
std::string InterlacedScanlines(int width, int height,
                                const std::vector<uint8_t>& rgba) {
  std::string out;
  const size_t width_u = static_cast<size_t>(width);
  for (const Adam7PassGeom& pass : kPasses) {
    const int pw = PassSize(width, pass.xs, pass.xst);
    const int ph = PassSize(height, pass.ys, pass.yst);
    if (pw == 0 || ph == 0) continue;
    const size_t pw_u = static_cast<size_t>(pw);
    std::vector<uint8_t> pass_pixels(pw_u * static_cast<size_t>(ph) * 4);
    for (int py = 0; py < ph; ++py) {
      for (int px = 0; px < pw; ++px) {
        const size_t ox = static_cast<size_t>(pass.xs) + static_cast<size_t>(px) * static_cast<size_t>(pass.xst);
        const size_t oy = static_cast<size_t>(pass.ys) + static_cast<size_t>(py) * static_cast<size_t>(pass.yst);
        const std::ptrdiff_t src = static_cast<std::ptrdiff_t>((oy * width_u + ox) * 4);
        const std::ptrdiff_t dst = static_cast<std::ptrdiff_t>((static_cast<size_t>(py) * pw_u + static_cast<size_t>(px)) * 4);
        std::copy(rgba.begin() + src, rgba.begin() + src + 4, pass_pixels.begin() + dst);
      }
    }
    for (int py = 0; py < ph; ++py) {
      out.push_back(0);
      out.append(reinterpret_cast<const char*>(pass_pixels.data()) +
                     static_cast<size_t>(py) * pw_u * 4,
                 pw_u * 4);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Minimal JPEG encoder (test-only, libjpeg)
// ---------------------------------------------------------------------------

std::string EncodeJpeg(int width, int height, const std::vector<uint8_t>& rgb) {
  jpeg_compress_struct cinfo{};
  jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  unsigned char* buffer = nullptr;
  unsigned long outsize = 0;
  jpeg_mem_dest(&cinfo, &buffer, &outsize);

  cinfo.image_width = static_cast<JDIMENSION>(width);
  cinfo.image_height = static_cast<JDIMENSION>(height);
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, 95, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  std::vector<unsigned char> row(static_cast<size_t>(width) * 3);
  while (cinfo.next_scanline < cinfo.image_height) {
    const size_t y = cinfo.next_scanline;
    std::copy(rgb.begin() + static_cast<long>(y) * width * 3,
              rgb.begin() + static_cast<long>(y + 1) * width * 3, row.begin());
    unsigned char* row_ptr = row.data();
    jpeg_write_scanlines(&cinfo, &row_ptr, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);

  std::string out(reinterpret_cast<char*>(buffer), outsize);
  std::free(buffer);
  return out;
}

// Asserts that decoded RGBA matches an expected 4-channel buffer.
void ExpectPixels(const Image& img, const std::vector<uint8_t>& expected,
                  int tolerance = 0) {
  ASSERT_EQ(img.width * img.height * 4, static_cast<int>(expected.size()));
  const size_t width = static_cast<size_t>(img.width);
  for (size_t i = 0; i < expected.size(); ++i) {
    const int diff = std::abs(static_cast<int>(img.rgba[i]) - static_cast<int>(expected[i]));
    EXPECT_LE(diff, tolerance) << "channel byte " << i << " (x=" << (i / 4) % width
                               << ", y=" << (i / 4) / width << ", c=" << i % 4 << ")";
  }
}

// ---------------------------------------------------------------------------
// PNG decoding
// ---------------------------------------------------------------------------

TEST(PngTest, DecodesRgb8FilterNone) {
  // 2x2 RGB: red, green / blue, white
  std::vector<uint8_t> pixels = {
      255, 0, 0,    0, 255, 0,     //
      0, 0, 255,    255, 255, 255,  //
  };
  const std::string rows = FilterRows(std::string_view(reinterpret_cast<const char*>(pixels.data()), pixels.size()), 6, 3, 0);
  const std::string png = EncodePng(2, 2, 8, 2, rows);
  ASSERT_FALSE(png.empty());
  ASSERT_TRUE(IsPng(png));

  auto result = DecodePng(png);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  const Image& img = result.value();
  ASSERT_EQ(img.width, 2);
  ASSERT_EQ(img.height, 2);
  std::vector<uint8_t> expected = {
      255, 0, 0, 255,      0, 255, 0, 255,   //
      0, 0, 255, 255,      255, 255, 255, 255,  //
  };
  ExpectPixels(img, expected);
}

TEST(PngTest, DecodesRgba8FilterSub) {
  // 3x1 RGBA with varying alpha
  std::vector<uint8_t> pixels = {
      10, 20, 30, 255,  40, 50, 60, 128,  70, 80, 90, 0,
  };
  const std::string rows = FilterRows(
      std::string_view(reinterpret_cast<const char*>(pixels.data()), pixels.size()), 12, 4, 1);
  const std::string png = EncodePng(3, 1, 8, 6, rows);
  ASSERT_FALSE(png.empty());
  auto result = DecodePng(png);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ExpectPixels(result.value(), pixels);
}

TEST(PngTest, DecodesRgba8FilterPaeth) {
  std::vector<uint8_t> pixels = {
      1, 2, 3, 4,      5, 6, 7, 8,      9, 10, 11, 12,  //
      13, 14, 15, 16,  17, 18, 19, 20,  21, 22, 23, 24,  //
  };
  const std::string rows = FilterRows(
      std::string_view(reinterpret_cast<const char*>(pixels.data()), pixels.size()), 12, 4, 4);
  const std::string png = EncodePng(3, 2, 8, 6, rows);
  ASSERT_FALSE(png.empty());
  auto result = DecodePng(png);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ExpectPixels(result.value(), pixels);
}

TEST(PngTest, DecodesFilterUp) {
  std::vector<uint8_t> pixels = {
      100, 101, 102, 255,  103, 104, 105, 255,  //
      200, 201, 202, 255,  203, 204, 205, 255,  //
  };
  const std::string rows = FilterRows(
      std::string_view(reinterpret_cast<const char*>(pixels.data()), pixels.size()), 8, 4, 2);
  const std::string png = EncodePng(2, 2, 8, 6, rows);
  auto result = DecodePng(png);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ExpectPixels(result.value(), pixels);
}

TEST(PngTest, DecodesPaletteWithTransparency) {
  // 2x2 palette image, 2 entries: blue and red.
  std::vector<uint8_t> index_pixels = {0, 1, 1, 0};
  std::vector<uint8_t> rows;
  for (int y = 0; y < 2; ++y) {
    rows.push_back(0);  // filter None
    rows.push_back(index_pixels[static_cast<size_t>(y) * 2 + 0]);
    rows.push_back(index_pixels[static_cast<size_t>(y) * 2 + 1]);
  }
  const std::string palette = std::string("\x00\x00\xff", 3) + std::string("\xff\x00\x00", 3);
  const std::string trns = std::string("\x80\xff", 2);  // blue 50% alpha, red opaque
  const std::string_view rows_view(reinterpret_cast<const char*>(rows.data()), rows.size());
  const std::string png = EncodePng(2, 2, 8, 3, rows_view, false, palette, trns);
  ASSERT_FALSE(png.empty());
  auto result = DecodePng(png);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  std::vector<uint8_t> expected = {
      0, 0, 255, 128,    255, 0, 0, 255,   //
      255, 0, 0, 255,    0, 0, 255, 128,   //
  };
  ExpectPixels(result.value(), expected);
}

TEST(PngTest, Decodes1BitGrayscale) {
  // 8x1: alternating black/white
  std::vector<uint8_t> packed;
  packed.push_back(0);          // filter
  packed.push_back(0b10101010);  // bits: 1,0,1,0,1,0,1,0
  const std::string_view packed_view(reinterpret_cast<const char*>(packed.data()),
                                     packed.size());
  const std::string png = EncodePng(8, 1, 1, 0, packed_view);
  auto result = DecodePng(png);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ASSERT_EQ(result.value().width, 8);
  std::vector<uint8_t> expected;
  for (int x = 0; x < 8; ++x) {
    const uint8_t v = (x % 2 == 0) ? 255 : 0;
    expected.insert(expected.end(), {v, v, v, 255});
  }
  ExpectPixels(result.value(), expected);
}

TEST(PngTest, Decodes4BitGrayscale) {
  // 2x1: values 0 and 15 -> 0 and 255
  std::vector<uint8_t> packed = {0, static_cast<uint8_t>(0x0F)};
  const std::string_view packed_view(reinterpret_cast<const char*>(packed.data()),
                                     packed.size());
  const std::string png = EncodePng(2, 1, 4, 0, packed_view);
  auto result = DecodePng(png);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  std::vector<uint8_t> expected = {0, 0, 0, 255, 255, 255, 255, 255};
  ExpectPixels(result.value(), expected);
}

TEST(PngTest, Decodes16BitRgb) {
  // 1x2, 16-bit samples (big-endian): (65535,0,0) and (0,65535,0)
  std::vector<uint8_t> pixels = {
      0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,  //
      0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00,  //
  };
  const std::string rows = FilterRows(
      std::string_view(reinterpret_cast<const char*>(pixels.data()), pixels.size()), 6, 6, 0);
  const std::string png = EncodePng(1, 2, 16, 2, rows);
  auto result = DecodePng(png);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  std::vector<uint8_t> expected = {
      255, 0, 0, 255,  //
      0, 255, 0, 255,  //
  };
  ExpectPixels(result.value(), expected);
}

TEST(PngTest, DecodesAdam7Interlaced) {
  // 5x5 RGBA with a deterministic gradient; the interlaced stream is built
  // from all seven Adam7 passes and must reassemble to the original image.
  const int w = 5, h = 5;
  std::vector<uint8_t> pixels;
  pixels.reserve(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      pixels.insert(pixels.end(), {static_cast<uint8_t>(x * 40),
                                   static_cast<uint8_t>(y * 40), 128, 255});
    }
  }
  const std::string interlaced = InterlacedScanlines(w, h, pixels);
  const std::string png = EncodePng(w, h, 8, 6, interlaced, /*interlace=*/true);
  ASSERT_FALSE(png.empty());
  auto result = DecodePng(png);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ExpectPixels(result.value(), pixels);
}

TEST(PngTest, RejectsCrcMismatch) {
  const std::string rows = FilterRows("\x01\x02\x03", 3, 3, 0);
  std::string png = EncodePng(1, 1, 8, 2, rows);
  ASSERT_FALSE(png.empty());
  // Flip a byte inside the IDAT data (after the 8-byte signature + 8-byte
  // IHDR header + 25 bytes IHDR chunk + 4 CRC = 45; IDAT data starts there).
  // Simpler: corrupt a byte in the middle and expect a CRC or inflate error.
  png[png.size() / 2] ^= 0x01;
  auto result = DecodePng(png);
  EXPECT_FALSE(result.has_value());
}

TEST(PngTest, RejectsTruncatedFile) {
  const std::string rows = FilterRows("\x01\x02\x03", 3, 3, 0);
  const std::string png = EncodePng(1, 1, 8, 2, rows);
  EXPECT_FALSE(DecodePng(png.substr(0, 20)).has_value());
}

TEST(PngTest, RejectsNonPng) {
  EXPECT_FALSE(DecodePng("not a png file at all").has_value());
}

TEST(PngTest, RejectsBadDimensions) {
  // Hand-build a PNG whose IHDR declares width 0.
  std::string ihdr = Be32(0) + Be32(1) + std::string("\x08\x02\x00\x00\x00", 5);
  std::string png = "\x89PNG\r\n\x1a\n";
  AppendChunk(png, "IHDR", ihdr);
  AppendChunk(png, "IEND", "");
  EXPECT_FALSE(DecodePng(png).has_value());
}

// ---------------------------------------------------------------------------
// JPEG decoding (libjpeg round trip)
// ---------------------------------------------------------------------------

TEST(JpegTest, RoundTripsRgbImage) {
  const int w = 4, h = 3;
  std::vector<uint8_t> rgb;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      rgb.insert(rgb.end(), {static_cast<uint8_t>(x * 60), static_cast<uint8_t>(y * 80),
                             static_cast<uint8_t>(128)});
    }
  }
  const std::string jpeg = EncodeJpeg(w, h, rgb);
  ASSERT_FALSE(jpeg.empty());
  ASSERT_TRUE(IsJpeg(jpeg));

  auto result = DecodeJpeg(jpeg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  const Image& img = result.value();
  ASSERT_EQ(img.width, w);
  ASSERT_EQ(img.height, h);

  // JPEG is lossy; small 4x3 tiles deviate more than typical images.
  std::vector<uint8_t> expected;
  for (size_t i = 0; i < rgb.size(); i += 3) {
    expected.insert(expected.end(), {rgb[i], rgb[i + 1], rgb[i + 2], 255});
  }
  ExpectPixels(img, expected, 48);
}

TEST(JpegTest, RejectsGarbage) {
  EXPECT_FALSE(DecodeJpeg("garbage that is not a jpeg").has_value());
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

TEST(DecodeImageTest, DispatchesByMagic) {
  const std::string rows = FilterRows("\x01\x02\x03", 3, 3, 0);
  const std::string png = EncodePng(1, 1, 8, 2, rows);
  ASSERT_FALSE(png.empty());
  EXPECT_TRUE(DecodeImage(png).has_value());

  const std::string jpeg = EncodeJpeg(2, 2, {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255});
  ASSERT_FALSE(jpeg.empty());
  EXPECT_TRUE(DecodeImage(jpeg).has_value());

  const auto unknown = DecodeImage("some other format");
  EXPECT_FALSE(unknown.has_value());
  EXPECT_EQ(unknown.error().category(), base::ErrorCategory::kNotImplemented);
}

}  // namespace
}  // namespace neko::image
