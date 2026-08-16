// Unit tests for the browser application layer (BrowserController +
// DownloadManager).  Network traffic is faked; content types are exercised
// with in-test encoders.

#include "neko/browser/browser_controller.h"
#include "neko/browser/download_manager.h"
#include "neko/dom/query.h"
#include "neko/image/image.h"
#include "neko/layout/layout_tree.h"
#include "neko/network/http.h"
#include "neko/storage/file_util.h"
#include "neko/url/url.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <zlib.h>

namespace neko::browser {
namespace {

using testing::Eq;

// ---------------------------------------------------------------------------
// Temp profile fixture + fake network
// ---------------------------------------------------------------------------

class TempProfile
{
public:
  TempProfile()
  {
    dir_ = std::filesystem::temp_directory_path() /
           ("neko-browser-test-" + std::to_string(::getpid()) + "-" + std::to_string(++seq_));
    std::filesystem::create_directories(dir_);
  }
  ~TempProfile()
  {
    std::filesystem::remove_all(dir_);
  }
  const std::string path() const
  {
    return dir_.string();
  }

private:
  static int seq_;
  std::filesystem::path dir_;
};
int TempProfile::seq_ = 0;

// Records every request (url + cookie header) and answers from a route map.
// Thread-safe: the controller may fetch page subresources in parallel on a
// thread pool (see FetchPageImages), so the recorded request lists are
// mutex-guarded.
class FakeFetcher
{
public:
  struct Route
  {
    int status = 200;
    std::vector<network::HttpHeader> headers;
    std::string body;
  };

  void Add(std::string url, Route route)
  {
    routes_[std::move(url)] = std::move(route);
  }

  base::Result<network::HttpResponse> operator()(const url::Url& url,
                                                 std::string_view cookie_header)
  {
    const std::string key = url.Serialize();
    network::HttpResponse response;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      requests_.push_back(key);
      cookies_seen_.push_back(std::string(cookie_header));
      const auto it = routes_.find(key);
      if (it == routes_.end()) {
        return base::Err(base::Error::Network("no fake route for " + key));
      }
      response.status_code = it->second.status;
      response.headers = it->second.headers;
      response.body = it->second.body;
    }
    return response;
  }

  std::vector<std::string> requests_;
  std::vector<std::string> cookies_seen_;
  std::map<std::string, Route> routes_;

private:
  std::mutex mutex_;
};

std::string LastCookie(const FakeFetcher& f)
{
  return f.cookies_seen_.empty() ? std::string() : f.cookies_seen_.back();
}

// ---------------------------------------------------------------------------
// In-test encoders for image / pdf / wav payloads
// ---------------------------------------------------------------------------

std::string Be32(uint32_t v)
{
  std::string out(4, '\0');
  out[0] = static_cast<char>((v >> 24) & 0xFF);
  out[1] = static_cast<char>((v >> 16) & 0xFF);
  out[2] = static_cast<char>((v >> 8) & 0xFF);
  out[3] = static_cast<char>(v & 0xFF);
  return out;
}
// RIFF/WAVE stores sizes and numbers little-endian.
std::string Le16(uint16_t v)
{
  return {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
}
std::string Le32(uint32_t v)
{
  return {static_cast<char>(v & 0xFF),
          static_cast<char>((v >> 8) & 0xFF),
          static_cast<char>((v >> 16) & 0xFF),
          static_cast<char>((v >> 24) & 0xFF)};
}
void AppendChunk(std::string& out, const char* type, std::string_view data)
{
  out += Be32(static_cast<uint32_t>(data.size()));
  out.append(type, 4);
  const size_t crc_start = out.size();
  out.append(data);
  const uLong crc = crc32(0L,
                          reinterpret_cast<const Bytef*>(out.data() + crc_start - 4),
                          static_cast<uInt>(4 + data.size()));
  out += Be32(static_cast<uint32_t>(crc));
}

// A 2x2 RGB PNG (red, green / blue, white), filter 0.
std::string MakePng()
{
  const std::string ihdr = Be32(2) + Be32(2) + std::string("\x08\x02\x00\x00\x00", 5);
  const std::string scanlines = std::string("\x00\xff\x00\x00\x00\xff\x00", 7) +
                                std::string("\x00\x00\x00\xff\xff\xff\xff", 7);
  uLongf bound = compressBound(static_cast<uLong>(scanlines.size()));
  std::vector<Bytef> comp(bound);
  uLongf comp_size = bound;
  compress2(comp.data(),
            &comp_size,
            reinterpret_cast<const Bytef*>(scanlines.data()),
            static_cast<uLong>(scanlines.size()),
            9);
  std::string png = "\x89PNG\r\n\x1a\n";
  AppendChunk(png, "IHDR", ihdr);
  AppendChunk(png, "IDAT", std::string_view(reinterpret_cast<const char*>(comp.data()), comp_size));
  AppendChunk(png, "IEND", "");
  return png;
}

// A minimal one-page PDF with an uncompressed content stream.
std::string MakePdf(std::string_view text)
{
  const size_t content_off = 0; // placeholder; recomputed via manual assembly below
  (void)content_off;
  std::string file = "%PDF-1.4\n";
  const size_t o1 = file.size();
  file += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
  const size_t o2 = file.size();
  file += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
  const size_t o3 = file.size();
  file += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 600] "
          "/Contents 4 0 R >>\nendobj\n";
  const size_t o4 = file.size();
  const std::string content = "BT /F1 12 Tf 10 10 Td (" + std::string(text) + ") Tj ET";
  file += "4 0 obj\n<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
          "\nendstream\nendobj\n";
  const size_t xref = file.size();
  auto pad = [](size_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%010zu", v);
    return std::string(b);
  };
  file += "xref\n0 5\n0000000000 65535 f \n";
  file += pad(o1) + " 00000 n \n";
  file += pad(o2) + " 00000 n \n";
  file += pad(o3) + " 00000 n \n";
  file += pad(o4) + " 00000 n \n";
  file += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
  return file;
}

// A tiny 16-bit mono WAV (4 samples), little-endian as the spec requires.
std::string MakeWav()
{
  const std::string pcm = std::string("\x00\x00\x01\x00\xff\xff\x00\x00", 8);
  const std::string fmt = Le16(1) + Le16(1) + Le32(8000) + Le32(16000) + Le16(2) + Le16(16);
  std::string wav = "RIFF" + Le32(static_cast<uint32_t>(4 + 8 + fmt.size() + 8 + pcm.size())) +
                    "WAVE" + "fmt " + Le32(static_cast<uint32_t>(fmt.size())) + fmt + "data" +
                    Le32(static_cast<uint32_t>(pcm.size())) + pcm;
  return wav;
}

// ---------------------------------------------------------------------------
// BrowserController
// ---------------------------------------------------------------------------

TEST(BrowserControllerTest, NavigatesToHtmlAndRecordsHistory)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>Hello</title></head>"
                               "<body><p>Hi</p></body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  EXPECT_EQ(tab->content_type, ContentType::kHtml);
  EXPECT_EQ(tab->title, "Hello");
  EXPECT_EQ(tab->url, "http://example.com/");
  EXPECT_EQ(tab->page->document()->Title(), "Hello");
  EXPECT_EQ(controller.history().size(), 1u);
  EXPECT_EQ(controller.history().All()[0].url, "http://example.com/");
  EXPECT_EQ(fetch.requests_.size(), 1u);
}

