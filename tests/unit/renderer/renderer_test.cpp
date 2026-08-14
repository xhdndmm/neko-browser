#include "neko/renderer/page.h"
#include "neko/paint/rasterizer.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <tuple>

#include "neko/dom/query.h"
#include "neko/html/parser.h"
#include "neko/image/image.h"
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

TEST(PageTest, BodyBackgroundPropagatesToCanvas) {
  // CSS canvas background: a <body> background paints the whole viewport when
  // <html> has none (e.g. example.com's #f0f0f2 page background).
  Page page;
  ASSERT_TRUE(page.LoadHtml(
                          "<html><body style=\"background-color:#ff0000\"><p>x</p></body></html>")
                  .has_value());
  page.Layout(400);
  paint::Rasterizer image = page.Rasterize(400, 300);

  // The bottom-right corner is outside the short body box; it must still be
  // red via background propagation (was white before the fix).
  const std::size_t offset = (static_cast<std::size_t>(290) * 400 + 390) * 4;
  EXPECT_EQ(image.pixels()[offset], 255);
  EXPECT_EQ(image.pixels()[offset + 1], 0);
  EXPECT_EQ(image.pixels()[offset + 2], 0);
}

TEST(PageTest, HtmlBackgroundPaintsCanvas) {
  Page page;
  ASSERT_TRUE(page.LoadHtml(
                          "<html style=\"background-color:#0000ff\"><body><p>x</p></body></html>")
                  .has_value());
  page.Layout(400);
  paint::Rasterizer image = page.Rasterize(400, 300);
  const std::size_t offset = (static_cast<std::size_t>(290) * 400 + 390) * 4;
  EXPECT_EQ(image.pixels()[offset], 0);
  EXPECT_EQ(image.pixels()[offset + 1], 0);
  EXPECT_EQ(image.pixels()[offset + 2], 255);
}

TEST(PageTest, NoBackgroundStaysWhite) {
  Page page;
  ASSERT_TRUE(page.LoadHtml("<html><body><p>x</p></body></html>").has_value());
  page.Layout(400);
  paint::Rasterizer image = page.Rasterize(400, 300);
  const std::size_t offset = (static_cast<std::size_t>(290) * 400 + 390) * 4;
  EXPECT_EQ(image.pixels()[offset], 255);
  EXPECT_EQ(image.pixels()[offset + 1], 255);
  EXPECT_EQ(image.pixels()[offset + 2], 255);
}

// A 2x2 solid-color image helper.
image::Image SolidImage(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
  image::Image img;
  img.width = w;
  img.height = h;
  img.rgba.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 255);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::size_t o =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
           static_cast<std::size_t>(x)) *
          4;
      img.rgba[o] = r;
      img.rgba[o + 1] = g;
      img.rgba[o + 2] = b;
    }
  }
  return img;
}

TEST(PageTest, RendersElementImageAtIntrinsicSize) {
  Page page;
  ASSERT_TRUE(page.LoadHtml(
                          "<html><body style=\"background-color:#ffffff\"><img></body></html>")
                  .has_value());
  dom::Element* img_el = dom::QuerySelector(*page.document(), "img");
  ASSERT_NE(img_el, nullptr);
  page.SetElementImage(*img_el, SolidImage(2, 2, 255, 0, 0));
  page.Layout(400);
  paint::Rasterizer image = page.Rasterize(400, 100);

  // The 2x2 image is baseline-aligned in its line box (CSS2.2 10.8: the line
  // has an imaginary strut whose ascent places the baseline ~14px down); the
  // image's bottom sits on that baseline (rows ~22-23).  Its pixels are red.
  const std::size_t o = (static_cast<std::size_t>(22) * 400 + 9) * 4;
  EXPECT_EQ(image.pixels()[o], 255);
  EXPECT_EQ(image.pixels()[o + 1], 0);
  EXPECT_EQ(image.pixels()[o + 2], 0);
}

