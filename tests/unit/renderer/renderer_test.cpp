#include "neko/base/thread_pool.h"
#include "neko/dom/query.h"
#include "neko/html/parser.h"
#include "neko/image/image.h"
#include "neko/paint/rasterizer.h"
#include "neko/renderer/page.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <tuple>

namespace neko::renderer {
namespace {

TEST(PageTest, LoadAndDumpDom)
{
  Page page;
  const auto result =
      page.LoadHtml("<html><head><title>T</title></head><body><p>hi</p></body></html>");
  ASSERT_TRUE(result.has_value());
  ASSERT_NE(page.document(), nullptr);
  EXPECT_EQ(page.document()->Title(), "T");
  EXPECT_NE(page.DumpDom().find("<p>hi</p>"), std::string::npos);
}

TEST(PageTest, LayoutAndDump)
{
  Page page;
  ASSERT_TRUE(page.LoadHtml("<body><div>text</div></body>").has_value());
  page.Layout(800);
  ASSERT_NE(page.layout_root(), nullptr);
  const std::string dump = page.DumpLayoutTree();
  EXPECT_NE(dump.find("<body>"), std::string::npos);
  EXPECT_NE(dump.find("<div>"), std::string::npos);
}

TEST(PageTest, RasterizeProducesImage)
{
  Page page;
  ASSERT_TRUE(
      page.LoadHtml("<body style=\"background-color:#ffffff\">"
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

TEST(PageTest, BodyBackgroundPropagatesToCanvas)
{
  // CSS canvas background: a <body> background paints the whole viewport when
  // <html> has none (e.g. example.com's #f0f0f2 page background).
  Page page;
  ASSERT_TRUE(page.LoadHtml("<html><body style=\"background-color:#ff0000\"><p>x</p></body></html>")
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

TEST(PageTest, HtmlBackgroundPaintsCanvas)
{
  Page page;
  ASSERT_TRUE(page.LoadHtml("<html style=\"background-color:#0000ff\"><body><p>x</p></body></html>")
                  .has_value());
  page.Layout(400);
  paint::Rasterizer image = page.Rasterize(400, 300);
  const std::size_t offset = (static_cast<std::size_t>(290) * 400 + 390) * 4;
  EXPECT_EQ(image.pixels()[offset], 0);
  EXPECT_EQ(image.pixels()[offset + 1], 0);
  EXPECT_EQ(image.pixels()[offset + 2], 255);
}

TEST(PageTest, NoBackgroundStaysWhite)
{
  Page page;
  ASSERT_TRUE(page.LoadHtml("<html><body><p>x</p></body></html>").has_value());
  page.Layout(400);
  paint::Rasterizer image = page.Rasterize(400, 300);
  const std::size_t offset = (static_cast<std::size_t>(290) * 400 + 390) * 4;
  EXPECT_EQ(image.pixels()[offset], 255);
  EXPECT_EQ(image.pixels()[offset + 1], 255);
  EXPECT_EQ(image.pixels()[offset + 2], 255);
}

TEST(PageTest, GifAnimationAdvancesFrames)
{
  Page page;
  ASSERT_TRUE(page.LoadHtml("<body><img id=\"i\" src=\"a.gif\" width=\"1\" height=\"1\"></body>")
                  .has_value());
  page.Layout(400);
  dom::Element* img = dom::QuerySelector(*page.document(), "#i");
  ASSERT_NE(img, nullptr);

  auto animation = std::make_shared<image::GifAnimation>();
  animation->width = 1;
  animation->height = 1;
  animation->loop_count = 0; // loop forever
  image::GifFrame f0;
  f0.rgba = {255, 0, 0, 255}; // red
  f0.delay_cs = 5;            // 50 ms
  image::GifFrame f1;
  f1.rgba = {0, 255, 0, 255}; // green
  f1.delay_cs = 10;           // 100 ms
  animation->frames.push_back(std::move(f0));
  animation->frames.push_back(std::move(f1));

  image::Image first;
  first.width = 1;
  first.height = 1;
  first.rgba = animation->frames[0].rgba;

  const std::uint64_t version = page.layout_version();
  page.SetElementImage(*img, std::move(first), animation);
  EXPECT_NE(page.layout_version(), version);
  const image::Image* current = page.Find(*img);
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->rgba[0], 255); // red first frame

  // Within the first frame's duration nothing changes.
  EXPECT_FALSE(page.AdvanceAnimations());
  current = page.Find(*img);
  EXPECT_EQ(current->rgba[0], 255);

  // At t≈120 ms the 50 ms first frame has elapsed and the 100 ms second frame
  // is current: the tick advances to frame 1 (green) and bumps the page
  // version so the UI repaints.
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  const std::uint64_t before = page.layout_version();
  EXPECT_TRUE(page.AdvanceAnimations());
  EXPECT_NE(page.layout_version(), before);
  current = page.Find(*img);
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->rgba[1], 255); // green
}

TEST(PageTest, GifAnimationFiniteLoopStopsOnLastFrame)
{
  Page page;
  ASSERT_TRUE(page.LoadHtml("<body><img id=\"i\" src=\"a.gif\"></body>").has_value());
  dom::Element* img = dom::QuerySelector(*page.document(), "#i");
  ASSERT_NE(img, nullptr);

  auto animation = std::make_shared<image::GifAnimation>();
  animation->width = 1;
  animation->height = 1;
  animation->loop_count = 1; // play exactly once
  image::GifFrame f0;
  f0.rgba = {255, 0, 0, 255};
  f0.delay_cs = 5;
  image::GifFrame f1;
  f1.rgba = {0, 255, 0, 255};
  f1.delay_cs = 5;
  animation->frames.push_back(std::move(f0));
  animation->frames.push_back(std::move(f1));

  image::Image first;
  first.width = 1;
  first.height = 1;
  first.rgba = animation->frames[0].rgba;
  page.SetElementImage(*img, std::move(first), animation);

  // Both 50 ms frames have elapsed: the single allowed pass is over and the
  // animation rests on its last frame.
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_TRUE(page.AdvanceAnimations());
  const image::Image* current = page.Find(*img);
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->rgba[1], 255); // green (last frame)
  EXPECT_FALSE(page.AdvanceAnimations());
}

// A 2x2 solid-color image helper.
image::Image SolidImage(int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
  image::Image img;
  img.width = w;
  img.height = h;
  img.rgba.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 255);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::size_t o = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                             static_cast<std::size_t>(x)) *
                            4;
      img.rgba[o] = r;
      img.rgba[o + 1] = g;
      img.rgba[o + 2] = b;
    }
  }
  return img;
}