// The tab records the security origin of the loaded page (Phase 10 M1).
TEST(BrowserControllerTest, RecordsPageOrigin)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add(
      "http://example.com:8080/path",
      FakeFetcher::Route{200, {{"content-type", "text/html"}}, "<html><body>x</body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com:8080/path").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  EXPECT_EQ(tab->origin, "http://example.com:8080");
  // The snapshot exposes the same origin to the GUI.
  const TabSnapshot snapshot = controller.SnapshotActiveTab();
  EXPECT_EQ(snapshot.origin, "http://example.com:8080");
}

// Page <script> execution (Phase 8 M2): inline scripts run on load and can
// mutate the DOM.
TEST(BrowserControllerTest, RunsInlineScriptsOnHtmlLoad)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>Before</title>"
                               "<script>document.title = 'After';"
                               "var d = document.createElement('div'); d.id = 'made';"
                               "d.textContent = 'from script';"
                               "document.body.appendChild(d);</script>"
                               "</head><body><p>Hi</p></body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  // The script changed the title and inserted a node.
  EXPECT_EQ(tab->title, "After");
  const std::string dom = tab->page->DumpDom();
  EXPECT_NE(dom.find("id=\"made\""), std::string::npos);
  EXPECT_NE(dom.find("from script"), std::string::npos);
  // The live runtime handle is kept on the tab.
  EXPECT_NE(tab->script_runtime, nullptr);
}

// Finds the first laid-out text run belonging to |target| and returns its
// top-left point (document coordinates, before scroll).
bool FindElementRunPoint(const layout::LayoutBox& box, const dom::Element* target, float& x, float& y)
{
  for (const layout::Line& line : box.lines) {
    for (const layout::TextRun& run : line.runs) {
      if (run.element == target) {
        x = run.x + 1.0f;
        y = run.y + 1.0f;
        return true;
      }
    }
    for (const layout::InlineBox& ib : line.boxes) {
      if (ib.block_box != nullptr && FindElementRunPoint(*ib.block_box, target, x, y)) {
        return true;
      }
    }
  }
  for (const auto& child : box.children) {
    if (FindElementRunPoint(*child, target, x, y)) {
      return true;
    }
  }
  for (const auto& f : box.floats) {
    if (FindElementRunPoint(*f, target, x, y)) {
      return true;
    }
  }
  return false;
}

TEST(BrowserControllerTest, PointerClickRunsClickEventAndNavigates)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
                               "<html><body><a href=\"http://target.example/\">go</a>"
                               "<script>var a=document.querySelector('a');"
                               "window.__clicked=false;"
                               "a.addEventListener('click',function(){window.__clicked=true;});"
                               "</script></body></html>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  tab->page->Layout(800, 600);
  dom::Element* a = dom::QuerySelector(*tab->page->document(), "a");
  ASSERT_NE(a, nullptr);
  float x = 0;
  float y = 0;
  ASSERT_TRUE(FindElementRunPoint(*tab->page->layout_root(), a, x, y));

  EXPECT_TRUE(controller.DispatchPointerClick(tab->id, x, y));
  // The click listener fired, and the default action navigated the link.
  EXPECT_TRUE(tab->script_runtime->Evaluate("window.__clicked").has_value());
  EXPECT_EQ(tab->url, "http://target.example/");
}

TEST(BrowserControllerTest, PointerClickPreventDefaultSkipsNavigation)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
                               "<html><body><a href=\"http://target.example/\">go</a>"
                               "<script>var a=document.querySelector('a');"
                               "a.addEventListener('click',function(e){e.preventDefault();});"
                               "</script></body></html>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  tab->page->Layout(800, 600);
  dom::Element* a = dom::QuerySelector(*tab->page->document(), "a");
  ASSERT_NE(a, nullptr);
  float x = 0;
  float y = 0;
  ASSERT_TRUE(FindElementRunPoint(*tab->page->layout_root(), a, x, y));

  EXPECT_TRUE(controller.DispatchPointerClick(tab->id, x, y));
  // preventDefault() canceled the click: no navigation happened.
  EXPECT_EQ(tab->url, "http://example.com/");
}

TEST(BrowserControllerTest, KeyboardDispatchRunsPageListener)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
                               "<html><body>"
                               "<script>window.__key='';"
                               "document.body.addEventListener('keydown',function(e){"
                               "  window.__key=e.key+':'+e.code+':'+e.cancelable;});"
                               "</script></body></html>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  EXPECT_TRUE(controller.DispatchKeyboard(tab->id, "keydown", "Enter", "Enter"));
  ASSERT_TRUE(tab->script_runtime != nullptr);
  const auto v = tab->script_runtime->Evaluate("window.__key");
  ASSERT_TRUE(v.has_value());
  const auto s = v.value().ToString();
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s.value(), "Enter:Enter:true");
}

TEST(BrowserControllerTest, SubmitButtonSubmitsFormWithEncodedData)
{
  TempProfile tp;
  FakeFetcher fetch;
  // Target page for the form action.
  fetch.Add("http://example.com/search",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
                               "<html><body>results</body></html>"});
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
                               "<html><body>"
                               "<form action=\"/search\">"
                               "<input name=\"q\" type=\"text\" value=\"hello world\">"
                               "<input name=\"agree\" type=\"checkbox\" checked>"
                               "<input name=\"no\" type=\"checkbox\">"
                               "<button type=\"submit\">Go</button>"
                               "</form></body></html>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  tab->page->Layout(800, 600);
  // Find the submit button's laid-out text position and click it.
  dom::Element* button = dom::QuerySelector(*tab->page->document(), "button");
  ASSERT_NE(button, nullptr);
  float x = 0;
  float y = 0;
  ASSERT_TRUE(FindElementRunPoint(*tab->page->layout_root(), button, x, y));
  EXPECT_TRUE(controller.DispatchPointerClick(tab->id, x, y));
  // GET submission: q=hello%20world&agree=on (unchecked boxes omitted).
  EXPECT_EQ(tab->url, "http://example.com/search?q=hello%20world&agree=on");
}

TEST(BrowserControllerTest, SubmitEventPreventDefaultBlocksNavigation)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
                               "<html><body>"
                               "<form action=\"/search\">"
                               "<input name=\"q\" type=\"text\" value=\"x\">"
                               "<button type=\"submit\">Go</button>"
                               "</form>"
                               "<script>document.querySelector('form').addEventListener("
                               "'submit', function(e){ e.preventDefault(); });</script>"
                               "</body></html>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  tab->page->Layout(800, 600);
  dom::Element* button = dom::QuerySelector(*tab->page->document(), "button");
  ASSERT_NE(button, nullptr);
  float x = 0;
  float y = 0;
  ASSERT_TRUE(FindElementRunPoint(*tab->page->layout_root(), button, x, y));
  EXPECT_TRUE(controller.DispatchPointerClick(tab->id, x, y));
  // preventDefault canceled the submit: no navigation.
  EXPECT_EQ(tab->url, "http://example.com/");
}

