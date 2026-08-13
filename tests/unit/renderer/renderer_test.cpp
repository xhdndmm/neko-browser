#include "neko/renderer/page.h"
#include "neko/paint/rasterizer.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "neko/html/parser.h"
#include <gtest/gtest.h>

namespace neko::renderer {
namespace {

TEST(PageTest, LoadAndDumpDom) {
  Page page;
  const auto result = page.LoadHtml("<html><head><title>T</title></head><body><p>hi</p></body></html>");
  ASSERT_TRUE(result.has_value());
  ASSERT_NE(page.document(), nullptr);
  EXPECT_EQ(page.document()->Title(), "T");
  EXPECT_NE(page.DumpDom().find("<p>hi</p>"), std::string::npos);
}

TEST(PageTest, LayoutAndDump) {
  Page page;
  ASSERT_TRUE(page.LoadHtml("<body><div>text</div></body>").has_value());
  page.Layout(800);
  ASSERT_NE(page.layout_root(), nullptr);
  const std::string dump = page.DumpLayoutTree();
  EXPECT_NE(dump.find("<body>"), std::string::npos);
  EXPECT_NE(dump.find("<div>"), std::string::npos);
}

TEST(PageTest, RasterizeProducesImage) {
  Page page;
  ASSERT_TRUE(page.LoadHtml(
                          "<body style=\"background-color:#ffffff\">"
                          "<div style=\"background-color:#ff0000;width:100px;height:50px\">x</div>"
                          "</body>")
                  .has_value());
  page.Layout(400);
  paint::Rasterizer image = page.Rasterize(400, 300);
  EXPECT_EQ(image.width(), 400);
  EXPECT_EQ(image.height(), 300);

  // The red div is at (8,8) sized 100x50.
  const std::size_t offset = (static_cast<std::size_t>(20) * 400 + 50) * 4;
  EXPECT_EQ(image.pixels()[offset], 255);
  EXPECT_EQ(image.pixels()[offset + 1], 0);
  EXPECT_EQ(image.pixels()[offset + 2], 0);
}

TEST(PageTest, LoadMissingFile) {
  Page page;
  const auto result = page.LoadFile("/nonexistent/neko_missing_file.html");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kIo);
}

TEST(PageTest, LoadFile) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "neko_renderer_test";
  fs::create_directories(dir);
  const fs::path file = dir / "page.html";
  {
    std::ofstream out(file);
    out << "<body><p>from file</p></body>";
  }
  Page page;
  const auto result = page.LoadFile(file.string());
  ASSERT_TRUE(result.has_value());
  EXPECT_NE(page.DumpDom().find("from file"), std::string::npos);
  fs::remove_all(dir);
}

}  // namespace
}  // namespace neko::renderer