// A 3-frame 2x1 video strip whose frames are solid shades of red.
renderer::Page::VideoStrip MakeTestVideoStrip()
{
  auto frames = std::make_shared<std::vector<image::Image>>();
  for (int i = 0; i < 3; ++i) {
    frames->push_back(SolidImage(2, 1, static_cast<uint8_t>(i * 80), 0, 0));
  }
  renderer::Page::VideoStrip strip;
  strip.frames = std::move(frames);
  strip.frame_rate = 20; // 50 ms per frame
  strip.loop = true;
  return strip;
}

TEST(PageTest, VideoAutoplayAdvancesFrames)
{
  Page page;
  ASSERT_TRUE(page.LoadHtml("<body><video id=\"v\" src=\"a.mp4\"></video></body>").has_value());
  page.Layout(400);
  dom::Element* video = dom::QuerySelector(*page.document(), "#v");
  ASSERT_NE(video, nullptr);

  renderer::Page::VideoStrip strip = MakeTestVideoStrip();
  const image::Image& f0 = (*strip.frames)[0];
  page.SetElementVideo(*video, f0, std::move(strip), /*autoplay=*/true);

  const image::Image* current = page.Find(*video);
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->rgba[0], 0); // first frame

  // The first tick starts autoplay at the current frame (no change yet).
  EXPECT_FALSE(page.AdvanceAnimations());
  // After well over one frame duration the next tick advances to frame 2.
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  const std::uint64_t before = page.layout_version();
  EXPECT_TRUE(page.AdvanceAnimations());
  EXPECT_NE(page.layout_version(), before);
  current = page.Find(*video);
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->rgba[0], 160); // frame 2 (2.4 * 20 fps -> frame 2)
}