TEST(BrowserControllerTest, PageScriptConsoleGoesToDevToolsLog)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body>"
                               "<script>console.log('hello from page');"
                               "console.error('page problem');</script>"
                               "</body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  const auto console = controller.SnapshotConsoleLog();
  ASSERT_EQ(console.size(), 2u);
  EXPECT_EQ(console[0].level, "log");
  EXPECT_EQ(console[0].message, "hello from page");
  EXPECT_EQ(console[1].level, "error");
  EXPECT_EQ(console[1].message, "page problem");
}

TEST(BrowserControllerTest, PageScriptErrorIsLoggedAndDoesNotStopPage)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body>"
                               "<script>document.getElementById('nope').x = 1;</script>"
                               "<p>still here</p>"
                               "</body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  EXPECT_NE(tab->page->DumpDom().find("still here"), std::string::npos);
  const auto console = controller.SnapshotConsoleLog();
  ASSERT_EQ(console.size(), 1u);
  EXPECT_EQ(console[0].level, "error");
  EXPECT_NE(console[0].message.find("Uncaught"), std::string::npos);
}

TEST(BrowserControllerTest, PumpScriptTimersRunsSetTimeout)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body>"
                               "<script>"
                               "window._runs = 0;"
                               "setTimeout(function(){ window._runs++; }, 1);"
                               "</script>"
                               "</body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->script_runtime, nullptr);
  // No timers run before the deadline.
  controller.PumpScriptTimers();
  auto runs = tab->script_runtime->Evaluate("window._runs");
  ASSERT_TRUE(runs.has_value());
  auto num = runs.value().ToNumber();
  ASSERT_TRUE(num.has_value());
  EXPECT_DOUBLE_EQ(num.value(), 0.0);

  // After the deadline, pumping runs the timer.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  controller.PumpScriptTimers();
  runs = tab->script_runtime->Evaluate("window._runs");
  ASSERT_TRUE(runs.has_value());
  num = runs.value().ToNumber();
  ASSERT_TRUE(num.has_value());
  EXPECT_DOUBLE_EQ(num.value(), 1.0);
}

// External classic <script src> is fetched and executed on load.
TEST(BrowserControllerTest, ExternalScriptIsFetchedAndRun)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>Before</title>"
                               "<script src=\"/app.js\"></script>"
                               "</head><body><p>x</p></body></html>"});
  fetch.Add("http://example.com/app.js",
            FakeFetcher::Route{
                200, {{"content-type", "text/javascript"}}, "document.title = 'external ran';"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  EXPECT_EQ(fetch.requests_.size(), 2u); // page + script
  EXPECT_EQ(controller.ActiveTab()->title, "external ran");
}

// Classic scripts run in document order: an earlier script defines a global
// that a later script (inline or external) depends on.
TEST(BrowserControllerTest, ExternalScriptsRunInDocumentOrder)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body>"
                               "<script src=\"/a.js\"></script>"
                               "<script>window.order.push('inline-b');</script>"
                               "<script src=\"/c.js\"></script>"
                               "</body></html>"});
  fetch.Add(
      "http://example.com/a.js",
      FakeFetcher::Route{200, {{"content-type", "text/javascript"}}, "window.order = ['a'];"});
  fetch.Add(
      "http://example.com/c.js",
      FakeFetcher::Route{200, {{"content-type", "text/javascript"}}, "window.order.push('c');"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->script_runtime, nullptr);
  auto order = tab->script_runtime->Evaluate("window.order.join(',')");
  ASSERT_TRUE(order.has_value()) << order.error().message();
  auto s = order.value().ToString();
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s.value(), "a,inline-b,c");
}

// defer scripts run after every classic script, regardless of source order.
TEST(BrowserControllerTest, DeferScriptRunsAfterClassic)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body>"
                               "<script defer src=\"/defer.js\"></script>"
                               "<script>window.phase = 'classic';</script>"
                               "</body></html>"});
  fetch.Add("http://example.com/defer.js",
            FakeFetcher::Route{200,
                               {{"content-type", "text/javascript"}},
                               "window.order = [window.phase, 'defer'];"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->script_runtime, nullptr);
  auto order = tab->script_runtime->Evaluate("window.order.join(',')");
  ASSERT_TRUE(order.has_value());
  auto s = order.value().ToString();
  ASSERT_TRUE(s.has_value());
  // The defer script observed the classic phase already having run.
  EXPECT_EQ(s.value(), "classic,defer");
}

// async scripts are fetched and executed after the classic+defer phases.
TEST(BrowserControllerTest, AsyncScriptRuns)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body>"
                               "<script>window.phase = 'start';</script>"
                               "<script async src=\"/async.js\"></script>"
                               "</body></html>"});
  fetch.Add("http://example.com/async.js",
            FakeFetcher::Route{200,
                               {{"content-type", "text/javascript"}},
                               "window.order = [window.phase, 'async'];"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->script_runtime, nullptr);
  auto order = tab->script_runtime->Evaluate("window.order.join(',')");
  ASSERT_TRUE(order.has_value());
  auto s = order.value().ToString();
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s.value(), "start,async");
}

// After every script has run, the document lifecycle events fire: pages that
// register document/window listeners for DOMContentLoaded/load see them run.
TEST(BrowserControllerTest, LifecycleEventsFireAfterScripts)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body>"
                               "<script>"
                               "window.ready = 0; window.loaded = 0; window.bare = 0;"
                               "document.addEventListener('DOMContentLoaded', "
                               "  function() { window.ready++; });"
                               "window.addEventListener('load', "
                               "  function() { window.loaded++; });"
                               "addEventListener('bare-event', "
                               "  function() { window.bare++; });"
                               "</script>"
                               "</body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->script_runtime, nullptr);
  auto ready = tab->script_runtime->Evaluate("window.ready");
  ASSERT_TRUE(ready.has_value());
  ASSERT_TRUE(ready.value().ToNumber().has_value());
  EXPECT_DOUBLE_EQ(ready.value().ToNumber().value(), 1.0);
  auto loaded = tab->script_runtime->Evaluate("window.loaded");
  ASSERT_TRUE(loaded.has_value());
  ASSERT_TRUE(loaded.value().ToNumber().has_value());
  EXPECT_DOUBLE_EQ(loaded.value().ToNumber().value(), 1.0);
  // No one dispatched 'bare-event'; the global alias registered but nothing fired.
  auto bare = tab->script_runtime->Evaluate("window.bare");
  ASSERT_TRUE(bare.has_value());
  ASSERT_TRUE(bare.value().ToNumber().has_value());
  EXPECT_DOUBLE_EQ(bare.value().ToNumber().value(), 0.0);
}

