#include "neko/paint/ppm_writer.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace neko::paint {
namespace {

namespace fs = std::filesystem;

fs::path MakeTestDir(std::string_view name)
{
  const fs::path dir = fs::temp_directory_path() / "neko_ppm_writer_test" / std::string(name);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

std::string ReadAll(const fs::path& file)
{
  std::ifstream in(file, std::ios::binary);
  if (!in.is_open()) {
    return {};
  }
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Leftover temporary files ("<target>.tmp.*") next to the destination.
int CountTempFiles(const fs::path& dir)
{
  int count = 0;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.path().filename().string().find(".tmp.") != std::string::npos) {
      ++count;
    }
  }
  return count;
}

// A deterministic RGBA pattern so band boundaries are visible in the data.
std::vector<std::uint8_t> Pattern(int width, int height)
{
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height) * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<std::uint8_t>((i * 37 + 11) % 256);
  }
  return pixels;
}

TEST(PpmWriterTest, EncodesRowsAndCompositesOverWhite)
{
  const fs::path dir = MakeTestDir("encode");
  const fs::path file = dir / "out.ppm";

  auto writer = PpmWriter::Create(file.string(), 2, 2);
  ASSERT_TRUE(writer.has_value());

  // Row 0: opaque red, then green at alpha 128 (composited over white).
  // Row 1: fully transparent pixels become white.
  const std::vector<std::uint8_t> pixels = {
      255,
      0,
      0,
      255,
      0,
      255,
      0,
      128, //
      1,
      2,
      3,
      0,
      7,
      8,
      9,
      0,
  };
  ASSERT_TRUE(writer.value().AppendRows(pixels.data(), 2).has_value());
  ASSERT_TRUE(writer.value().Finish().has_value());

  const std::string content = ReadAll(file);
  ASSERT_EQ(content.size(), 11u + 2u * 2u * 3u);
  EXPECT_EQ(content.substr(0, 11), "P6\n2 2\n255\n");
  const std::vector<std::uint8_t> body(content.begin() + 11, content.end());
  const std::vector<std::uint8_t> expected = {
      255,
      0,
      0,
      127,
      255,
      127, // row 0
      255,
      255,
      255,
      255,
      255,
      255, // row 1
  };
  EXPECT_EQ(body, expected);
}

TEST(PpmWriterTest, BandedAppendsMatchASingleAppend)
{
  namespace fs = std::filesystem;
  const fs::path dir = MakeTestDir("bands");
  const int width = 19;
  const int height = 37;
  const std::vector<std::uint8_t> pixels = Pattern(width, height);

  const fs::path whole = dir / "whole.ppm";
  {
    auto writer = PpmWriter::Create(whole.string(), width, height);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value().AppendRows(pixels.data(), height).has_value());
    ASSERT_TRUE(writer.value().Finish().has_value());
  }

  const fs::path banded = dir / "banded.ppm";
  {
    auto writer = PpmWriter::Create(banded.string(), width, height);
    ASSERT_TRUE(writer.has_value());
    const int bands[] = {5, 1, 12, 19};
    int written = 0;
    for (const int rows : bands) {
      const std::uint8_t* start =
          pixels.data() + static_cast<std::size_t>(written) * static_cast<std::size_t>(width) * 4;
      ASSERT_TRUE(writer.value().AppendRows(start, rows).has_value());
      written += rows;
    }
    ASSERT_EQ(written, height);
    ASSERT_TRUE(writer.value().Finish().has_value());
  }

  EXPECT_EQ(ReadAll(whole), ReadAll(banded));
}

TEST(PpmWriterTest, DestructorWithoutFinishRemovesTemporaryFile)
{
  const fs::path dir = MakeTestDir("cleanup");
  const fs::path file = dir / "shot.ppm";
  {
    auto writer = PpmWriter::Create(file.string(), 2, 1);
    ASSERT_TRUE(writer.has_value());
    const std::vector<std::uint8_t> row = {1, 2, 3, 255, 4, 5, 6, 255};
    ASSERT_TRUE(writer.value().AppendRows(row.data(), 1).has_value());
    // No Finish(): the destructor must remove the temporary file.
  }
  EXPECT_FALSE(fs::exists(file));
  EXPECT_EQ(CountTempFiles(dir), 0);
}

TEST(PpmWriterTest, FinishReplacesAnExistingFile)
{
  const fs::path dir = MakeTestDir("replace");
  const fs::path file = dir / "shot.ppm";
  {
    std::ofstream old(file, std::ios::binary);
    old << "stale";
  }

  auto writer = PpmWriter::Create(file.string(), 1, 1);
  ASSERT_TRUE(writer.has_value());
  const std::vector<std::uint8_t> pixel = {10, 20, 30, 255};
  ASSERT_TRUE(writer.value().AppendRows(pixel.data(), 1).has_value());
  ASSERT_TRUE(writer.value().Finish().has_value());

  const std::string content = ReadAll(file);
  ASSERT_EQ(content.size(), 11u + 3u);
  EXPECT_EQ(content.substr(0, 11), "P6\n1 1\n255\n");
  EXPECT_EQ(static_cast<std::uint8_t>(content[11]), 10);
  EXPECT_EQ(static_cast<std::uint8_t>(content[12]), 20);
  EXPECT_EQ(static_cast<std::uint8_t>(content[13]), 30);
  EXPECT_EQ(CountTempFiles(dir), 0);
}

TEST(PpmWriterTest, RejectsWrongRowCounts)
{
  const fs::path dir = MakeTestDir("rows");
  const fs::path file = dir / "shot.ppm";
  const std::vector<std::uint8_t> row = {1, 2, 3, 255, 4, 5, 6, 255};
  {
    auto writer = PpmWriter::Create(file.string(), 2, 2);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value().AppendRows(row.data(), 1).has_value());
    // Finishing with fewer rows than declared fails...
    EXPECT_FALSE(writer.value().Finish().has_value());
    // ...and so does overshooting the declared height.
    EXPECT_FALSE(writer.value().AppendRows(row.data(), 2).has_value());
  }
  // The failed writer must not leave anything behind.
  EXPECT_FALSE(fs::exists(file));
  EXPECT_EQ(CountTempFiles(dir), 0);
}

TEST(PpmWriterTest, RejectsInvalidArguments)
{
  const fs::path dir = MakeTestDir("invalid");
  const fs::path file = dir / "shot.ppm";
  EXPECT_FALSE(PpmWriter::Create(file.string(), 0, 1).has_value());
  EXPECT_FALSE(PpmWriter::Create(file.string(), 1, -1).has_value());
  EXPECT_FALSE(PpmWriter::Create("", 1, 1).has_value());
}

} // namespace
} // namespace neko::paint