TEST(PageTest, VideoPlayPauseSeekAndDuration)
{
  Page page;
  ASSERT_TRUE(page.LoadHtml("<body><video id=\"v\" src=\"a.mp4\"></video></body>").has_value());
  page.Layout(400);
  dom::Element* video = dom::QuerySelector(*page.document(), "#v");
  ASSERT_NE(video, nullptr);

  renderer::Page::VideoStrip strip = MakeTestVideoStrip();
  const image::Image& f0 = (*strip.frames)[0];
  page.SetElementVideo(*video, f0, std::move(strip), /*autoplay=*/false);

  EXPECT_FALSE(page.IsVideoPlaying(*video));
  EXPECT_NEAR(page.VideoDuration(*video).value(), 0.15, 0.001); // 3 frames / 20 fps
  EXPECT_NEAR(page.VideoCurrentTime(*video).value(), 0.0, 0.001);

  page.PlayVideo(*video);
  EXPECT_TRUE(page.IsVideoPlaying(*video));
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  page.AdvanceAnimations();
  const image::Image* current = page.Find(*video);
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->rgba[0], 160); // frame 2 while playing

  page.PauseVideo(*video);
  EXPECT_FALSE(page.IsVideoPlaying(*video));
  const double paused_at = page.VideoCurrentTime(*video).value();
  EXPECT_GT(paused_at, 0.1);

  page.SeekVideo(*video, 0.0);
  current = page.Find(*video);
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->rgba[0], 0); // back to the first frame
  EXPECT_NEAR(page.VideoCurrentTime(*video).value(), 0.0, 0.001);

  // Resuming from frame 0 after a seek plays forward again.
  page.PlayVideo(*video);
  EXPECT_TRUE(page.IsVideoPlaying(*video));
}

TEST(PageTest, RendersElementImageAtIntrinsicSize)
{
  Page page;
  ASSERT_TRUE(page.LoadHtml("<html><body style=\"background-color:#ffffff\"><img></body></html>")
                  .has_value());
  dom::Element* img_el = dom::QuerySelector(*page.document(), "img");
  ASSERT_NE(img_el, nullptr);
  page.SetElementImage(*img_el, SolidImage(2, 2, 255, 0, 0));
  page.Layout(400);
  paint::Rasterizer image = page.Rasterize(400, 100);

  // The 2x2 image sits somewhere on the first line (platform font metrics
  // shift its exact row); its pixels are red.  Scan for the red square instead
  // of hard-coding a row.
  bool found_red = false;
  const std::size_t h = static_cast<std::size_t>(image.height());
  const std::size_t w = static_cast<std::size_t>(image.width());
  for (std::size_t y = 0; y < h && !found_red; ++y) {
    for (std::size_t x = 0; x < w; ++x) {
      const std::size_t p = (y * w + x) * 4;
      if (image.pixels()[p] == 255 && image.pixels()[p + 1] == 0 && image.pixels()[p + 2] == 0) {
        found_red = true;
        break;
      }
    }
  }
  EXPECT_TRUE(found_red);
}