// The page's scripts can use window.localStorage (scoped to the page origin),
// persisting across navigations to the same origin.
TEST(BrowserControllerTest, PageScriptLocalStoragePersists)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body>"
                               "<script>"
                               "localStorage.setItem('key', 'value-1');"
                               "window._stored = localStorage.getItem('key');"
                               "</script>"
                               "</body></html>"});
  fetch.Add("http://example.com/other",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body><p>other</p>"
                               "<script>window._read = localStorage.getItem('key');</script>"
                               "</body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->script_runtime, nullptr);
  auto stored = tab->script_runtime->Evaluate("window._stored");
  ASSERT_TRUE(stored.has_value());
  ASSERT_TRUE(stored.value().ToString().has_value());
  EXPECT_EQ(stored.value().ToString().value(), "value-1");

  // A later page on the same origin reads the persisted value.
  ASSERT_TRUE(controller.NavigateActive("http://example.com/other").has_value());
  Tab* tab2 = controller.ActiveTab();
  ASSERT_NE(tab2, nullptr);
  ASSERT_NE(tab2->script_runtime, nullptr);
  auto read = tab2->script_runtime->Evaluate("window._read");
  ASSERT_TRUE(read.has_value());
  ASSERT_TRUE(read.value().ToString().has_value());
  EXPECT_EQ(read.value().ToString().value(), "value-1");
}

// The page's <video src> subresource is fetched, decoded (FFmpeg) and
// attached; autoplay advances frames on the script/animation pump.
TEST(BrowserControllerTest, PageVideoDecodesAndAutoplays)
{
  // Read the committed H.264 fixture (NEKO_TEST_PAGES_DIR from CMake).
  std::ifstream clip(std::string(NEKO_TEST_PAGES_DIR) + "/sample_8x6_h264.mp4",
                     std::ios::binary);
  ASSERT_TRUE(clip.good());
  const std::string bytes((std::istreambuf_iterator<char>(clip)),
                          std::istreambuf_iterator<char>());
  ASSERT_FALSE(bytes.empty());

  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body>"
                               "<video id=\"v\" src=\"/clip.mp4\" autoplay></video>"
                               "<script>window.__boot = 1;</script>"
                               "</body></html>"});
  fetch.Add("http://example.com/clip.mp4",
            FakeFetcher::Route{200, {{"content-type", "video/mp4"}}, bytes});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->page, nullptr);

  // The decoded first frame is attached to the element with the clip's
  // intrinsic size (8x6).
  dom::Element* video = dom::QuerySelector(*tab->page->document(), "#v");
  ASSERT_NE(video, nullptr);
  const image::Image* frame = tab->page->Find(*video);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->width, 8);
  EXPECT_EQ(frame->height, 6);

  // Autoplay: the first pump tick starts playback at frame 0; after well
  // past one 2 fps frame interval, the next pump advances the displayed
  // frame and bumps the layout version.
  controller.PumpScriptTimers();
  const std::uint64_t before = tab->page->layout_version();
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  controller.PumpScriptTimers();
  EXPECT_NE(tab->page->layout_version(), before);

  // Playback state is reachable from scripts (HTMLMediaElement subset).
  ASSERT_NE(tab->script_runtime, nullptr);
  ASSERT_EQ(tab, controller.ActiveTab());
  auto playing = tab->script_runtime->Evaluate(
      "document.getElementById('v').paused === false");
  ASSERT_TRUE(playing.has_value());
  ASSERT_TRUE(playing.value().ToBoolean().has_value());
  EXPECT_TRUE(playing.value().ToBoolean().value());
  auto duration = tab->script_runtime->Evaluate("document.getElementById('v').duration");
  ASSERT_TRUE(duration.has_value());
  ASSERT_TRUE(duration.value().ToNumber().has_value());
  EXPECT_GT(duration.value().ToNumber().value(), 1.0);
}

// The page's scripts can use window.indexedDB (scoped to the page origin),
// persisting records across navigations to the same origin.
TEST(BrowserControllerTest, PageScriptIndexedDbPersists)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body>"
                               "<script>"
                               "window._idb = 'pending';"
                               "var req = indexedDB.open('kv', 1);"
                               "req.onupgradeneeded = function(e) {"
                               "  window._idb = 'upgraded';"
                               "  var db = e.target.result;"
                               "  db.createObjectStore('items', {keyPath: 'id'});"
                               "};"
                               "req.onsuccess = function(e) {"
                               "  window._idb = 'opened';"
                               "  var db = e.target.result;"
                               "  var tx = db.transaction('items', 'readwrite');"
                               "  tx.objectStore('items').add({id: 1, label: 'stored'});"
                               "};"
                               "</script>"
                               "</body></html>"});
  fetch.Add("http://example.com/other",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body><p>other</p>"
                               "<script>"
                               "window._idb = 'pending';"
                               "var req = indexedDB.open('kv');"
                               "req.onsuccess = function(e) {"
                               "  var db = e.target.result;"
                               "  var tx = db.transaction('items');"
                               "  tx.objectStore('items').get(1).onsuccess = function(e2) {"
                               "    window._idb = e2.target.result.label;"
                               "  };"
                               "};"
                               "</script>"
                               "</body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->script_runtime, nullptr);
  auto opened = tab->script_runtime->Evaluate("window._idb");
  ASSERT_TRUE(opened.has_value());
  ASSERT_TRUE(opened.value().ToString().has_value());
  EXPECT_EQ(opened.value().ToString().value(), "opened");

  // A later page on the same origin reads the persisted record.
  ASSERT_TRUE(controller.NavigateActive("http://example.com/other").has_value());
  Tab* tab2 = controller.ActiveTab();
  ASSERT_NE(tab2, nullptr);
  ASSERT_NE(tab2->script_runtime, nullptr);
  auto read = tab2->script_runtime->Evaluate("window._idb");
  ASSERT_TRUE(read.has_value());
  ASSERT_TRUE(read.value().ToString().has_value());
  EXPECT_EQ(read.value().ToString().value(), "stored");
}

// window.fetch resolves relative URLs and surfaces the response to the page.
TEST(BrowserControllerTest, PageScriptFetch)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body>"
                               "<script>"
                               "fetch('/data.json').then(function(r){"
                               "  window._status = r.status;"
                               "  return r.json();"
                               "}).then(function(d){"
                               "  window._name = d.name;"
                               "});"
                               "</script>"
                               "</body></html>"});
  fetch.Add(
      "http://example.com/data.json",
      FakeFetcher::Route{200, {{"content-type", "application/json"}}, "{\"name\": \"neko\"}"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->script_runtime, nullptr);
  auto status = tab->script_runtime->Evaluate("window._status");
  ASSERT_TRUE(status.has_value());
  ASSERT_TRUE(status.value().ToNumber().has_value());
  EXPECT_DOUBLE_EQ(status.value().ToNumber().value(), 200.0);
  auto name = tab->script_runtime->Evaluate("window._name");
  ASSERT_TRUE(name.has_value());
  ASSERT_TRUE(name.value().ToString().has_value());
  EXPECT_EQ(name.value().ToString().value(), "neko");
}

