// Golden-image rendering regression (AGENTS.md §33).
//
// Renders a committed HTML fixture through the full engine path
// (parse → style → layout → paint → raster) and compares the raster against
// a committed golden PPM pixel by pixel.  This is the layer above the
// point-probes in renderer_test.cpp: instead of sampling a few coordinates,
// the whole frame is pinned, so any visual change anywhere in the frame is
// either an approved golden update or a failing test.
//
// Determinism rules for fixtures in tests/golden/:
//   * no text (fonts differ per machine) and no external resources;
//   * integer-friendly geometry (fixed px sizes; avoid fractional layouts
//     whose rounding could differ between toolchains).
// Goldens are written through paint::WritePpm (RGB888, alpha composited over
// white), so a "pixel" here is exactly what `neko_browser --screenshot`
// produces.
//
// To (re)generate goldens after an intentional rendering change:
//   NEKO_UPDATE_GOLDEN=1 ctest -R GoldenImage
// then inspect the diff in git before committing.  On mismatch the actual
// frame and a diff image are written to the gtest temp dir for triage.
//
// A fixture is declared by a metadata comment in its HTML:
//   <!-- neko-golden: 320x240 -->          viewport width x height
//   <!-- neko-golden: 320x240 tolerance=2 -->  per-channel slack (default 0)

