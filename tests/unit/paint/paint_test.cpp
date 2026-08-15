#include "neko/base/thread_pool.h"
#include "neko/dom/query.h"
#include "neko/graphics/font_registry.h"
#include "neko/graphics/system_fonts.h"
#include "neko/html/parser.h"
#include "neko/image/image.h"
#include "neko/layout/layout_tree.h"
#include "neko/paint/painter.h"
#include "neko/paint/rasterizer.h"
#include "neko/style/style_engine.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>

namespace neko::paint {
namespace {

css::Color Pixel(const Rasterizer& image, int x, int y)
{
  const std::size_t offset =
      (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width()) +
       static_cast<std::size_t>(x)) *
      4;
  return css::Color{image.pixels()[offset],
                    image.pixels()[offset + 1],
                    image.pixels()[offset + 2],
                    image.pixels()[offset + 3]};
}

TEST(RasterizerTest, FillRect)
{
  Rasterizer image(20, 20);
  image.Clear(css::Color{255, 255, 255, 255});
  DisplayList list;
  list.FillRect(2, 2, 10, 10, css::Color{255, 0, 0, 255});
  image.Rasterize(list);

  EXPECT_EQ(Pixel(image, 5, 5), (css::Color{255, 0, 0, 255}));
  EXPECT_EQ(Pixel(image, 0, 0), (css::Color{255, 255, 255, 255}));
  EXPECT_EQ(Pixel(image, 15, 15), (css::Color{255, 255, 255, 255}));
}

TEST(RasterizerTest, SetScrollOffset)
{
  Rasterizer image(20, 20);
  image.Clear(css::Color{255, 255, 255, 255});
  DisplayList list;
  list.FillRect(2, 2, 10, 10, css::Color{255, 0, 0, 255});
  image.SetScrollOffset(5.0f);
  image.Rasterize(list);

  // The rect (y=2..12) shifts up by 5 to y=-3..7, i.e. visible at y=0..6.
  EXPECT_EQ(Pixel(image, 5, 1), (css::Color{255, 0, 0, 255}));     // newly revealed top
  EXPECT_EQ(Pixel(image, 5, 9), (css::Color{255, 255, 255, 255})); // scrolled-out bottom
}

TEST(RasterizerTest, SetScrollOffsetClipsScrolledOutContent)
{
  Rasterizer image(20, 20);
  image.Clear(css::Color{255, 255, 255, 255});
  DisplayList list;
  list.FillRect(0, 0, 20, 20, css::Color{255, 0, 0, 255});
  image.SetScrollOffset(40.0f); // fully scrolled out
  image.Rasterize(list);
  EXPECT_EQ(Pixel(image, 10, 10), (css::Color{255, 255, 255, 255}));
}

TEST(RasterizerTest, PushPopClipRestrictsFills)
{
  Rasterizer image(20, 20);
  image.Clear(css::Color{255, 255, 255, 255});
  DisplayList list;
  // A red rect that overflows the clip (10..14) on all sides.
  list.PushClip(4, 4, 12, 12);
  list.FillRect(0, 0, 20, 20, css::Color{255, 0, 0, 255});
  list.PopClip();
  image.Rasterize(list);

  EXPECT_EQ(Pixel(image, 10, 10), (css::Color{255, 0, 0, 255}));     // inside clip
  EXPECT_EQ(Pixel(image, 2, 2), (css::Color{255, 255, 255, 255}));   // outside clip
  EXPECT_EQ(Pixel(image, 17, 17), (css::Color{255, 255, 255, 255})); // outside clip
}

TEST(RasterizerTest, NestedClipsIntersect)
{
  Rasterizer image(20, 20);
  image.Clear(css::Color{255, 255, 255, 255});
  DisplayList list;
  list.PushClip(2, 2, 16, 16);
  list.PushClip(6, 6, 8, 8);
  list.FillRect(0, 0, 20, 20, css::Color{255, 0, 0, 255});
  list.PopClip();
  list.PopClip();
  image.Rasterize(list);

  EXPECT_EQ(Pixel(image, 8, 8), (css::Color{255, 0, 0, 255}));       // inside both clips
  EXPECT_EQ(Pixel(image, 4, 4), (css::Color{255, 255, 255, 255}));   // only outer clip
  EXPECT_EQ(Pixel(image, 14, 14), (css::Color{255, 255, 255, 255})); // only outer clip
}

TEST(RasterizerTest, AlphaBlend)
{
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

TEST(RasterizerTest, BorderRect)
{
  Rasterizer image(20, 20);
  image.Clear(css::Color{255, 255, 255, 255});
  DisplayList list;
  list.BorderRect(2, 2, 10, 10, 2, 2, 2, 2, css::Color{0, 0, 0, 255});
  image.Rasterize(list);

  EXPECT_EQ(Pixel(image, 3, 3), (css::Color{0, 0, 0, 255}));       // top border
  EXPECT_EQ(Pixel(image, 5, 5), (css::Color{255, 255, 255, 255})); // inside
  EXPECT_EQ(Pixel(image, 3, 11), (css::Color{0, 0, 0, 255}));      // bottom border
}

TEST(RasterizerTest, TextRendering)
{
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

TEST(RasterizerTest, GlyphsAreNotMirrored)
{
  Rasterizer image(16, 16);
  image.Clear(css::Color{255, 255, 255, 255});
  DisplayList list;
  list.DrawText(0, 0, "/", 8, css::Color{0, 0, 0, 255});
  image.Rasterize(list);

  const auto dark = [&](int x, int y) { return Pixel(image, x, y).r < 128; };

  // '/' slopes from top-right down to bottom-left. A horizontally mirrored
  // glyph would instead put the top on the left and the bottom on the right.
  EXPECT_TRUE(dark(6, 0));  // top of the stroke sits on the right
  EXPECT_TRUE(dark(0, 6));  // bottom of the stroke sits on the left
  EXPECT_FALSE(dark(0, 0)); // top-left corner stays clear
  EXPECT_FALSE(dark(6, 6)); // bottom-right corner stays clear
}

TEST(RasterizerTest, TextMetrics)
{
  EXPECT_FLOAT_EQ(Rasterizer::CharWidth(16), 16.0f);
  EXPECT_FLOAT_EQ(Rasterizer::TextWidth("hello", 16), 80.0f);
}

TEST(RasterizerTest, DrawImageFractionalOriginAndUpscale)
{
  // Regression: a fractional destination origin plus an upscaled image used to
  // sample rgba at index -1 (floor of dst_y was below dst_y, so the first row
  // mapped to a negative source row) and crash in DrawImage.
  image::Image img;
  img.width = 10;
  img.height = 40; // taller than the 10px box -> upscale (ih > dst_h)
  img.rgba.assign(10u * 40u * 4u, 255);
  for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
    img.rgba[i] = 255;     // R
    img.rgba[i + 1] = 0;   // G
    img.rgba[i + 2] = 0;   // B
    img.rgba[i + 3] = 255; // A
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

TEST(RasterizerTest, FreeTypeTextRendering)
{
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

TEST(RasterizerTest, WritePpm)
{
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

TEST(PainterTest, DisplayListFromPage)
{
  auto doc = html::Parser("<body><div style=\"background-color: #00ff00\">"
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

TEST(PainterTest, EndToEndRasterization)
{
  auto doc = html::Parser("<body style=\"background-color: #ffffff\">"
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

TEST(PainterTest, InlineBlockPaintsInnerBlockBackground)
{
  // An inline-block's inner block layout (background) must be painted even
  // though it lives inside a line box, not a block child.
  auto doc = html::Parser("<body><div><span style=\"display:inline-block;background-color:#ff0000;"
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

// ---------------------------------------------------------------------------
// Renderer performance: buffer reuse, scroll blit, bands, parallel raster.
// ---------------------------------------------------------------------------

TEST(RasterizerTest, ResizeReusesBufferStorage)
{
  Rasterizer image(100, 100);
  image.Clear(css::Color{1, 2, 3, 255});
  const uint8_t* before = image.pixels().data();
  // Shrink then grow within the previous capacity: the storage (and its data
  // pointer) must be reused, so per-frame paints do not reallocate.
  image.Resize(80, 60);
  EXPECT_EQ(image.pixels().data(), before);
  image.Resize(100, 100);
  EXPECT_EQ(image.pixels().data(), before);
  // Content that never left the buffer (inside the shrunk 80x60 region) is
  // preserved across the resize cycle.
  EXPECT_EQ(Pixel(image, 40, 40), (css::Color{1, 2, 3, 255}));
}

TEST(RasterizerTest, ShiftRowsScrollsBufferContent)
{
  Rasterizer image(20, 20);
  image.Clear(css::Color{255, 255, 255, 255});
  DisplayList list;
  // A red block in the top half, blue in the bottom half.
  list.FillRect(0, 0, 20, 8, css::Color{255, 0, 0, 255});
  list.FillRect(0, 12, 20, 8, css::Color{0, 0, 255, 255});
  image.Rasterize(list);

  image.ShiftRows(5); // content moves down by 5 rows
  // The exposed top band is stale by design; the caller re-rasterizes it.
  image.ClearBand(0, 5, css::Color{255, 255, 255, 255});
  EXPECT_EQ(Pixel(image, 10, 3), (css::Color{255, 255, 255, 255})); // top cleared
  EXPECT_EQ(Pixel(image, 10, 6), (css::Color{255, 0, 0, 255}));     // red moved down
  EXPECT_EQ(Pixel(image, 10, 17), (css::Color{0, 0, 255, 255}));    // blue moved down

  Rasterizer image2(20, 20);
  image2.Clear(css::Color{255, 255, 255, 255});
  DisplayList list2;
  list2.FillRect(0, 0, 20, 8, css::Color{255, 0, 0, 255});
  image2.Rasterize(list2);
  image2.ShiftRows(-3); // content moves up by 3 rows
  image2.ClearBand(17, 20, css::Color{255, 255, 255, 255});
  EXPECT_EQ(Pixel(image2, 10, 1), (css::Color{255, 0, 0, 255})); // red moved up
  EXPECT_EQ(Pixel(image2, 10, 10), (css::Color{255, 255, 255, 255}));
}

TEST(RasterizerTest, VisibleBandRestrictsDrawing)
{
  Rasterizer image(20, 20);
  image.Clear(css::Color{255, 255, 255, 255});
  DisplayList list;
  list.FillRect(0, 0, 20, 20, css::Color{255, 0, 0, 255});
  image.SetVisibleBand(5, 10);
  image.Rasterize(list);
  image.ResetVisibleBand();

  EXPECT_EQ(Pixel(image, 10, 5), (css::Color{255, 0, 0, 255}));
  EXPECT_EQ(Pixel(image, 10, 9), (css::Color{255, 0, 0, 255}));
  EXPECT_EQ(Pixel(image, 10, 4), (css::Color{255, 255, 255, 255}));  // outside band
  EXPECT_EQ(Pixel(image, 10, 10), (css::Color{255, 255, 255, 255})); // outside band
}

TEST(RasterizerTest, ClearBandOnlyTouchesBand)
{
  Rasterizer image(20, 20);
  image.Clear(css::Color{255, 0, 0, 255});
  image.ClearBand(5, 10, css::Color{255, 255, 255, 255});
  EXPECT_EQ(Pixel(image, 10, 7), (css::Color{255, 255, 255, 255}));
  EXPECT_EQ(Pixel(image, 10, 3), (css::Color{255, 0, 0, 255}));
  EXPECT_EQ(Pixel(image, 10, 12), (css::Color{255, 0, 0, 255}));
}

TEST(RasterizerTest, ParallelRasterizationMatchesSerial)
{
  // A display list exercising rects, clips, the embedded-font text fallback
  // and an image; the parallel banded path must produce byte-identical output.
  DisplayList list;
  list.FillRect(0, 0, 64, 64, css::Color{240, 240, 240, 255});
  list.PushClip(4, 4, 56, 56);
  list.FillRect(0, 0, 64, 64, css::Color{255, 0, 0, 255});
  list.PopClip();
  list.FillRoundRect(8, 8, 20, 20, 4.0f, css::Color{0, 255, 0, 255});
  list.DrawText(4, 10, "Hello", 16, css::Color{0, 0, 255, 255});
  image::Image img;
  img.width = 8;
  img.height = 8;
  img.rgba.assign(8u * 8u * 4u, 255);
  for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
    img.rgba[i] = 200;
    img.rgba[i + 3] = 255;
  }
  list.DrawImage(30, 30, 24, 24, img, style::ObjectFit::kFill);

  base::ThreadPool pool(4);
  Rasterizer serial(64, 64);
  serial.Clear(css::Color{255, 255, 255, 255});
  serial.Rasterize(list);

  Rasterizer parallel(64, 64);
  parallel.Clear(css::Color{255, 255, 255, 255});
  parallel.RasterizeParallel(list, pool, /*min_band_height=*/4);

  ASSERT_EQ(serial.pixels().size(), parallel.pixels().size());
  EXPECT_EQ(serial.pixels(), parallel.pixels());
}

TEST(RasterizerTest, IntegerBlendMatchesReferenceAlpha)
{
  // A semi-transparent red over an opaque blue: the integer fixed-point blend
  // must equal the reference float computation up to one LSB.
  const css::Color src{255, 0, 0, 128};
  const css::Color dst{0, 0, 255, 255};
  const float sa = static_cast<float>(src.a) / 255.0f;
  const float da = static_cast<float>(dst.a) / 255.0f;
  const float out_a = sa + da * (1.0f - sa);
  const auto ref = [&](uint8_t s, uint8_t d) -> uint8_t {
    return static_cast<uint8_t>(
        (static_cast<float>(s) * sa + static_cast<float>(d) * da * (1.0f - sa)) / out_a + 0.5f);
  };

  Rasterizer image(1, 1);
  image.Clear(dst);
  DisplayList list;
  list.FillRect(0, 0, 1, 1, src);
  image.Rasterize(list);

  const css::Color out = Pixel(image, 0, 0);
  const uint8_t ref_r = ref(src.r, dst.r);
  const uint8_t ref_b = ref(src.b, dst.b);
  // Integer math rounds slightly differently; allow one LSB of difference.
  EXPECT_LE(std::abs(static_cast<int>(out.r) - static_cast<int>(ref_r)), 1);
  EXPECT_LE(std::abs(static_cast<int>(out.b) - static_cast<int>(ref_b)), 1);
  EXPECT_LE(std::abs(static_cast<int>(out.a) -
                     static_cast<int>(static_cast<uint8_t>(out_a * 255.0f + 0.5f))),
            1);
}

} // namespace
} // namespace neko::paint