// A failed external fetch is logged and does not stop the remaining scripts.
TEST(BrowserControllerTest, FailedExternalScriptDoesNotStopOthers)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body>"
                               "<script src=\"/missing.js\"></script>"
                               "<script>document.title = 'still ran';</script>"
                               "</body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  EXPECT_EQ(controller.ActiveTab()->title, "still ran");
  const auto console = controller.SnapshotConsoleLog();
  ASSERT_EQ(console.size(), 1u);
  EXPECT_EQ(console[0].level, "error");
  EXPECT_NE(console[0].message.find("fetch failed"), std::string::npos);
}

// A page script can navigate the browser by assigning window.location (e.g.
// a JS redirect page); the controller acts on it instead of publishing the
// script's own document.
TEST(BrowserControllerTest, PageScriptLocationHrefNavigates)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>Redirector</title>"
                               "<script>location.href = 'http://example.com/final';</script>"
                               "</head><body>redirecting</body></html>"});
  fetch.Add("http://example.com/final",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>Final</title></head><body>done</body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  // The script's own page must not be published; the requested URL is loaded.
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  EXPECT_EQ(tab->url, "http://example.com/final");
  EXPECT_EQ(tab->title, "Final");
  EXPECT_EQ(tab->page->document()->Title(), "Final");
  // Two network requests happened: the redirector page and the target.
  EXPECT_EQ(fetch.requests_.size(), 2u);
}

// location.reload() from a script reloads the current page.
TEST(BrowserControllerTest, PageScriptLocationReloadRefetches)
{
  TempProfile tp;
  int served = 0;
  struct CountingFetcher
  {
    int* served;
    base::Result<network::HttpResponse> operator()(const url::Url& url, std::string_view cookie)
    {
      (void)url;
      (void)cookie;
      ++*served;
      network::HttpResponse response;
      response.status_code = 200;
      response.headers = {{"content-type", "text/html"}};
      if (*served == 1) {
        // First response: a script that reloads once.
        response.body = "<html><head><title>First</title>"
                        "<script>location.reload();</script></head><body>f</body></html>";
      } else {
        // Second response: no script, the chain stops.
        response.body = "<html><head><title>Second</title></head><body>s</body></html>";
      }
      return response;
    }
  } cf{&served};

  BrowserController controller(tp.path(), std::ref(cf));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  // reload() refetches the current URL; the second response has no script, so
  // the chain stops there.
  EXPECT_EQ(served, 2);
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  EXPECT_EQ(tab->url, "http://example.com/");
  EXPECT_EQ(tab->page->document()->Title(), "Second");
}

// In-page <img> subresources are decoded in parallel on a thread pool and
// injected into the page.
TEST(BrowserControllerTest, InjectsMultiplePageImages)
{
  TempProfile tp;
  FakeFetcher fetch;
  const std::string png = MakePng();
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body>"
                               "<img src=\"/a.png\"><img src=\"/b.png\"><img src=\"/c.png\">"
                               "</body></html>"});
  fetch.Add("http://example.com/a.png",
            FakeFetcher::Route{200, {{"content-type", "image/png"}}, png});
  fetch.Add("http://example.com/b.png",
            FakeFetcher::Route{200, {{"content-type", "image/png"}}, png});
  fetch.Add("http://example.com/c.png",
            FakeFetcher::Route{200, {{"content-type", "image/png"}}, png});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  // All three image requests went out and every <img> got a decoded 2x2 image.
  EXPECT_EQ(fetch.requests_.size(), 4u); // page + 3 images
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  const std::vector<dom::Element*> imgs = dom::QuerySelectorAll(*tab->page->document(), "img");
  ASSERT_EQ(imgs.size(), 3u);
  for (const dom::Element* element : imgs) {
    const image::Image* decoded = tab->page->Find(*element);
    ASSERT_NE(decoded, nullptr);
    EXPECT_EQ(decoded->width, 2);
    EXPECT_EQ(decoded->height, 2);
  }
}

// External <link rel=stylesheet> sheets are fetched, parsed and applied before
// the page is published (real pages put most of their CSS in external files).
TEST(BrowserControllerTest, FetchesAndAppliesExternalStylesheets)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head>"
                               "<link rel=\"stylesheet\" href=\"/style.css\">"
                               "<link rel=\"icon\" href=\"/favicon.ico\">"
                               "</head><body><p id=\"t\">x</p></body></html>"});
  fetch.Add("http://example.com/style.css",
            FakeFetcher::Route{200,
                               {{"content-type", "text/css"}},
                               "#t { color: rgb(255, 0, 0); font-size: 24px; }"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  // The stylesheet (and only it — the icon link is not a stylesheet) was
  // fetched alongside the page.
  EXPECT_NE(
      std::find(fetch.requests_.begin(), fetch.requests_.end(), "http://example.com/style.css"),
      fetch.requests_.end());
  EXPECT_EQ(
      std::find(fetch.requests_.begin(), fetch.requests_.end(), "http://example.com/favicon.ico"),
      fetch.requests_.end());

  // The computed style reflects the external sheet (red text, 24px).
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  dom::Element* target = dom::QuerySelector(*tab->page->document(), "#t");
  ASSERT_NE(target, nullptr);
  const style::ComputedStyle& style = tab->page->styles().StyleFor(*target);
  ASSERT_TRUE(style.color.has_value());
  EXPECT_EQ(style.color.value().r, 255);
  EXPECT_EQ(style.color.value().g, 0);
  EXPECT_EQ(style.color.value().b, 0);
  EXPECT_FLOAT_EQ(style.font_size, 24.0f);
}

TEST(BrowserControllerTest, ExternalStylesheetsCascadeInDocumentOrder)
{
  // Two external sheets with equal-specificity declarations: the later one
  // in document order must win.  (Regression: sheets were collected in
  // reverse document order, inverting the cascade.)
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head>"
                               "<link rel=\"stylesheet\" href=\"/a.css\">"
                               "<link rel=\"stylesheet\" href=\"/b.css\">"
                               "</head><body><p id=\"t\">x</p></body></html>"});
  fetch.Add("http://example.com/a.css",
            FakeFetcher::Route{200, {{"content-type", "text/css"}}, "#t { color: rgb(255, 0, 0); }"});
  fetch.Add("http://example.com/b.css",
            FakeFetcher::Route{200, {{"content-type", "text/css"}}, "#t { color: rgb(0, 0, 255); }"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  dom::Element* target = dom::QuerySelector(*tab->page->document(), "#t");
  ASSERT_NE(target, nullptr);
  const style::ComputedStyle& style = tab->page->styles().StyleFor(*target);
  ASSERT_TRUE(style.color.has_value());
  // b.css comes later in the document, so blue wins over red.
  EXPECT_EQ(style.color.value().r, 0);
  EXPECT_EQ(style.color.value().g, 0);
  EXPECT_EQ(style.color.value().b, 255);
}

// A failing external stylesheet is skipped without stopping the page.
TEST(BrowserControllerTest, MissingExternalStylesheetIsSkipped)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head>"
                               "<link rel=\"stylesheet\" href=\"/missing.css\">"
                               "</head><body><p>ok</p></body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  EXPECT_EQ(tab->content_type, ContentType::kHtml);
  EXPECT_NE(tab->page, nullptr);
}