#include "neko/paint/ppm_writer.h"
#include "neko/paint/rasterizer.h"
#include "neko/renderer/page.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace neko::renderer {
namespace {

struct RgbImage
{
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgb; // width * height * 3
};

// Minimal P6 reader for goldens written by paint::WritePpm.
std::optional<RgbImage> ReadPpm(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::string magic;
  int width = 0;
  int height = 0;
  int maxval = 0;
  in >> magic >> width >> height >> maxval;
  if (magic != "P6" || width <= 0 || height <= 0 || maxval != 255 || !in) {
    return std::nullopt;
  }
  in.get(); // single whitespace after the header
  RgbImage image;
  image.width = width;
  image.height = height;
  image.rgb.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
  in.read(reinterpret_cast<char*>(image.rgb.data()),
          static_cast<std::streamsize>(image.rgb.size()));
  if (!in) {
    return std::nullopt;
  }
  return image;
}

struct GoldenSpec
{
  int width = 0;
  int height = 0;
  int tolerance = 0; // max per-channel absolute difference
};

// Parses "<!-- neko-golden: WxH [tolerance=N] -->".
std::optional<GoldenSpec> ParseSpec(const std::string& html)
{
  const std::size_t pos = html.find("neko-golden:");
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  std::istringstream fields(html.substr(pos + 12, html.find("-->", pos) - pos - 12));
  GoldenSpec spec;
  std::string size;
  fields >> size;
  const std::size_t x = size.find('x');
  if (x == std::string::npos) {
    return std::nullopt;
  }
  try {
    spec.width = std::stoi(size.substr(0, x));
    spec.height = std::stoi(size.substr(x + 1));
  } catch (...) {
    return std::nullopt;
  }
  std::string key;
  while (fields >> key) {
    if (key.rfind("tolerance=", 0) == 0) {
      spec.tolerance = std::stoi(key.substr(10));
    }
  }
  if (spec.width <= 0 || spec.height <= 0) {
    return std::nullopt;
  }
  return spec;
}

std::string ReadFile(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Every fixture below must stay text-free and self-contained; see the
// determinism rules at the top of this file.
constexpr const char* kFixtures[] = {
    "boxes",       // box model: margin/padding/border/background, box-sizing
    "flex",        // flex rows/columns: grow/shrink, gap, alignment, reverse
    "grid",        // grid tracks, fr units, gaps
    "positioning", // absolute placement + z-index paint order
    "float",       // float stacking, wrapping room, clear
    "opacity",     // group opacity: subtree composites as one unit
};

class GoldenImageTest : public testing::TestWithParam<const char*>
{
};

TEST_P(GoldenImageTest, MatchesGoldenRaster)
{
  const std::string name = GetParam();
  const std::filesystem::path dir(NEKO_TEST_GOLDEN_DIR);
  const std::string html = ReadFile(dir / (name + ".html"));
  ASSERT_FALSE(html.empty()) << "missing fixture " << (dir / (name + ".html"));

  const std::optional<GoldenSpec> spec = ParseSpec(html);
  ASSERT_TRUE(spec.has_value()) << "fixture lacks a <!-- neko-golden: WxH --> header";

  Page page;
  ASSERT_TRUE(page.LoadHtml(html).has_value());
  page.Layout(static_cast<float>(spec->width));
  const paint::Rasterizer raster = page.Rasterize(spec->width, spec->height);
  ASSERT_EQ(raster.width(), spec->width);
  ASSERT_EQ(raster.height(), spec->height);

  const std::filesystem::path golden_path = dir / (name + ".ppm");
  const std::filesystem::path actual_path =
      std::filesystem::path(::testing::TempDir()) / (name + ".actual.ppm");

  // The comparison basis is the committed golden's own encoding: route the
  // actual frame through paint::WritePpm so "different" can never mean "the
  // test composites alpha differently than the screenshot writer".
  ASSERT_TRUE(paint::WritePpm(actual_path.string(), raster).has_value());

  const char* update = std::getenv("NEKO_UPDATE_GOLDEN");
  if (update != nullptr && std::string(update) == "1") {
    ASSERT_TRUE(paint::WritePpm(golden_path.string(), raster).has_value());
    GTEST_SKIP() << "golden updated: " << golden_path;
  }

  const auto golden = ReadPpm(golden_path);
  ASSERT_TRUE(golden.has_value()) << "missing/unreadable golden " << golden_path
                                  << " (run with NEKO_UPDATE_GOLDEN=1 to create)";
  const auto actual = ReadPpm(actual_path);
  ASSERT_TRUE(actual.has_value());
  ASSERT_EQ(actual->width, golden->width);
  ASSERT_EQ(actual->height, golden->height);

  std::size_t bad_pixels = 0;
  int max_delta = 0;
  int first_bad_x = -1;
  int first_bad_y = -1;
  for (int y = 0; y < golden->height; ++y) {
    for (int x = 0; x < golden->width; ++x) {
      const std::size_t o = (static_cast<std::size_t>(y) * static_cast<std::size_t>(golden->width) +
                             static_cast<std::size_t>(x)) *
                            3;
      int pixel_delta = 0;
      for (int c = 0; c < 3; ++c) {
        const int delta = std::abs(static_cast<int>(golden->rgb[o + static_cast<std::size_t>(c)]) -
                                   static_cast<int>(actual->rgb[o + static_cast<std::size_t>(c)]));
        pixel_delta = std::max(pixel_delta, delta);
      }
      max_delta = std::max(max_delta, pixel_delta);
      if (pixel_delta > spec->tolerance) {
        if (bad_pixels == 0) {
          first_bad_x = x;
          first_bad_y = y;
        }
        ++bad_pixels;
      }
    }
  }

  if (bad_pixels > 0) {
    const std::filesystem::path diff_path =
        std::filesystem::path(::testing::TempDir()) / (name + ".diff.ppm");
    std::filesystem::copy_file(
        actual_path, diff_path, std::filesystem::copy_options::overwrite_existing);
    FAIL() << bad_pixels << " pixel(s) differ from golden " << golden_path << " (max channel delta "
           << max_delta << ", tolerance " << spec->tolerance << "), first at (" << first_bad_x
           << "," << first_bad_y << "); actual frame kept at " << actual_path << ", copy at "
           << diff_path << " — inspect, then NEKO_UPDATE_GOLDEN=1 if the change is intended";
  }
}

INSTANTIATE_TEST_SUITE_P(AllFixtures,
                         GoldenImageTest,
                         testing::ValuesIn(kFixtures),
                         [](const testing::TestParamInfo<const char*>& info) {
                           return std::string(info.param);
                         });

// Regenerating goldens must never silently drop fixtures: a fixture without a
// golden is caught here rather than by an accidental skip in the comparison.
TEST(GoldenImageInventoryTest, EveryFixtureHasAGolden)
{
  const std::filesystem::path dir(NEKO_TEST_GOLDEN_DIR);
  for (const char* name : kFixtures) {
    const std::string base = name;
    EXPECT_TRUE(std::filesystem::is_regular_file(dir / (base + ".html"))) << name;
    EXPECT_TRUE(std::filesystem::is_regular_file(dir / (base + ".ppm"))) << name;
  }
}

} // namespace
} // namespace neko::renderer