TEST(PageTest, LoadHtmlClearsStaleElementImages)
{
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

TEST(PageTest, ImageWithExplicitWidthScalesAndFills)
{
  Page page;
  ASSERT_TRUE(page.LoadHtml("<html><body style=\"background-color:#ffffff\">"
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

TEST(PageTest, ConcurrentRasterizeAndImageInjection)
{
  // Regression: the GUI paints on the main thread while the worker thread
  // injects decoded images (SetElementImage + Layout).  Page must serialize
  // these; without the mutex the rasterizer reads a destroyed image entry and
  // crashes in DrawImage.
  Page page;
  ASSERT_TRUE(page.LoadHtml("<html><body style=\"background-color:#ffffff\">"
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

TEST(PageTest, ContentHeight)
{
  Page page;
  EXPECT_EQ(page.ContentHeight(), 0.0f); // no layout yet

  ASSERT_TRUE(page.LoadHtml("<body><div style=\"height:500px\">x</div></body>").has_value());
  EXPECT_EQ(page.ContentHeight(), 0.0f); // loaded but not laid out yet

  page.Layout(400);
  // body default margin (8px top + 8px bottom) plus the 500px div.
  EXPECT_FLOAT_EQ(page.ContentHeight(), 516.0f);
}

TEST(PageTest, RasterizeScrollsContent)
{
  Page page;
  ASSERT_TRUE(
      page.LoadHtml("<body style=\"background-color:#ffffff\">"
                    "<div style=\"background-color:#ff0000;width:100px;height:50px\">x</div>"
                    "</body>")
          .has_value());
  page.Layout(400);

  const auto pixel = [](const paint::Rasterizer& image, int x, int y) {
    const std::size_t offset =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width()) +
         static_cast<std::size_t>(x)) *
        4;
    return css::Color{image.pixels()[offset],
                      image.pixels()[offset + 1],
                      image.pixels()[offset + 2],
                      image.pixels()[offset + 3]};
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

TEST(PageTest, LoadMissingFile)
{
  Page page;
  const auto result = page.LoadFile("/nonexistent/neko_missing_file.html");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kIo);
}

TEST(PageTest, ElementAtHitTestsInlineContent)
{
  Page page;
  ASSERT_TRUE(page.LoadHtml("<body><p>plain <a href=\"https://example.com/x\">link</a></p></body>")
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

TEST(PageTest, ElementAtOutsideContentReturnsNull)
{
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

TEST(PageTest, LoadFile)
{
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
std::tuple<int, int, int> RgbAt(const paint::Rasterizer& image, int x, int y)
{
  const std::size_t offset = (static_cast<std::size_t>(y) * 400 + static_cast<std::size_t>(x)) * 4;
  return {image.pixels()[offset], image.pixels()[offset + 1], image.pixels()[offset + 2]};
}

bool HasColor(const paint::Rasterizer& image, int r, int g, int b)
{
  for (std::size_t i = 0; i + 2 < image.pixels().size(); i += 4) {
    if (image.pixels()[i] == r && image.pixels()[i + 1] == g && image.pixels()[i + 2] == b) {
      return true;
    }
  }
  return false;
}

TEST(PageTest, ButtonRendersNativeAppearance)
{
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

TEST(PageTest, ButtonAppearanceNoneDisablesNativeLook)
{
  // appearance:none drops the native face; the UA border (black) remains.
  Page page;
  ASSERT_TRUE(
      page.LoadHtml("<body><button style=\"appearance: none\">OK</button></body>").has_value());
  page.Layout(400);
  const paint::Rasterizer image = page.Rasterize(400, 300);
  EXPECT_FALSE(HasColor(image, 0xec, 0xec, 0xec));
  // Top-left corner of the button box: the 2px UA border is black.
  EXPECT_EQ((RgbAt(image, 8, 8)), (std::make_tuple(0, 0, 0)));
}

TEST(PageTest, ButtonAutoUsesAuthorBackground)
{
  // With a definite appearance (button, auto) author background-color still
  // wins over the native face (browser behavior for e.g. styled dropdown
  // buttons); the native buttonface is only the no-style fallback.
  Page page;
  ASSERT_TRUE(page.LoadHtml("<body><button style=\"background-color:#ff0000;"
                            "width:100px;height:30px\">B</button></body>")
                  .has_value());
  page.Layout(400);
  const paint::Rasterizer image = page.Rasterize(400, 300);
  EXPECT_EQ((RgbAt(image, 20, 15)), (std::make_tuple(255, 0, 0)));
  EXPECT_FALSE(HasColor(image, 0xec, 0xec, 0xec));
}

TEST(PageTest, ButtonNoneUsesAuthorBackground)
{
  // appearance:none -> plain CSS box: the author's background paints.
  Page page;
  ASSERT_TRUE(page.LoadHtml("<body><button style=\"appearance:none;background-color:#ff0000;"
                            "width:100px;height:30px\">B</button></body>")
                  .has_value());
  page.Layout(400);
  const paint::Rasterizer image = page.Rasterize(400, 300);
  EXPECT_EQ((RgbAt(image, 20, 15)), (std::make_tuple(255, 0, 0)));
}

TEST(PageTest, AppearanceButtonForcesNativeLookOnDiv)
{
  // appearance:button forces the button look on any element.
  Page page;
  ASSERT_TRUE(page.LoadHtml("<body><div style=\"appearance:button;width:80px;height:30px\">"
                            "D</div></body>")
                  .has_value());
  page.Layout(400);
  const paint::Rasterizer image = page.Rasterize(400, 300);
  // The button face must be painted somewhere in the box.  Assert by color
  // presence rather than an exact pixel: font metrics vary across platforms
  // and can shift the glyph over a fixed sample coordinate.
  EXPECT_TRUE(HasColor(image, 0xec, 0xec, 0xec));
}

TEST(PageTest, FloatPaintsAboveBlockChildBackground)
{
  // A float paints above in-flow block children but below inline content
  // (CSS2.1 Appendix E).  A red float followed by a block element with a
  // background must not be covered by that background where they overlap.
  Page page;
  ASSERT_TRUE(page.LoadHtml("<body><div style=\"width:300px\">"
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

// ---------------------------------------------------------------------------
// Character encoding + renderer performance
// ---------------------------------------------------------------------------

TEST(PageTest, LoadHtmlTranscodesGbkDocument)
{
  // A GBK document whose <meta charset=gb2312> declaration must trigger the
  // transcoder before the tokenizer runs.  The title text "中文" is GBK bytes
  // 0xD6D0 0xCEC4.
  const std::string gbk = "<html><head><meta charset=\"gb2312\">"
                          "<title>\xD6\xD0\xCE\xC4</title></head>"
                          "<body><p>\xD6\xD0\xCE\xC4\xD1\xA7\xCF\xB0</p></body></html>";
  Page page;
  ASSERT_TRUE(page.LoadHtml(gbk).has_value());
  EXPECT_EQ(page.document()->Title(), "\xE4\xB8\xAD\xE6\x96\x87"); // 中文
  EXPECT_NE(page.DumpDom().find("\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\xA6\xE4\xB9\xA0"),
            std::string::npos);
}

TEST(PageTest, HttpCharsetHintUsedForGbkBody)
{
  // No <meta> declaration: the HTTP charset hint must drive the decode.
  const std::string gbk = "<html><head><title>\xD6\xD0\xCE\xC4</title></head><body>x</body></html>";
  Page page;
  ASSERT_TRUE(page.LoadHtml(gbk, base::encoding::Charset::kGb18030).has_value());
  EXPECT_EQ(page.document()->Title(), "\xE4\xB8\xAD\xE6\x96\x87");
}

TEST(PageTest, Utf8BomOverridesHint)
{
  // A UTF-8 BOM is more authoritative than the HTTP hint.
  const std::string doc = "\xEF\xBB\xBF<html><head><title>ok</title></head></html>";
  Page page;
  ASSERT_TRUE(page.LoadHtml(doc, base::encoding::Charset::kGb18030).has_value());
  EXPECT_EQ(page.document()->Title(), "ok");
}

TEST(PageTest, LayoutVersionTracksContentMutations)
{
  Page page;
  ASSERT_TRUE(page.LoadHtml("<body><p>hi</p></body>").has_value());
  const std::uint64_t v0 = page.layout_version();
  page.Layout(400);
  const std::uint64_t v1 = page.layout_version();
  EXPECT_GT(v1, v0);
  page.ReapplyStyles();
  EXPECT_GT(page.layout_version(), v1);
  // Rasterizing does not change the version (display list cache is stable).
  const std::uint64_t v2 = page.layout_version();
  (void)page.Rasterize(400, 300);
  EXPECT_EQ(page.layout_version(), v2);
}

TEST(PageTest, ScrollBlitBandMatchesFullRasterization)
{
  // The UI scroll-blit path must reproduce the full rasterization at the new
  // scroll offset: shift the cached buffer, then re-rasterize only the exposed
  // band (RasterizeInto), and compare against a from-scratch rasterize.
  Page page;
  ASSERT_TRUE(
      page.LoadHtml("<body style=\"background-color:#ffffff\">"
                    "<div style=\"background-color:#ff0000;width:300px;height:60px\">a</div>"
                    "<div style=\"background-color:#00ff00;width:300px;height:80px\">b</div>"
                    "<div style=\"background-color:#0000ff;width:300px;height:60px\">c</div>"
                    "</body>")
          .has_value());
  page.Layout(400);

  constexpr int kWidth = 400;
  constexpr int kHeight = 300;
  const int from = 30;
  const int to = 110; // scroll down by 80px

  const paint::Rasterizer full_old = page.Rasterize(kWidth, kHeight, static_cast<float>(from));
  paint::Rasterizer blit = full_old; // cached buffer at scroll=from
  const int delta = from - to;       // negative: content moves up
  blit.ShiftRows(delta);
  const int band_y0 = kHeight + delta;
  const int band_y1 = kHeight;
  page.RasterizeInto(blit, band_y0, band_y1, static_cast<float>(to));

  const paint::Rasterizer full_new = page.Rasterize(kWidth, kHeight, static_cast<float>(to));
  ASSERT_EQ(blit.pixels().size(), full_new.pixels().size());
  EXPECT_EQ(blit.pixels(), full_new.pixels());
}

TEST(PageTest, ScrollUpBlitBandMatchesFullRasterization)
{
  Page page;
  ASSERT_TRUE(
      page.LoadHtml("<body style=\"background-color:#ffffff\">"
                    "<div style=\"background-color:#ff0000;width:300px;height:60px\">a</div>"
                    "<div style=\"background-color:#00ff00;width:300px;height:80px\">b</div>"
                    "<div style=\"background-color:#0000ff;width:300px;height:60px\">c</div>"
                    "</body>")
          .has_value());
  page.Layout(400);

  constexpr int kWidth = 400;
  constexpr int kHeight = 300;
  const int from = 110;
  const int to = 30; // scroll up by 80px

  const paint::Rasterizer full_old = page.Rasterize(kWidth, kHeight, static_cast<float>(from));
  paint::Rasterizer blit = full_old;
  const int delta = from - to; // positive: content moves down
  blit.ShiftRows(delta);
  page.RasterizeInto(blit, 0, delta, static_cast<float>(to));

  const paint::Rasterizer full_new = page.Rasterize(kWidth, kHeight, static_cast<float>(to));
  EXPECT_EQ(blit.pixels(), full_new.pixels());
}

TEST(PageTest, RasterizeFullReusesBuffer)
{
  Page page;
  ASSERT_TRUE(
      page.LoadHtml("<body style=\"background-color:#ffffff\">"
                    "<div style=\"background-color:#ff0000;width:100px;height:40px\">x</div>"
                    "</body>")
          .has_value());
  page.Layout(400);
  paint::Rasterizer raster(400, 300);
  page.RasterizeFull(raster, 0.0f, nullptr);
  const std::size_t offset = (static_cast<std::size_t>(20) * 400 + 50) * 4;
  EXPECT_EQ(raster.pixels()[offset], 255);
  EXPECT_EQ(raster.pixels()[offset + 1], 0);
}

TEST(PageTest, HoverStateRestylesAndKeepsLayout)
{
  Page page;
  ASSERT_TRUE(page.LoadHtml("<body><style>a:hover { color: red; }</style>"
                            "<a href=\"https://example.com/\">x</a></body>")
                  .has_value());
  page.Layout(400);
  ASSERT_NE(page.layout_root(), nullptr);
  const std::uint64_t before = page.layout_version();

  dom::Element* a = dom::QuerySelector(*page.document(), "a");
  ASSERT_NE(a, nullptr);
  page.SetHoveredElement(a);

  // Setting the hovered element re-runs the cascade and rebuilds the layout in
  // place: the version is bumped, but the layout tree stays valid.  A null
  // root here would make the UI's Refresh() treat the page as freshly loaded
  // and reset the scroll position to the top.
  EXPECT_GT(page.layout_version(), before);
  EXPECT_NE(page.layout_root(), nullptr);

  // Clearing the hover state also keeps the layout valid.
  const std::uint64_t after = page.layout_version();
  page.SetHoveredElement(nullptr);
  EXPECT_GT(page.layout_version(), after);
  EXPECT_NE(page.layout_root(), nullptr);
}

// Regression: when a script removes the hovered element from the DOM (the
// document pointer stays the same, so the UI's hover cache is not reset), the
// stale :hover pointer must not be dereferenced during the next cascade pass.
// This would be a use-after-free in IsSelfOrAncestor while matching :hover.
TEST(PageTest, HoveredElementRemovedFromDomDoesNotDangle)
{
  Page page;
  ASSERT_TRUE(page.LoadHtml("<body><style>*:hover { color: red; }</style>"
                            "<a id=\"x\" href=\"https://example.com/\">x</a></body>")
                  .has_value());
  page.Layout(400);

  dom::Element* a = dom::QuerySelector(*page.document(), "a");
  ASSERT_NE(a, nullptr);
  page.SetHoveredElement(a); // Page stores the pointer as hovered

  // A script replaces the body's content, destroying the hovered <a>.
  dom::Element* body = dom::QuerySelector(*page.document(), "body");
  ASSERT_NE(body, nullptr);
  while (body->first_child() != nullptr) {
    body->RemoveChild(body->first_child());
  }

  // The cascade must drop the dangling hovered pointer instead of matching
  // :hover against freed memory (no crash under ASan).
  EXPECT_NO_FATAL_FAILURE(page.ReapplyStyles());
  // The page stays usable: ReapplyStyles rebuilds a fresh (now empty) layout
  // tree rather than leaving the root null.
  EXPECT_NE(page.layout_root(), nullptr);
}

TEST(PageTest, ScrollBlitMatchesFullRasterize)
{
  Page page;
  std::string html = "<body style=\"background:#ffffff\">";
  for (int i = 0; i < 120; ++i) {
    html += "<p style=\"font-size:16px\">The quick brown fox jumps " + std::to_string(i) + "</p>";
  }
  html += "</body>";
  ASSERT_TRUE(page.LoadHtml(html).has_value());
  page.Layout(400);

  const int w = 400;
  const int h = 300;
  const auto count_diff = [](const paint::Rasterizer& a, const paint::Rasterizer& b) {
    int diff = 0;
    for (std::size_t o = 0; o < a.pixels().size(); o += 4) {
      if (a.pixels()[o] != b.pixels()[o] || a.pixels()[o + 1] != b.pixels()[o + 1] ||
          a.pixels()[o + 2] != b.pixels()[o + 2] || a.pixels()[o + 3] != b.pixels()[o + 3]) {
        ++diff;
      }
    }
    return diff;
  };

  // A single large scroll via blit + band re-raster must match a full render.
  for (const int scroll : {1, 40, 53, 120, 200}) {
    paint::Rasterizer ref(w, h);
    page.RasterizeFull(ref, static_cast<float>(scroll), nullptr);
    paint::Rasterizer blit(w, h);
    page.RasterizeFull(blit, 0.0f, nullptr);
    blit.ShiftRows(0 - scroll);
    page.RasterizeInto(blit, h - scroll, h, static_cast<float>(scroll));
    EXPECT_EQ(count_diff(ref, blit), 0);
  }

  // A sequence of small scrolls (accumulating blits) must match a full render.
  {
    paint::Rasterizer blit(w, h);
    page.RasterizeFull(blit, 0.0f, nullptr);
    int cached = 0;
    for (const int scroll : {40, 80, 120, 160}) {
      const int delta = cached - scroll;
      blit.ShiftRows(delta);
      if (delta > 0) {
        page.RasterizeInto(blit, 0, delta, static_cast<float>(scroll));
      } else {
        page.RasterizeInto(blit, h + delta, h, static_cast<float>(scroll));
      }
      cached = scroll;
    }
    paint::Rasterizer ref(w, h);
    page.RasterizeFull(ref, 160.0f, nullptr);
    EXPECT_EQ(count_diff(ref, blit), 0);
  }

  // Parallel banded rasterization must match serial (FreeType text included).
  {
    base::ThreadPool pool(4);
    paint::Rasterizer serial(w, h);
    page.RasterizeFull(serial, 0.0f, nullptr);
    paint::Rasterizer parallel(w, h);
    page.RasterizeFull(parallel, 0.0f, &pool);
    EXPECT_EQ(count_diff(serial, parallel), 0);
  }
}

} // namespace

} // namespace neko::renderer