TEST(BrowserControllerTest, ExtractsCookiesAndSendsThemNextRequest)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/login",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"},
                                {"set-cookie", "session=abc; Path=/"},
                                {"set-cookie", "theme=dark"}},
                               "<html><body>x</body></html>"});
  fetch.Add(
      "http://example.com/dashboard",
      FakeFetcher::Route{200, {{"content-type", "text/html"}}, "<html><body>dash</body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/login").has_value());
  EXPECT_EQ(controller.cookies().size(), 2u);

  // The first request went out with no cookies.
  EXPECT_EQ(fetch.cookies_seen_[0], "");
  // Second navigation sends both cookies.
  ASSERT_TRUE(controller.NavigateActive("http://example.com/dashboard").has_value());
  const std::string cookie = LastCookie(fetch);
  EXPECT_NE(cookie.find("session=abc"), std::string::npos);
  EXPECT_NE(cookie.find("theme=dark"), std::string::npos);
}

TEST(BrowserControllerTest, RoutesImageContent)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/img.png",
            FakeFetcher::Route{200, {{"content-type", "image/png"}}, MakePng()});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/img.png").has_value());
  Tab* tab = controller.ActiveTab();
  EXPECT_EQ(tab->content_type, ContentType::kImage);
  EXPECT_EQ(tab->image->width, 2);
  EXPECT_EQ(tab->image->height, 2);
}

TEST(BrowserControllerTest, RoutesPdfContent)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/doc.pdf",
            FakeFetcher::Route{
                200, {{"content-type", "application/pdf"}}, MakePdf("Extracted PDF Words")});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/doc.pdf").has_value());
  Tab* tab = controller.ActiveTab();
  EXPECT_EQ(tab->content_type, ContentType::kPdf);
  EXPECT_EQ(tab->pdf->page_count, 1);
  EXPECT_THAT(tab->pdf->pages[0].text, testing::HasSubstr("Extracted PDF Words"));
}

TEST(BrowserControllerTest, RoutesAudioContent)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/sound.wav",
            FakeFetcher::Route{200, {{"content-type", "audio/wav"}}, MakeWav()});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/sound.wav").has_value());
  Tab* tab = controller.ActiveTab();
  EXPECT_EQ(tab->content_type, ContentType::kAudio);
  EXPECT_EQ(tab->audio->sample_rate, 8000);
  EXPECT_EQ(tab->audio->samples.size(), 4u);
}

TEST(BrowserControllerTest, RoutesPlainText)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/notes.txt",
            FakeFetcher::Route{200, {{"content-type", "text/plain"}}, "plain text content"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/notes.txt").has_value());
  EXPECT_EQ(controller.ActiveTab()->content_type, ContentType::kText);
  EXPECT_EQ(*controller.ActiveTab()->raw_text, "plain text content");
}

TEST(BrowserControllerTest, ReportsHttpError)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/missing",
            FakeFetcher::Route{404, {{"content-type", "text/html"}}, "not found"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/missing").has_value());
  Tab* tab = controller.ActiveTab();
  EXPECT_EQ(tab->content_type, ContentType::kError);
  EXPECT_NE(tab->error->find("404"), std::string::npos);
  // Errors still land in the network log (DevTools).
  ASSERT_FALSE(controller.network_log().empty());
  EXPECT_EQ(controller.network_log().back().status, 404);
}

TEST(BrowserControllerTest, BackAndForward)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/a",
            FakeFetcher::Route{200, {{"content-type", "text/html"}}, "<p>a</p>"});
  fetch.Add("http://example.com/b",
            FakeFetcher::Route{200, {{"content-type", "text/html"}}, "<p>b</p>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/a").has_value());
  ASSERT_TRUE(controller.NavigateActive("http://example.com/b").has_value());
  EXPECT_EQ(controller.ActiveTab()->url, "http://example.com/b");

  controller.Back();
  EXPECT_EQ(controller.ActiveTab()->url, "http://example.com/a");
  controller.Forward();
  EXPECT_EQ(controller.ActiveTab()->url, "http://example.com/b");
}

TEST(BrowserControllerTest, ResolveInput)
{
  TempProfile tp;
  FakeFetcher fetch;
  BrowserController controller(tp.path(), std::ref(fetch));
  EXPECT_EQ(controller.ResolveInput("example.com"), "http://example.com");
  EXPECT_EQ(controller.ResolveInput("http://example.com/x"), "http://example.com/x");
  EXPECT_EQ(controller.ResolveInput(""), "");
}

TEST(BrowserControllerTest, BookmarkActiveTab)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>Example</title></head></html>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  auto id = controller.BookmarkActive("Work");
  ASSERT_TRUE(id.has_value());
  ASSERT_EQ(controller.bookmarks().size(), 1u);
  EXPECT_EQ(controller.bookmarks().All()[0].title, "Example");
  EXPECT_EQ(controller.bookmarks().All()[0].folder, "Work");
}

TEST(BrowserControllerTest, TabsAndNewTab)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}}, "<p>t</p>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  const int t1 = controller.NewTab();
  const int t2 = controller.NewTab("http://example.com/");
  EXPECT_EQ(controller.tabs().size(), 2u);
  EXPECT_EQ(controller.active_tab(), 1); // last created tab is active
  controller.CloseTab(t1);
  EXPECT_EQ(controller.tabs().size(), 1u);
  EXPECT_EQ(controller.ActiveTab()->id, t2);
}

TEST(BrowserControllerTest, LoadsLocalFile)
{
  TempProfile tp;
  FakeFetcher fetch;
  BrowserController controller(tp.path(), std::ref(fetch));
  const std::string file = tp.path() + "/page.html";
  ASSERT_TRUE(storage::WriteFileAtomic(file, "<html><title>Local</title></html>").has_value());
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive(file).has_value());
  EXPECT_EQ(controller.ActiveTab()->content_type, ContentType::kHtml);
  EXPECT_EQ(controller.ActiveTab()->title, "Local");
}

TEST(BrowserControllerTest, LocalFormSubmitKeepsQueryAndLoadsFile)
{
  TempProfile tp;
  FakeFetcher fetch;
  BrowserController controller(tp.path(), std::ref(fetch));
  const std::string file = tp.path() + "/form.html";
  ASSERT_TRUE(storage::WriteFileAtomic(
      file, "<html><title>F</title><body>"
            "<form action=\"form.html\"><input name=\"q\" value=\"hi\">"
            "<button type=\"submit\">Go</button></form></body></html>")
                 .has_value());
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive(file).has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  tab->page->Layout(800, 600);
  dom::Element* button = dom::QuerySelector(*tab->page->document(), "button");
  ASSERT_NE(button, nullptr);
  float x = 0;
  float y = 0;
  ASSERT_TRUE(FindElementRunPoint(*tab->page->layout_root(), button, x, y));
  EXPECT_TRUE(controller.DispatchPointerClick(tab->id, x, y));
  // The query stays in the URL; the file on disk loads without it.
  EXPECT_EQ(tab->url, file + "?q=hi");
  EXPECT_EQ(tab->content_type, ContentType::kHtml);
  EXPECT_EQ(tab->title, "F");
}

