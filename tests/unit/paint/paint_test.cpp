#include "neko/paint/painter.h"
#include "neko/paint/rasterizer.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "neko/dom/query.h"
#include "neko/html/parser.h"
#include "neko/layout/layout_tree.h"
#include "neko/style/style_engine.h"
#include <gtest/gtest.h>

namespace neko::paint {
namespace {

css::Color Pixel(const Rasterizer& image, int x, int y) {
  const std::size_t offset =
      (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width()) +
       static_cast<std::size_t>(x)) *
      4;
  return css::Color{image.pixels()[offset], image.pixels()[offset + 1],
                    image.pixels()[offset + 2], image.pixels()[offset + 3]};
}

TEST(RasterizerTest, FillRect) {
  Rasterizer image(20, 20);
  image.Clear(css::Color{255, 255, 255, 255});
  DisplayList list;
  list.FillRect(2, 2, 10, 10, css::Color{255, 0, 0, 255});
  image.Rasterize(list);

  EXPECT_EQ(Pixel(image, 5, 5), (css::Color{255, 0, 0, 255}));
  EXPECT_EQ(Pixel(image, 0, 0), (css::Color{255, 255, 255, 255}));
  EXPECT_EQ(Pixel(image, 15, 15), (css::Color{255, 255, 255, 255}));
}

TEST(RasterizerTest, SetScrollOffset) {
  Rasterizer image(20, 20);
  image.Clear(css::Color{255, 255, 255, 255});
  DisplayList list;
  list.FillRect(2, 2, 10, 10, css::Color{255, 0, 0, 255});
  image.SetScrollOffset(5.0f);
  image.Rasterize(list);

  // The rect (y=2..12) shifts up by 5 to y=-3..7, i.e. visible at y=0..6.
  EXPECT_EQ(Pixel(image, 5, 1), (css::Color{255, 0, 0, 255}));   // newly revealed top
  EXPECT_EQ(Pixel(image, 5, 9), (css::Color{255, 255, 255, 255}));  // scrolled-out bottom
}

TEST(RasterizerTest, SetScrollOffsetClipsScrolledOutContent) {
  Rasterizer image(20, 20);
  image.Clear(css::Color{255, 255, 255, 255});
  DisplayList list;
  list.FillRect(0, 0, 20, 20, css::Color{255, 0, 0, 255});
  image.SetScrollOffset(40.0f);  // fully scrolled out
  image.Rasterize(list);
  EXPECT_EQ(Pixel(image, 10, 10), (css::Color{255, 255, 255, 255}));
}

TEST(RasterizerTest, AlphaBlend) {
  Rasterizer image(10, 10);
  image.Clear(css::Color{255, 255, 255, 255});
  DisplayList list;
  // 50% red over white -> ~(255, 128, 128).
  list.FillRect(0, 0, 10, 10, css::Color{255, 0, 0, 128});
  image.Rasterize(list);
  const css::Color p = Pixel(image, 5, 5);
  EXPECT_GE(p.r, 200);
  EXPECT_LE(p.g, 160);
  EXPECT_LE(p.b, 160);
}

TEST(RasterizerTest, BorderRect) {
  Rasterizer image(20, 20);
  image.Clear(css::Color{255, 255, 255, 255});
  DisplayList list;
  list.BorderRect(2, 2, 10, 10, 2, 2, 2, 2, css::Color{0, 0, 0, 255});
  image.Rasterize(list);

  EXPECT_EQ(Pixel(image, 3, 3), (css::Color{0, 0, 0, 255}));   // top border
  EXPECT_EQ(Pixel(image, 5, 5), (css::Color{255, 255, 255, 255}));  // inside
  EXPECT_EQ(Pixel(image, 3, 11), (css::Color{0, 0, 0, 255}));  // bottom border
}

TEST(RasterizerTest, TextRendering) {
  Rasterizer image(64, 32);
  image.Clear(css::Color{255, 255, 255, 255});
  DisplayList list;
  list.DrawText(0, 0, "A", 16, css::Color{0, 0, 0, 255});
  image.Rasterize(list);

  // The glyph 'A' has pixels; verify some are set within its 16x16 box.
  int dark_pixels = 0;
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      const css::Color p = Pixel(image, x, y);
      if (p.r < 128) {
        ++dark_pixels;
      }
    }
  }
  EXPECT_GT(dark_pixels, 10);
}

TEST(RasterizerTest, TextMetrics) {
  EXPECT_FLOAT_EQ(Rasterizer::CharWidth(16), 16.0f);
  EXPECT_FLOAT_EQ(Rasterizer::TextWidth("hello", 16), 80.0f);
}

TEST(RasterizerTest, WritePpm) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "neko_paint_test";
  fs::create_directories(dir);
  const fs::path file = dir / "out.ppm";

  Rasterizer image(4, 3);
  image.Clear(css::Color{10, 20, 30, 255});
  const auto result = WritePpm(file.string(), image);
  ASSERT_TRUE(result.has_value());

  std::ifstream in(file, std::ios::binary);
  ASSERT_TRUE(in.is_open());
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(content.substr(0, 11), "P6\n4 3\n255\n");
  // 11-byte header + 4*3*3 bytes of RGB data.
  EXPECT_EQ(content.size(), 11u + 36u);

  fs::remove_all(dir);
}

TEST(PainterTest, DisplayListFromPage) {
  auto doc = html::Parser(
                     "<body><div style=\"background-color: #00ff00\">"
                     "<p style=\"color: #0000ff\">Hello</p></div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine layout(styles);
  std::unique_ptr<layout::LayoutBox> root = layout.BuildLayoutTree(*doc, 800);

  Painter painter(root.get());
  const DisplayList list = painter.Paint();
  ASSERT_FALSE(list.commands().empty());

  bool found_fill = false;
  bool found_text = false;
  for (const DrawCommand& command : list.commands()) {
    if (command.type == CommandType::kFillRect && command.color == css::Color{0, 255, 0, 255}) {
      found_fill = true;
    }
    if (command.type == CommandType::kDrawText && command.text == "Hello") {
      found_text = true;
      EXPECT_EQ(command.text_color, (css::Color{0, 0, 255, 255}));
    }
  }
  EXPECT_TRUE(found_fill);
  EXPECT_TRUE(found_text);
}

TEST(PainterTest, EndToEndRasterization) {
  auto doc = html::Parser(
                     "<body style=\"background-color: #ffffff\">"
                     "<div style=\"background-color: #ff0000; width: 100px; height: 40px\">"
                     "hi</div></body>")
                 .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine layout(styles);
  std::unique_ptr<layout::LayoutBox> root = layout.BuildLayoutTree(*doc, 400);

  Painter painter(root.get());
  const DisplayList list = painter.Paint();
  Rasterizer image(400, 200);
  image.Clear(css::Color{255, 255, 255, 255});
  image.Rasterize(list);

  // The red div starts at body content (8,8) and is 100x40.
  const css::Color red = Pixel(image, 50, 20);
  EXPECT_EQ(red, (css::Color{255, 0, 0, 255}));
}

}  // namespace
}  // namespace neko::paint
