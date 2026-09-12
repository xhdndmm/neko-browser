// BrowserController renderer-process mode tests (ADR 0016 M2): HTML documents
// rendered by a child process, frames pulled on demand, input forwarded,
// crash isolation with rebuild, and in-process fallback for non-HTML content.
// Each test spawns the real browser binary in --renderer-session mode.

#include "neko/browser/browser_controller.h"
#include "neko/dom/element.h"
#include "neko/dom/query.h"
#include "neko/renderer/page.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#ifndef _WIN32
#include <csignal>
#include <unistd.h>

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

// A temporary profile directory removed when the test finishes.
class TempProfile
{
public:
  TempProfile()
  {
    std::error_code ec;
    path_ = std::filesystem::temp_directory_path(ec) /
            ("neko-renderer-mode-test-" + std::to_string(::getpid()) + "-" +
             std::to_string(counter_++));
    std::filesystem::create_directories(path_, ec);
  }
  ~TempProfile()
  {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  std::string path() const
  {
    return path_.string();
  }

private:
  std::filesystem::path path_;
  static int counter_;
};
int TempProfile::counter_ = 0;

// Serves canned responses; every request is recorded.
class FakeFetcher
{
public:
  struct Route
  {
    int status = 200;
    std::string content_type = "text/html";
    std::string body;
  };

  void Add(std::string url, Route route)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    routes_[std::move(url)] = std::move(route);
  }

  base::Result<network::HttpResponse> operator()(const url::Url& url, std::string_view)
  {
    const std::string key = url.Serialize();
    std::lock_guard<std::mutex> lock(mutex_);
    requests_.push_back(key);
    const auto it = routes_.find(key);
    if (it == routes_.end()) {
      return base::Err(base::Error::Network("no route for " + key));
    }
    network::HttpResponse response;
    response.status_code = it->second.status;
    response.headers.push_back({"content-type", it->second.content_type});
    response.body = it->second.body;
    response.final_url = key;
    return response;
  }

  std::vector<std::string> requests() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

private:
  mutable std::mutex mutex_;
  std::map<std::string, Route> routes_;
  std::vector<std::string> requests_;
};

RendererOptions RendererMode()
{
  RendererOptions options;
  options.enabled = true;
  options.executable = NEKO_BROWSER_BIN;
  return options;
}

// Chrome-free fixture: a 200x60 button whose click turns the log div red, plus
// a link and a one-shot timer (see tests/pages/session_test.html).
constexpr int kViewportWidth = 320;
constexpr int kViewportHeight = 240;
constexpr char kFixtureName[] = "session_test.html";
constexpr char kFixtureUrl[] = "https://test.invalid/session_test.html";

struct Point
{
  float x = 0;
  float y = 0;
};

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
  EXPECT_TRUE(geometry.has_value()) << selector;
  if (!geometry.has_value()) {
    return {};
  }
  return {geometry->x + geometry->width / 2.0f, geometry->y + geometry->height / 2.0f};
}

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

TEST(RendererModeTest, RendersHtmlThroughChildSession)
{
  TempProfile profile;
  FakeFetcher fetch;
  fetch.Add(kFixtureUrl, {200, "text/html", ReadFixture(kFixtureName)});
  BrowserController controller(profile.path(), std::ref(fetch), RendererMode());
  const int tab_id = controller.NewTab();
  ASSERT_TRUE(controller.Navigate(tab_id, kFixtureUrl).has_value());
  controller.SetTabViewport(tab_id, kViewportWidth, kViewportHeight);

  const TabSnapshot snapshot = controller.SnapshotTab(tab_id);
  EXPECT_TRUE(snapshot.remote);
  ASSERT_TRUE(snapshot.remote_frame != nullptr);
  EXPECT_EQ(snapshot.remote_frame->width, kViewportWidth);
  EXPECT_EQ(snapshot.remote_frame->height, kViewportHeight);
  EXPECT_EQ(snapshot.title, "Session Fixture");
  EXPECT_GT(snapshot.remote_content_height, 0.0f);
  // The document lives in the child: no in-process page is published.
  EXPECT_EQ(snapshot.page, nullptr);
  EXPECT_EQ(snapshot.content_type, ContentType::kHtml);
}