TEST(BrowserControllerTest, FocusAndTypeIntoInput)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
                               "<html><body><input id=\"q\" value=\"hi\"></body></html>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  tab->page->Layout(800, 600);
  dom::Element* input = dom::QuerySelector(*tab->page->document(), "#q");
  ASSERT_NE(input, nullptr);
  float x = 0;
  float y = 0;
  ASSERT_TRUE(FindElementRunPoint(*tab->page->layout_root(), input, x, y));

  // Clicking the input focuses it.
  controller.DispatchPointerClick(tab->id, x, y);
  EXPECT_EQ(tab->focused_element, input);
  EXPECT_EQ(tab->page->FocusedElement(), input);

  // Typing appends to the value.
  EXPECT_TRUE(controller.DispatchKeyboard(tab->id, "keydown", "a", "KeyA"));
  EXPECT_TRUE(controller.DispatchKeyboard(tab->id, "keydown", "b", "KeyB"));
  EXPECT_EQ(std::string(input->GetAttribute("value").value_or("")), "hiab");

  // Backspace deletes the last character.
  EXPECT_TRUE(controller.DispatchKeyboard(tab->id, "keydown", "Backspace", "Backspace"));
  EXPECT_EQ(std::string(input->GetAttribute("value").value_or("")), "hia");
}

TEST(BrowserControllerTest, EnterInFocusedInputSubmitsForm)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/search",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
                               "<html><body>results</body></html>"});
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
                               "<html><body>"
                               "<form action=\"/search\"><input id=\"q\" name=\"q\" value=\"go\"></form>"
                               "</body></html>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  tab->page->Layout(800, 600);
  dom::Element* input = dom::QuerySelector(*tab->page->document(), "#q");
  ASSERT_NE(input, nullptr);
  float x = 0;
  float y = 0;
  ASSERT_TRUE(FindElementRunPoint(*tab->page->layout_root(), input, x, y));
  controller.DispatchPointerClick(tab->id, x, y);
  EXPECT_EQ(tab->focused_element, input);
  // Enter in a text input submits its form.
  EXPECT_TRUE(controller.DispatchKeyboard(tab->id, "keydown", "Enter", "Enter"));
  EXPECT_EQ(tab->url, "http://example.com/search?q=go");
}

TEST(BrowserControllerTest, ElementGeometryApisReflectLayout)
{
  TempProfile tp;
  FakeFetcher fetch;
  // Default (content-box) sizing: width:100px is the content width, so the
  // border box adds 2px borders + 4px padding each side.
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
                               "<html><body style=\"margin:0\">"
                               "<div id=\"box\" style=\"width:100px;height:50px;margin:10px;"
                               "border:2px solid black;padding:4px\">x<span id=\"sp\">text</span></div>"
                               "<script>"
                               "var b = document.getElementById('box');"
                               "b.setAttribute('data-w', b.offsetWidth);"
                               "b.setAttribute('data-h', b.offsetHeight);"
                               "b.setAttribute('data-x', b.getBoundingClientRect().x);"
                               "b.setAttribute('data-y', b.getBoundingClientRect().y);"
                               "b.setAttribute('data-cw', b.clientWidth);"
                               "b.setAttribute('data-ch', b.clientHeight);"
                               "b.setAttribute('data-ct', b.clientTop);"
                               "b.setAttribute('data-cl', b.clientLeft);"
                               "b.setAttribute('data-ot', b.offsetTop);"
                               "var s = document.getElementById('sp');"
                               "s.setAttribute('data-w', s.offsetWidth);"
                               "s.setAttribute('data-sx', s.getBoundingClientRect().x);"
                               "</script></body></html>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  tab->page->Layout(800, 600);
  dom::Element* box = dom::QuerySelector(*tab->page->document(), "#box");
  ASSERT_NE(box, nullptr);
  EXPECT_EQ(box->GetAttribute("data-w"), "112"); // 100 + 2*2 border + 2*4 padding
  EXPECT_EQ(box->GetAttribute("data-h"), "62");  // 50 + 4 + 8
  EXPECT_EQ(box->GetAttribute("data-x"), "10");  // margin 10, body margin 0
  EXPECT_EQ(box->GetAttribute("data-y"), "10");
  EXPECT_EQ(box->GetAttribute("data-cw"), "108"); // 112 - 2*2 border
  EXPECT_EQ(box->GetAttribute("data-ch"), "58");  // 62 - 2*2 border
  EXPECT_EQ(box->GetAttribute("data-ct"), "2");   // border-top
  EXPECT_EQ(box->GetAttribute("data-cl"), "2");   // border-left
  EXPECT_EQ(box->GetAttribute("data-ot"), "10");  // document coordinate (offsetParent = body)
  dom::Element* sp = dom::QuerySelector(*tab->page->document(), "#sp");
  ASSERT_NE(sp, nullptr);
  // An inline element has no box of its own; its geometry aggregates its text
  // runs, so it still reports a real size and position.
  EXPECT_TRUE(sp->GetAttribute("data-w").has_value());
  EXPECT_GT(std::stod(std::string(sp->GetAttribute("data-w").value())), 0);
  EXPECT_TRUE(sp->GetAttribute("data-sx").has_value());
  EXPECT_GT(std::stod(std::string(sp->GetAttribute("data-sx").value())), 0);
}

TEST(BrowserControllerTest, ClickRunsElementOnclickHandler)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
                               "<html><body style=\"margin:0\">"
                               "<button id=\"b\" style=\"width:100px;height:40px\">go</button>"
                               "<script>"
                               "window.__n = 0;"
                               "var b = document.getElementById('b');"
                               "b.onclick = function(ev){"
                               "  b.setAttribute('data-n', ++window.__n);"
                               "  b.setAttribute('data-cx', ev.clientX);"
                               "};"
                               "</script></body></html>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  tab->page->Layout(800, 600);
  dom::Element* button = dom::QuerySelector(*tab->page->document(), "#b");
  ASSERT_NE(button, nullptr);
  float x = 0;
  float y = 0;
  ASSERT_TRUE(FindElementRunPoint(*tab->page->layout_root(), button, x, y));
  controller.DispatchPointerClick(tab->id, x, y);
  EXPECT_EQ(button->GetAttribute("data-n"), "1");
  // The click event is a MouseEvent with client coordinates.
  EXPECT_GT(std::stod(std::string(button->GetAttribute("data-cx").value_or("0"))), 0);
}

