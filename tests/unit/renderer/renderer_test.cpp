#include "neko/renderer/page.h"
#include "neko/paint/rasterizer.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include "neko/dom/query.h"
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

TEST(PageTest, ContentHeight) {
  Page page;
  EXPECT_EQ(page.ContentHeight(), 0.0f);  // no layout yet

  ASSERT_TRUE(page.LoadHtml("<body><div style=\"height:500px\">x</div></body>").has_value());
  EXPECT_EQ(page.ContentHeight(), 0.0f);  // loaded but not laid out yet

  page.Layout(400);
  // body default margin (8px top + 8px bottom) plus the 500px div.
  EXPECT_FLOAT_EQ(page.ContentHeight(), 516.0f);
}

TEST(PageTest, RasterizeScrollsContent) {
  Page page;
  ASSERT_TRUE(page.LoadHtml(
                          "<body style=\"background-color:#ffffff\">"
                          "<div style=\"background-color:#ff0000;width:100px;height:50px\">x</div>"
                          "</body>")
                  .has_value());
  page.Layout(400);

  const auto pixel = [](const paint::Rasterizer& image, int x, int y) {
    const std::size_t offset =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width()) +
         static_cast<std::size_t>(x)) *
        4;
    return css::Color{image.pixels()[offset], image.pixels()[offset + 1],
                      image.pixels()[offset + 2], image.pixels()[offset + 3]};
  };

  const paint::Rasterizer top = page.Rasterize(400, 300, 0);
  const paint::Rasterizer scrolled = page.Rasterize(400, 300, 20);

  // The red div spans y=8..58 at offset 0; at offset 20 it moves up to y=0..38.
  EXPECT_EQ(pixel(top, 50, 20), (css::Color{255, 0, 0, 255}));
  EXPECT_EQ(pixel(scrolled, 50, 20), (css::Color{255, 0, 0, 255}));
  EXPECT_EQ(pixel(top, 50, 50), (css::Color{255, 0, 0, 255}));
  EXPECT_EQ(pixel(scrolled, 50, 50), (css::Color{255, 255, 255, 255}));
  EXPECT_EQ(pixel(top, 50, 0), (css::Color{255, 255, 255, 255}));
  EXPECT_EQ(pixel(scrolled, 50, 0), (css::Color{255, 0, 0, 255}));
}

TEST(PageTest, LoadMissingFile) {
  Page page;
  const auto result = page.LoadFile("/nonexistent/neko_missing_file.html");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kIo);
}

TEST(PageTest, ElementAtHitTestsInlineContent) {
  Page page;
  ASSERT_TRUE(page.LoadHtml(
      "<body><p>plain <a href=\"https://example.com/x\">link</a></p></body>")
                  .has_value());
  page.Layout(400);

  dom::Element* link = dom::QuerySelector(*page.document(), "a");
  dom::Element* p = dom::QuerySelector(*page.document(), "p");
  ASSERT_NE(link, nullptr);
  ASSERT_NE(p, nullptr);

  // Click the center of each text run and check which element is hit.
  const dom::Element* hit_link = nullptr;
  const dom::Element* hit_plain = nullptr;
  std::function<void(const layout::LayoutBox&)> walk = [&](const layout::LayoutBox& box) {
    for (const layout::Line& line : box.lines) {
      for (const layout::TextRun& run : line.runs) {
        // Use the run's measured width (real font advances when available).
        const float cx = run.x + run.width / 2.0f;
        const float cy = run.y + run.font_size / 2.0f;
        const dom::Element* hit = page.ElementAt(cx, cy);
        if (run.element == link && run.text == "link") {
          hit_link = hit;
        } else if (run.element == p && run.text == "plain") {
          hit_plain = hit;
        }
      }
    }
    for (const auto& child : box.children) {
      walk(*child);
    }
  };
  walk(*page.layout_root());

  ASSERT_NE(hit_link, nullptr);
  EXPECT_EQ(hit_link, static_cast<const dom::Element*>(link));
  ASSERT_NE(hit_plain, nullptr);
  EXPECT_EQ(hit_plain, static_cast<const dom::Element*>(p));
}

TEST(PageTest, ElementAtOutsideContentReturnsNull) {
  Page page;
  ASSERT_TRUE(page.LoadHtml("<body><p>hi</p></body>").has_value());
  page.Layout(400);
  // Far below the laid-out content (and outside the root box) -> no element.
  EXPECT_EQ(page.ElementAt(10.0f, 100000.0f), nullptr);

  // No layout tree yet -> nullptr regardless of the point.
  Page empty;
  ASSERT_TRUE(empty.LoadHtml("<body><p>hi</p></body>").has_value());
  EXPECT_EQ(empty.ElementAt(10.0f, 20.0f), nullptr);
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