TEST(RendererModeTest, ClickIsForwardedAndRepaints)
{
  TempProfile profile;
  FakeFetcher fetch;
  const std::string html = ReadFixture(kFixtureName);
  fetch.Add(kFixtureUrl, {200, "text/html", html});
  BrowserController controller(profile.path(), std::ref(fetch), RendererMode());
  const int tab_id = controller.NewTab();
  ASSERT_TRUE(controller.Navigate(tab_id, kFixtureUrl).has_value());
  controller.SetTabViewport(tab_id, kViewportWidth, kViewportHeight);

  const Point button = ElementCenter(html, "#btn");
  const Point log = ElementCenter(html, "#log");
  ASSERT_TRUE(controller.SnapshotTab(tab_id).remote_frame != nullptr);
  EXPECT_FALSE(IsRed(*controller.SnapshotTab(tab_id).remote_frame, log));

  // The button's click listener turns the log red inside the child; the
  // browser pulls the changed frame.
  EXPECT_FALSE(controller.DispatchPointerClick(tab_id, button.x, button.y));
  const TabSnapshot after = controller.SnapshotTab(tab_id);
  ASSERT_TRUE(after.remote_frame != nullptr);
  EXPECT_TRUE(IsRed(*after.remote_frame, log));
}

TEST(RendererModeTest, ViewportResizeReframesAtNewSize)
{
  TempProfile profile;
  FakeFetcher fetch;
  fetch.Add(kFixtureUrl, {200, "text/html", ReadFixture(kFixtureName)});
  BrowserController controller(profile.path(), std::ref(fetch), RendererMode());
  const int tab_id = controller.NewTab();
  ASSERT_TRUE(controller.Navigate(tab_id, kFixtureUrl).has_value());

  controller.SetTabViewport(tab_id, 400, 300);
  TabSnapshot snapshot = controller.SnapshotTab(tab_id);
  ASSERT_TRUE(snapshot.remote_frame != nullptr);
  EXPECT_EQ(snapshot.remote_frame->width, 400);
  EXPECT_EQ(snapshot.remote_frame->height, 300);

  controller.SetTabViewport(tab_id, 250, 200);
  snapshot = controller.SnapshotTab(tab_id);
  ASSERT_TRUE(snapshot.remote_frame != nullptr);
  EXPECT_EQ(snapshot.remote_frame->width, 250);
  EXPECT_EQ(snapshot.remote_frame->height, 200);
}

TEST(RendererModeTest, SameSiteNavigationReusesTheSession)
{
  TempProfile profile;
  const std::string html = ReadFixture(kFixtureName);
  FakeFetcher fetch;
  fetch.Add("https://site-a.invalid/one.html", {200, "text/html", html});
  fetch.Add("https://site-a.invalid/two.html", {200, "text/html", html});
  fetch.Add("https://site-b.invalid/one.html", {200, "text/html", html});
  BrowserController controller(profile.path(), std::ref(fetch), RendererMode());
  const int tab_id = controller.NewTab();

  ASSERT_TRUE(controller.Navigate(tab_id, "https://site-a.invalid/one.html").has_value());
  Tab* tab = controller.FindTab(tab_id);
  ASSERT_NE(tab, nullptr);
  ASSERT_TRUE(tab->session != nullptr);
  const std::shared_ptr<RendererSession> first = tab->session;

  // Same site: the process is reused (ADR 0016 M2 per-site sessions).
  ASSERT_TRUE(controller.Navigate(tab_id, "https://site-a.invalid/two.html").has_value());
  EXPECT_EQ(controller.FindTab(tab_id)->session, first);

  // Different site: a fresh session is spawned.
  ASSERT_TRUE(controller.Navigate(tab_id, "https://site-b.invalid/one.html").has_value());
  const std::shared_ptr<RendererSession> second = controller.FindTab(tab_id)->session;
  EXPECT_NE(second, nullptr);
  EXPECT_NE(second, first);
  EXPECT_EQ(controller.SnapshotTab(tab_id).origin, "https://site-b.invalid");
}

TEST(RendererModeTest, NonHtmlNavigationStaysInProcess)
{
  TempProfile profile;
  FakeFetcher fetch;
  fetch.Add("https://test.invalid/page.html", {200, "text/html", "<html><body>x</body></html>"});
  fetch.Add("https://test.invalid/notes.txt", {200, "text/plain", "plain notes"});
  BrowserController controller(profile.path(), std::ref(fetch), RendererMode());
  const int tab_id = controller.NewTab();

  ASSERT_TRUE(controller.Navigate(tab_id, "https://test.invalid/page.html").has_value());
  EXPECT_TRUE(controller.SnapshotTab(tab_id).remote);

  // Only HTML goes to the renderer; text (and images / PDF / audio) keep the
  // in-process viewers, so the tab falls back to the local paths.
  ASSERT_TRUE(controller.Navigate(tab_id, "https://test.invalid/notes.txt").has_value());
  const TabSnapshot snapshot = controller.SnapshotTab(tab_id);
  EXPECT_FALSE(snapshot.remote);
  EXPECT_EQ(snapshot.content_type, ContentType::kText);
  ASSERT_TRUE(snapshot.raw_text != nullptr);
  EXPECT_EQ(*snapshot.raw_text, "plain notes");
}