TEST(PageTest, LoadHtmlClearsStaleElementImages) {
  // Image entries are keyed by element address.  After a navigation the old
  // DOM (and its element addresses) is gone; a stale entry could be reused by
  // a new element at the same address and render the previous page's image.
  Page page;
  ASSERT_TRUE(page.LoadHtml("<html><body><img></body></html>").has_value());
  dom::Element* img_el = dom::QuerySelector(*page.document(), "img");
  ASSERT_NE(img_el, nullptr);
  page.SetElementImage(*img_el, SolidImage(2, 2, 255, 0, 0));

  // Navigate: the previous document (and img element) is replaced.
  ASSERT_TRUE(page.LoadHtml("<html><body><p>x</p></body></html>").has_value());
  page.Layout(400);
  paint::Rasterizer image = page.Rasterize(400, 100);
  // No red image pixels may survive the navigation (the new page has no img).
  bool saw_red = false;
  const auto& pix = image.pixels();
  for (std::size_t i = 0; i + 2 < pix.size(); i += 4) {
    if (pix[i] == 255 && pix[i + 1] == 0 && pix[i + 2] == 0) {
      saw_red = true;
      break;
    }
  }
  EXPECT_FALSE(saw_red);
}

TEST(PageTest, ImageWithExplicitWidthScalesAndFills) {
  Page page;
  ASSERT_TRUE(page.LoadHtml(
                          "<html><body style=\"background-color:#ffffff\">"
                          "<img style=\"width: 100px\"></body></html>")
                  .has_value());
  dom::Element* img_el = dom::QuerySelector(*page.document(), "img");
  ASSERT_NE(img_el, nullptr);
  // 2x4 image: explicit width 100px -> height 200px (aspect ratio kept).
  page.SetElementImage(*img_el, SolidImage(2, 4, 0, 0, 255));
  page.Layout(400);
  paint::Rasterizer image = page.Rasterize(400, 300);

  // Content box spans (8,8)-(108,208); a pixel near its center is blue.
  const std::size_t o = (static_cast<std::size_t>(100) * 400 + 60) * 4;
  EXPECT_EQ(image.pixels()[o], 0);
  EXPECT_EQ(image.pixels()[o + 1], 0);
  EXPECT_EQ(image.pixels()[o + 2], 255);
}

