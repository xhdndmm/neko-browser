// Renderer session tests (ADR 0016 M2): a live child process serving the
// session protocol — load, on-demand frames, input forwarding, script pumping,
// redirect reporting, and crash isolation.  Each test spawns the real browser
// binary in --renderer-session mode.

#include "neko/browser/browser_controller.h"
#include "neko/browser/renderer_session.h"
#include "neko/dom/element.h"
#include "neko/dom/query.h"
#include "neko/renderer/page.h"

#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <thread>

#ifndef _WIN32
#include <csignal>
#include <unistd.h>

// Writing to a pipe whose peer is gone (the crash test) must fail with EPIPE,
// not SIGPIPE.
const int kSigpipeIgnored = [] {
  std::signal(SIGPIPE, SIG_IGN);
  return 0;
}();
#endif

namespace neko::browser {
namespace {

std::string ReadFixture(const std::string& name)
{
  const std::string path = std::string(NEKO_TEST_PAGES_DIR) + "/" + name;
  std::ifstream input(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

constexpr int kViewportWidth = 320;
constexpr int kViewportHeight = 240;
constexpr char kFixtureName[] = "session_test.html";
constexpr char kLinkUrl[] = "https://127.0.0.1:9/next";

struct Point
{
  float x = 0;
  float y = 0;
};

// Runs the fixture through the in-process engine and returns the click point
// (element centre) for the CSS selector |selector|.  The renderer child runs
// the identical pipeline, so the tests do not encode layout assumptions.
Point ElementCenter(const std::string& html, const std::string& selector)
{
  renderer::Page page;
  EXPECT_TRUE(page.LoadHtml(html).has_value());
  page.Layout(static_cast<float>(kViewportWidth), static_cast<float>(kViewportHeight));
  dom::Element* element = dom::QuerySelector(*page.document(), selector);
  EXPECT_NE(element, nullptr) << selector;
  if (element == nullptr) {
    return {};
  }
  const auto geometry = page.ElementBoxGeometry(*element);
  EXPECT_TRUE(geometry.has_value()) << "no layout box for " << selector;
  if (!geometry.has_value()) {
    return {};
  }
  return {geometry->x + geometry->width / 2.0f, geometry->y + geometry->height / 2.0f};
}

std::shared_ptr<RendererSession> SpawnSession()
{
  auto session = RendererSession::Spawn(NEKO_BROWSER_BIN, "test-site");
  EXPECT_TRUE(session.has_value()) << (session.has_value() ? "" : session.error().message());
  return session.has_value() ? session.value() : nullptr;
}

base::Result<RendererUpdate> LoadFixture(RendererSession& session)
{
  return session.Load(ReadFixture(kFixtureName),
                      "text/html; charset=utf-8",
                      std::string("https://test.invalid/") + kFixtureName,
                      kViewportWidth,
                      kViewportHeight);
}

bool FramesDiffer(const RemoteFrame& a, const RemoteFrame& b)
{
  if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size()) {
    return true;
  }
  return a.rgba != b.rgba;
}

bool HasNonWhitePixel(const RemoteFrame& frame)
{
  for (std::size_t i = 0; i + 3 < frame.rgba.size(); i += 4) {
    if (frame.rgba[i] != 255 || frame.rgba[i + 1] != 255 || frame.rgba[i + 2] != 255) {
      return true;
    }
  }
  return false;
}

// Samples one pixel of a frame (RGBA8888, viewport coordinates).
bool IsRed(const RemoteFrame& frame, const Point& point)
{
  const int x = static_cast<int>(point.x);
  const int y = static_cast<int>(point.y);
  if (x < 0 || y < 0 || x >= frame.width || y >= frame.height) {
    return false;
  }
  const std::size_t index = (static_cast<std::size_t>(y) * frame.width + x) * 4;
  if (index + 3 >= frame.rgba.size()) {
    return false;
  }
  return frame.rgba[index] > 200 && frame.rgba[index + 1] < 60 && frame.rgba[index + 2] < 60;
}

TEST(RendererSessionTest, SpawnRejectsEmptyExecutable)
{
  const auto session = RendererSession::Spawn("", "test");
  ASSERT_FALSE(session.has_value());
  EXPECT_EQ(session.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST(RendererSessionTest, ReportsErrorWhenChildCannotStart)
{
  const auto session = RendererSession::Spawn("/nonexistent/neko-browser-test", "test");
  if (!session.has_value()) {
    SUCCEED(); // Refusing to spawn outright is also acceptable.
    return;
  }
  // A child that cannot serve the protocol must fail the first request
  // instead of hanging.
  const auto loaded =
      session.value()->Load("<html></html>", "text/html", "https://a.invalid/", 80, 60);
  EXPECT_FALSE(loaded.has_value());
  EXPECT_FALSE(session.value()->alive());
}

TEST(RendererSessionTest, LoadsDocumentAndRasterizesViewport)
{
  auto session = SpawnSession();
  ASSERT_TRUE(session != nullptr);
  const auto loaded = LoadFixture(*session);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().message();
  EXPECT_TRUE(loaded.value().changed);
  EXPECT_EQ(loaded.value().title, "Session Fixture");
  EXPECT_GT(loaded.value().content_height, 0.0f);

  RemoteFrame frame;
  const auto snapshot = session->Snapshot(kViewportWidth, kViewportHeight, 0, &frame);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().message();
  EXPECT_EQ(frame.width, kViewportWidth);
  EXPECT_EQ(frame.height, kViewportHeight);
  EXPECT_EQ(frame.rgba.size(), static_cast<std::size_t>(kViewportWidth * kViewportHeight * 4));
  EXPECT_TRUE(HasNonWhitePixel(frame));
  EXPECT_FALSE(snapshot.value().changed); // A snapshot does not itself change content.
  session->Shutdown();
}

TEST(RendererSessionTest, ClickRunsPageScriptAndRepaints)
{
  auto session = SpawnSession();
  ASSERT_TRUE(session != nullptr);
  ASSERT_TRUE(LoadFixture(*session).has_value());
  const std::string html = ReadFixture(kFixtureName);
  const Point button = ElementCenter(html, "#btn");
  const Point log = ElementCenter(html, "#log");

  RemoteFrame before;
  ASSERT_TRUE(session->Snapshot(kViewportWidth, kViewportHeight, 0, &before).has_value());
  EXPECT_FALSE(IsRed(before, log));

  // The fixture's click listener turns the log div red: the reply flags the
  // change and the next frame carries it.
  const auto clicked = session->Click(button.x, button.y);
  ASSERT_TRUE(clicked.has_value()) << clicked.error().message();
  EXPECT_TRUE(clicked.value().changed);

  RemoteFrame after;
  ASSERT_TRUE(session->Snapshot(kViewportWidth, kViewportHeight, 0, &after).has_value());
  EXPECT_TRUE(FramesDiffer(before, after));
  EXPECT_TRUE(IsRed(after, log));
  session->Shutdown();
}

TEST(RendererSessionTest, ClickOnPlainElementChangesNothing)
{
  auto session = SpawnSession();
  ASSERT_TRUE(session != nullptr);
  ASSERT_TRUE(LoadFixture(*session).has_value());
  const Point log = ElementCenter(ReadFixture(kFixtureName), "#log");

  const auto clicked = session->Click(log.x, log.y);
  ASSERT_TRUE(clicked.has_value()) << clicked.error().message();
  EXPECT_FALSE(clicked.value().changed);
  EXPECT_FALSE(clicked.value().handled);
  session->Shutdown();
}

TEST(RendererSessionTest, PumpAdvancesPageTimers)
{
  auto session = SpawnSession();
  ASSERT_TRUE(session != nullptr);
  ASSERT_TRUE(LoadFixture(*session).has_value());

  // The fixture's timer fires 30 ms after load and mutates the DOM once.
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  const auto pumped = session->Pump();
  ASSERT_TRUE(pumped.has_value()) << pumped.error().message();
  EXPECT_TRUE(pumped.value().changed);

  // The timer fired at most once, so a second pump reports no change.
  const auto again = session->Pump();
  ASSERT_TRUE(again.has_value());
  EXPECT_FALSE(again.value().changed);
  session->Shutdown();
}

TEST(RendererSessionTest, HoverReportsLinkTarget)
{
  auto session = SpawnSession();
  ASSERT_TRUE(session != nullptr);
  ASSERT_TRUE(LoadFixture(*session).has_value());
  const Point link = ElementCenter(ReadFixture(kFixtureName), "#next");

  const auto hovered = session->Hover(link.x, link.y);
  ASSERT_TRUE(hovered.has_value()) << hovered.error().message();
  EXPECT_EQ(hovered.value().hover_link, kLinkUrl);

  const auto cleared = session->HoverClear();
  ASSERT_TRUE(cleared.has_value());
  EXPECT_TRUE(cleared.value().hover_link.empty());
  session->Shutdown();
}

TEST(RendererSessionTest, LinkClickReportsRedirectForTheBrowser)
{
  auto session = SpawnSession();
  ASSERT_TRUE(session != nullptr);
  ASSERT_TRUE(LoadFixture(*session).has_value());
  const Point link = ElementCenter(ReadFixture(kFixtureName), "#next");

  const auto clicked = session->Click(link.x, link.y);
  ASSERT_TRUE(clicked.has_value()) << clicked.error().message();
  EXPECT_TRUE(clicked.value().handled);
  // The child never owns navigation: it reports the target for the browser to
  // re-run with its own cookies and content routing.
  EXPECT_EQ(clicked.value().redirect_url, kLinkUrl);
  session->Shutdown();
}

TEST(RendererSessionTest, ScrollOffsetIsReportedBack)
{
  auto session = SpawnSession();
  ASSERT_TRUE(session != nullptr);
  ASSERT_TRUE(LoadFixture(*session).has_value());

  const auto scrolled = session->ScrollTo(42);
  ASSERT_TRUE(scrolled.has_value()) << scrolled.error().message();
  EXPECT_FLOAT_EQ(scrolled.value().scroll_y, 42.0f);
  session->Shutdown();
}

TEST(RendererSessionTest, ChildCrashIsDetectedAndReported)
{
  auto session = SpawnSession();
  ASSERT_TRUE(session != nullptr);
  ASSERT_TRUE(LoadFixture(*session).has_value());
  ASSERT_TRUE(session->alive());

#ifndef _WIN32
  const long pid = session->ProcessId();
  ASSERT_GT(pid, 0);
  ASSERT_EQ(::kill(static_cast<pid_t>(pid), SIGKILL), 0);
  // The next request must fail rather than hang or crash the test process.
  const auto pumped = session->Pump();
  EXPECT_FALSE(pumped.has_value());
  EXPECT_FALSE(session->alive());
  session->Shutdown(); // Reaping a killed child must stay safe.
#else
  GTEST_SKIP() << "POSIX-only crash test";
#endif
}

TEST(RendererSessionTest, ShutdownIsIdempotent)
{
  auto session = SpawnSession();
  ASSERT_TRUE(session != nullptr);
  ASSERT_TRUE(LoadFixture(*session).has_value());
  session->Shutdown();
  EXPECT_FALSE(session->alive());
  session->Shutdown();
  EXPECT_FALSE(session->alive());
}

TEST(RendererSessionTest, SnapshotClampsScrollToContentHeight)
{
  auto session = SpawnSession();
  ASSERT_TRUE(session != nullptr);
  const auto loaded = LoadFixture(*session);
  ASSERT_TRUE(loaded.has_value());
  RemoteFrame frame;
  const auto snapshot = session->Snapshot(kViewportWidth, kViewportHeight, 100000.0f, &frame);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().message();
  // The requested offset is far past the content: it is clamped to the last
  // full viewport (content height - viewport height, or 0).
  const float expected = loaded.value().content_height > static_cast<float>(kViewportHeight)
                             ? loaded.value().content_height - static_cast<float>(kViewportHeight)
                             : 0.0f;
  EXPECT_NEAR(snapshot.value().scroll_y, expected, 1.0f);
  session->Shutdown();
}

} // namespace
} // namespace neko::browser