TEST(RendererModeTest, CrashSurfacesAsErrorAndNextNavigationRebuilds)
{
  TempProfile profile;
  FakeFetcher fetch;
  fetch.Add(kFixtureUrl, {200, "text/html", ReadFixture(kFixtureName)});
  BrowserController controller(profile.path(), std::ref(fetch), RendererMode());
  const int tab_id = controller.NewTab();
  ASSERT_TRUE(controller.Navigate(tab_id, kFixtureUrl).has_value());
  Tab* tab = controller.FindTab(tab_id);
  ASSERT_NE(tab, nullptr);
  ASSERT_TRUE(tab->session != nullptr);

#ifndef _WIN32
  const long pid = tab->session->ProcessId();
  ASSERT_GT(pid, 0);
  ASSERT_EQ(::kill(static_cast<pid_t>(pid), SIGKILL), 0);
  // The next worker-thread action observes the dead child and turns the tab
  // into an error page instead of crashing the browser.
  controller.PumpScriptTimers();
  const TabSnapshot broken = controller.SnapshotTab(tab_id);
  EXPECT_EQ(broken.content_type, ContentType::kError);
  EXPECT_FALSE(broken.remote);
  ASSERT_TRUE(broken.error != nullptr);
  EXPECT_THAT(*broken.error, ::testing::HasSubstr("renderer process"));

  // Recovery: the next navigation spawns a fresh session.
  ASSERT_TRUE(controller.Navigate(tab_id, kFixtureUrl).has_value());
  controller.SetTabViewport(tab_id, kViewportWidth, kViewportHeight);
  const TabSnapshot rebuilt = controller.SnapshotTab(tab_id);
  EXPECT_TRUE(rebuilt.remote);
  ASSERT_TRUE(rebuilt.remote_frame != nullptr);
  EXPECT_EQ(rebuilt.title, "Session Fixture");
#else
  GTEST_SKIP() << "POSIX-only crash test";
#endif
}

TEST(RendererModeTest, PageNavigationInsideTheChildRedirectsToTheBrowser)
{
  TempProfile profile;
  // The fixture's timer changes the DOM; this variant navigates instead.
  const std::string html =
      "<!DOCTYPE html><html><head><title>Redirector</title></head><body>"
      "<script>setTimeout(function(){ location.href = 'http://127.0.0.1:9/next.html'; }, 20);"
      "</script></body></html>";
  FakeFetcher fetch;
  fetch.Add("https://test.invalid/start.html", {200, "text/html", html});
  fetch.Add(
      "http://127.0.0.1:9/next.html",
      {200, "text/html", "<html><head><title>Arrived</title></head><body>next</body></html>"});
  BrowserController controller(profile.path(), std::ref(fetch), RendererMode());
  const int tab_id = controller.NewTab();
  ASSERT_TRUE(controller.Navigate(tab_id, "https://test.invalid/start.html").has_value());

  // Let the child's timer become due, then pump: the child reports the
  // navigation and the browser re-runs it with its own fetch path.
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  controller.PumpScriptTimers();

  const TabSnapshot snapshot = controller.SnapshotTab(tab_id);
  EXPECT_EQ(snapshot.url, "http://127.0.0.1:9/next.html");
  EXPECT_EQ(snapshot.title, "Arrived");
  EXPECT_TRUE(snapshot.remote);
  // The browser, not the child, fetched the target (with its own cookies).
  const std::vector<std::string> requests = fetch.requests();
  EXPECT_NE(std::find(requests.begin(), requests.end(), "http://127.0.0.1:9/next.html"),
            requests.end());
}

TEST(RendererModeTest, UnspawnableRendererSurfacesAsError)
{
  TempProfile profile;
  FakeFetcher fetch;
  fetch.Add(kFixtureUrl, {200, "text/html", ReadFixture(kFixtureName)});
  RendererOptions options;
  options.enabled = true;
  options.executable = "/nonexistent/neko-browser";
  BrowserController controller(profile.path(), std::ref(fetch), options);
  const int tab_id = controller.NewTab();
  ASSERT_TRUE(controller.Navigate(tab_id, kFixtureUrl).has_value());
  const TabSnapshot snapshot = controller.SnapshotTab(tab_id);
  // The configured child cannot be spawned: the tab reports it as an error
  // page instead of silently rendering somewhere else.
  EXPECT_EQ(snapshot.content_type, ContentType::kError);
  EXPECT_FALSE(snapshot.remote);
  ASSERT_TRUE(snapshot.error != nullptr);
  EXPECT_THAT(*snapshot.error, ::testing::HasSubstr("renderer process"));
}

} // namespace
} // namespace neko::browser