TEST(PageTest, ConcurrentRasterizeAndImageInjection) {
  // Regression: the GUI paints on the main thread while the worker thread
  // injects decoded images (SetElementImage + Layout).  Page must serialize
  // these; without the mutex the rasterizer reads a destroyed image entry and
  // crashes in DrawImage.
  Page page;
  ASSERT_TRUE(page.LoadHtml(
                          "<html><body style=\"background-color:#ffffff\">"
                          "<img><img><img><p>text</p></body></html>")
                  .has_value());
  std::vector<dom::Element*> imgs;
  std::vector<dom::Node*> stack;
  for (dom::Node* child : page.document()->ChildNodes()) {
    stack.push_back(child);
  }
  while (!stack.empty()) {
    dom::Node* node = stack.back();
    stack.pop_back();
    if (node->node_type() == dom::NodeType::kElement) {
      auto* el = static_cast<dom::Element*>(node);
      if (el->tag_name() == "img") {
        imgs.push_back(el);
      }
      for (dom::Node* child : node->ChildNodes()) {
        stack.push_back(child);
      }
    }
  }
  ASSERT_GE(imgs.size(), 2u);
  page.Layout(400);

  std::atomic<bool> stop{false};
  std::thread writer([&] {
    int i = 0;
    while (!stop) {
      page.SetElementImage(*imgs[static_cast<std::size_t>(i) % imgs.size()],
                           SolidImage(4, 4, 200, 100, 50));
      page.Layout(400);
      ++i;
    }
  });

  for (int k = 0; k < 200; ++k) {
    paint::Rasterizer raster = page.Rasterize(400, 200);
    EXPECT_EQ(raster.width(), 400);
    page.ContentHeight();
    page.ElementAt(10.0f, 10.0f);
  }

  stop = true;
  writer.join();
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

// --- button / appearance rendering ---------------------------------------

// Reads the RGB triple at (x, y) of a 400x300 raster.
std::tuple<int, int, int> RgbAt(const paint::Rasterizer& image, int x, int y) {
  const std::size_t offset =
      (static_cast<std::size_t>(y) * 400 + static_cast<std::size_t>(x)) * 4;
  return {image.pixels()[offset], image.pixels()[offset + 1], image.pixels()[offset + 2]};
}

bool HasColor(const paint::Rasterizer& image, int r, int g, int b) {
  for (std::size_t i = 0; i + 2 < image.pixels().size(); i += 4) {
    if (image.pixels()[i] == r && image.pixels()[i + 1] == g && image.pixels()[i + 2] == b) {
      return true;
    }
  }
  return false;
}

TEST(PageTest, ButtonRendersNativeAppearance) {
  // A <button> defaults to appearance:auto (WHATWG rendering §15.5.4) and
  // paints a native buttonface with an outset border.
  Page page;
  ASSERT_TRUE(page.LoadHtml("<body><button>OK</button></body>").has_value());
  page.Layout(400);
  const paint::Rasterizer image = page.Rasterize(400, 300);
  // Button box starts at (8,8) (body margin); (12,12) is inside the 2px
  // border, in the left padding — buttonface, no glyph there.
  EXPECT_EQ((RgbAt(image, 12, 12)), (std::make_tuple(0xec, 0xec, 0xec)));
  EXPECT_TRUE(HasColor(image, 0xec, 0xec, 0xec));
}

TEST(PageTest, ButtonAppearanceNoneDisablesNativeLook) {
  // appearance:none drops the native face; the UA border (black) remains.
  Page page;
  ASSERT_TRUE(
      page.LoadHtml("<body><button style=\"appearance: none\">OK</button></body>")
          .has_value());
  page.Layout(400);
  const paint::Rasterizer image = page.Rasterize(400, 300);
  EXPECT_FALSE(HasColor(image, 0xec, 0xec, 0xec));
  // Top-left corner of the button box: the 2px UA border is black.
  EXPECT_EQ((RgbAt(image, 8, 8)), (std::make_tuple(0, 0, 0)));
}

TEST(PageTest, ButtonAutoUsesAuthorBackground) {
  // With a definite appearance (button, auto) author background-color still
  // wins over the native face (browser behavior for e.g. styled dropdown
  // buttons); the native buttonface is only the no-style fallback.
  Page page;
  ASSERT_TRUE(page.LoadHtml(
                          "<body><button style=\"background-color:#ff0000;"
                          "width:100px;height:30px\">B</button></body>")
                  .has_value());
  page.Layout(400);
  const paint::Rasterizer image = page.Rasterize(400, 300);
  EXPECT_EQ((RgbAt(image, 20, 15)), (std::make_tuple(255, 0, 0)));
  EXPECT_FALSE(HasColor(image, 0xec, 0xec, 0xec));
}

TEST(PageTest, ButtonNoneUsesAuthorBackground) {
  // appearance:none -> plain CSS box: the author's background paints.
  Page page;
  ASSERT_TRUE(page.LoadHtml(
                          "<body><button style=\"appearance:none;background-color:#ff0000;"
                          "width:100px;height:30px\">B</button></body>")
                  .has_value());
  page.Layout(400);
  const paint::Rasterizer image = page.Rasterize(400, 300);
  EXPECT_EQ((RgbAt(image, 20, 15)), (std::make_tuple(255, 0, 0)));
}

TEST(PageTest, AppearanceButtonForcesNativeLookOnDiv) {
  // appearance:button forces the button look on any element.
  Page page;
  ASSERT_TRUE(page.LoadHtml(
                          "<body><div style=\"appearance:button;width:80px;height:30px\">"
                          "D</div></body>")
                  .has_value());
  page.Layout(400);
  const paint::Rasterizer image = page.Rasterize(400, 300);
  // The button face must be painted somewhere in the box.  Assert by color
  // presence rather than an exact pixel: font metrics vary across platforms
  // and can shift the glyph over a fixed sample coordinate.
  EXPECT_TRUE(HasColor(image, 0xec, 0xec, 0xec));
}

TEST(PageTest, FloatPaintsAboveBlockChildBackground) {
  // A float paints above in-flow block children but below inline content
  // (CSS2.1 Appendix E).  A red float followed by a block element with a
  // background must not be covered by that background where they overlap.
  Page page;
  ASSERT_TRUE(page.LoadHtml(
                          "<body><div style=\"width:300px\">"
                          "<span style=\"float:left;background:red;width:100px;height:50px\">"
                          "L</span>"
                          "<p style=\"background:gray\">after</p>"
                          "</div></body>")
                  .has_value());
  page.Layout(400);
  const paint::Rasterizer image = page.Rasterize(400, 300);
  // The following paragraph's gray background box (y~24..43, full width)
  // overlaps the red float (y 8..58, x<100).  In the overlap the float must
  // win: the pixel must stay red, not gray.
  EXPECT_EQ((RgbAt(image, 20, 30)), (std::make_tuple(255, 0, 0)));
  EXPECT_EQ((RgbAt(image, 20, 40)), (std::make_tuple(255, 0, 0)));
}

}  // namespace

}  // namespace neko::renderer

