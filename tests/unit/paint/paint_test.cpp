#include "neko/paint/painter.h"
#include "neko/paint/rasterizer.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include "neko/dom/query.h"
#include "neko/graphics/font_registry.h"
#include "neko/graphics/system_fonts.h"
#include "neko/image/image.h"
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

TEST(RasterizerTest, GlyphsAreNotMirrored) {
  Rasterizer image(16, 16);
  image.Clear(css::Color{255, 255, 255, 255});
  DisplayList list;
  list.DrawText(0, 0, "/", 8, css::Color{0, 0, 0, 255});
  image.Rasterize(list);

  const auto dark = [&](int x, int y) { return Pixel(image, x, y).r < 128; };

  // '/' slopes from top-right down to bottom-left. A horizontally mirrored
  // glyph would instead put the top on the left and the bottom on the right.
  EXPECT_TRUE(dark(6, 0));   // top of the stroke sits on the right
  EXPECT_TRUE(dark(0, 6));   // bottom of the stroke sits on the left
  EXPECT_FALSE(dark(0, 0));  // top-left corner stays clear
  EXPECT_FALSE(dark(6, 6));  // bottom-right corner stays clear
}

TEST(RasterizerTest, TextMetrics) {
  EXPECT_FLOAT_EQ(Rasterizer::CharWidth(16), 16.0f);
  EXPECT_FLOAT_EQ(Rasterizer::TextWidth("hello", 16), 80.0f);
}

TEST(RasterizerTest, DrawImageFractionalOriginAndUpscale) {
  // Regression: a fractional destination origin plus an upscaled image used to
  // sample rgba at index -1 (floor of dst_y was below dst_y, so the first row
  // mapped to a negative source row) and crash in DrawImage.
  image::Image img;
  img.width = 10;
  img.height = 40;  // taller than the 10px box -> upscale (ih > dst_h)
  img.rgba.assign(10u * 40u * 4u, 255);
  for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
    img.rgba[i] = 255;      // R
    img.rgba[i + 1] = 0;    // G
    img.rgba[i + 2] = 0;    // B
    img.rgba[i + 3] = 255;  // A
  }

  Rasterizer raster(64, 64);
  raster.Clear(css::Color{255, 255, 255, 255});
  DisplayList list;
  list.DrawImage(0.0f, 10.4f, 20.0f, 10.0f, img, style::ObjectFit::kFill);
  raster.Rasterize(list);

  // A pixel inside the drawn box is red (the command ran without crashing).
  EXPECT_EQ(Pixel(raster, 10, 15).r, 255);
  EXPECT_EQ(Pixel(raster, 10, 15).g, 0);
}

TEST(RasterizerTest, FreeTypeTextRendering) {
  if (graphics::FindSystemFonts(graphics::GenericFamily::kSansSerif).empty()) {
    GTEST_SKIP() << "no system sans-serif font available";
  }
  graphics::FontRegistry registry;

  Rasterizer image(64, 32);
  image.Clear(css::Color{255, 255, 255, 255});
  image.SetFontRegistry(&registry);
  DisplayList list;
  list.DrawText(0, 0, "A", 16, css::Color{0, 0, 0, 255});
  image.Rasterize(list);

  // FreeType output is antialiased: some pixels must be strictly between the
  // background (255) and the text color (0), which the 8x8 fallback never
  // produces.  Also require actual dark pixels.
  bool saw_gray = false;
  int dark_pixels = 0;
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 64; ++x) {
      const uint8_t r = Pixel(image, x, y).r;
      if (r > 0 && r < 255) {
        saw_gray = true;
      }
      if (r < 128) {
        ++dark_pixels;
      }
    }
  }
  EXPECT_TRUE(saw_gray);
  EXPECT_GT(dark_pixels, 5);
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
  in.close();
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

TEST(PainterTest, InlineBlockPaintsInnerBlockBackground) {
  // An inline-block's inner block layout (background) must be painted even
  // though it lives inside a line box, not a block child.
  auto doc = html::Parser(
      "<body><div><span style=\"display:inline-block;background-color:#ff0000;"
      "width:100px;height:40px\">x</span></div></body>")
      .Parse();
  style::StyleEngine styles;
  styles.ApplyStyles(*doc);
  layout::LayoutEngine layout(styles);
  std::unique_ptr<layout::LayoutBox> root = layout.BuildLayoutTree(*doc, 400);

  Painter painter(root.get());
  const DisplayList list = painter.Paint();
  ASSERT_FALSE(list.commands().empty());

  bool found = false;
  for (const DrawCommand& c : list.commands()) {
    if (c.type == CommandType::kFillRect && c.color == css::Color{255, 0, 0, 255}) {
      EXPECT_FLOAT_EQ(c.width, 100.0f);
      EXPECT_FLOAT_EQ(c.height, 40.0f);
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

}  // namespace
}  // namespace neko::paint