TEST(BrowserControllerTest, OnclickPreventDefaultBlocksNavigation)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/nav",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
                               "<html><body>nav</body></html>"});
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
                               "<html><body style=\"margin:0\">"
                               "<a id=\"lk\" href=\"/nav\">go</a>"
                               "<script>"
                               "document.getElementById('lk').onclick = function(ev){"
                               "  ev.preventDefault();"
                               "};"
                               "</script></body></html>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  tab->page->Layout(800, 600);
  dom::Element* link = dom::QuerySelector(*tab->page->document(), "#lk");
  ASSERT_NE(link, nullptr);
  float x = 0;
  float y = 0;
  ASSERT_TRUE(FindElementRunPoint(*tab->page->layout_root(), link, x, y));
  controller.DispatchPointerClick(tab->id, x, y);
  EXPECT_EQ(tab->url, "http://example.com/"); // still on the page
}

TEST(BrowserControllerTest, TypingFiresInputEvent)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
                               "<html><body style=\"margin:0\">"
                               "<input id=\"q\" value=\"x\">"
                               "<script>"
                               "var q = document.getElementById('q');"
                               "q.oninput = function(){ q.setAttribute('data-v', q.value); };"
                               "q.addEventListener('input', function(){"
                               "  q.setAttribute('data-n', q.value.length);"
                               "});"
                               "</script></body></html>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  tab->page->Layout(800, 600);
  dom::Element* input = dom::QuerySelector(*tab->page->document(), "#q");
  ASSERT_NE(input, nullptr);
  float x = 0;
  float y = 0;
  ASSERT_TRUE(FindElementRunPoint(*tab->page->layout_root(), input, x, y));
  controller.DispatchPointerClick(tab->id, x, y);
  EXPECT_TRUE(controller.DispatchKeyboard(tab->id, "keydown", "a", "KeyA"));
  // oninput + input listener both fire; the value reflects the typed char.
  EXPECT_EQ(input->GetAttribute("data-v"), "xa");
  EXPECT_EQ(input->GetAttribute("data-n"), "2");
}

TEST(BrowserControllerTest, FocusAndBlurFireOnClick)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
                               "<html><body style=\"margin:0\">"
                               "<input id=\"q\" value=\"x\">"
                               "<script>"
                               "var q = document.getElementById('q');"
                               "q.onfocus = function(){ q.setAttribute('data-f','1'); };"
                               "q.onblur = function(){ q.setAttribute('data-f','2'); };"
                               "</script></body></html>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  tab->page->Layout(800, 600);
  dom::Element* input = dom::QuerySelector(*tab->page->document(), "#q");
  ASSERT_NE(input, nullptr);
  float x = 0;
  float y = 0;
  ASSERT_TRUE(FindElementRunPoint(*tab->page->layout_root(), input, x, y));
  // Clicking the input focuses it -> onfocus.
  controller.DispatchPointerClick(tab->id, x, y);
  EXPECT_EQ(input->GetAttribute("data-f"), "1");
  // Clicking the body (right of the ~180px input) blurs it -> onblur.
  controller.DispatchPointerClick(tab->id, 400, 5);
  EXPECT_EQ(input->GetAttribute("data-f"), "2");
}

TEST(BrowserControllerTest, HoverFiresMouseOverAndOut)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
                               "<html><body style=\"margin:0\">"
                               "<div id=\"a\" style=\"width:100px;height:50px\">A</div>"
                               "<div id=\"b\" style=\"width:100px;height:50px\">B</div>"
                               "<script>"
                               "window.__log = [];"
                               "function log(s){ window.__log.push(s);"
                               "  document.body.setAttribute('data-log', window.__log.join(',')); }"
                               "var a = document.getElementById('a');"
                               "var b = document.getElementById('b');"
                               "a.addEventListener('mouseover', function(){ log('over-a'); });"
                               "a.addEventListener('mouseout', function(){ log('out-a'); });"
                               "b.addEventListener('mouseover', function(){ log('over-b'); });"
                               "b.addEventListener('mouseout', function(){ log('out-b'); });"
                               "</script></body></html>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  tab->page->Layout(800, 600);
  dom::Element* body = dom::QuerySelector(*tab->page->document(), "body");
  ASSERT_NE(body, nullptr);
  // #a spans (0,0)-(100,50); #b is below it at y>=50.
  controller.DispatchHover(tab->id, 10, 10);  // enter #a
  controller.DispatchHover(tab->id, 110, 10); // leave #a -> body
  controller.DispatchHover(tab->id, 10, 60);  // enter #b
  EXPECT_EQ(body->GetAttribute("data-log"), "over-a,out-a,over-b");
  // Clearing the hover fires mouseout on the current element (#b).
  controller.DispatchHoverClear(tab->id);
  EXPECT_EQ(body->GetAttribute("data-log"), "over-a,out-a,over-b,out-b");
}

// ---------------------------------------------------------------------------
// DownloadManager
// ---------------------------------------------------------------------------

TEST(DownloadManagerTest, DownloadsToDirectory)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/file.bin",
            FakeFetcher::Route{200, {{"content-type", "application/octet-stream"}}, "0123456789"});
  DownloadManager manager(tp.path() + "/dl", [&fetch](const url::Url& u) { return fetch(u, ""); });
  const auto url = url::Url::Parse("http://example.com/file.bin");
  ASSERT_TRUE(url.has_value());
  auto result = manager.Start(url.value(), "session=abc");
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(result.value().state, DownloadState::kCompleted);
  EXPECT_EQ(result.value().received_bytes, 10);
  EXPECT_EQ(manager.size(), 1u);
  EXPECT_NE(result.value().filename.find("file.bin"), std::string::npos);
  auto data = storage::ReadFile(result.value().filename);
  ASSERT_TRUE(data.has_value());
  EXPECT_EQ(data.value(), "0123456789");
}

TEST(DownloadManagerTest, UsesContentDispositionFilename)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/dl",
            FakeFetcher::Route{200,
                               {{"content-type", "text/plain"},
                                {"content-disposition", "attachment; filename=\"report.txt\""}},
                               "data"});
  DownloadManager manager(tp.path() + "/dl", [&fetch](const url::Url& u) { return fetch(u, ""); });
  const auto url = url::Url::Parse("http://example.com/dl");
  ASSERT_TRUE(url.has_value());
  auto result = manager.Start(url.value(), "");
  ASSERT_TRUE(result.has_value());
  EXPECT_NE(result.value().filename.find("report.txt"), std::string::npos);
}

TEST(DownloadManagerTest, RecordsFailure)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/bad", FakeFetcher::Route{500, {}, "error"});
  DownloadManager manager(tp.path() + "/dl", [&fetch](const url::Url& u) { return fetch(u, ""); });
  const auto url = url::Url::Parse("http://example.com/bad");
  ASSERT_TRUE(url.has_value());
  auto result = manager.Start(url.value(), "");
  // 5xx is not treated as a download failure here (body still written), so
  // instead exercise a network-level failure: an unrouted URL.
  (void)result;
  const auto missing = url::Url::Parse("http://example.com/unrouted");
  ASSERT_TRUE(missing.has_value());
  auto failed = manager.Start(missing.value(), "");
  EXPECT_FALSE(failed.has_value());
  ASSERT_EQ(manager.size(), 2u);
  EXPECT_EQ(manager.items()[1].state, DownloadState::kFailed);
  EXPECT_FALSE(manager.items()[1].error.empty());
}

} // namespace
} // namespace neko::browser
