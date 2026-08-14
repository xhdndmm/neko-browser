// Unit tests for the browser application layer (BrowserController +
// DownloadManager).  Network traffic is faked; content types are exercised
// with in-test encoders.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <zlib.h>

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "neko/browser/browser_controller.h"
#include "neko/browser/download_manager.h"
#include "neko/dom/query.h"
#include "neko/image/image.h"
#include "neko/network/http.h"
#include "neko/storage/file_util.h"
#include "neko/url/url.h"

namespace neko::browser {
namespace {

using testing::Eq;

// ---------------------------------------------------------------------------
// Temp profile fixture + fake network
// ---------------------------------------------------------------------------

class TempProfile {
 public:
  TempProfile() {
    dir_ = std::filesystem::temp_directory_path() /
           ("neko-browser-test-" + std::to_string(::getpid()) + "-" +
            std::to_string(++seq_));
    std::filesystem::create_directories(dir_);
  }
  ~TempProfile() { std::filesystem::remove_all(dir_); }
  const std::string path() const { return dir_.string(); }

 private:
  static int seq_;
  std::filesystem::path dir_;
};
int TempProfile::seq_ = 0;

// Records every request (url + cookie header) and answers from a route map.
class FakeFetcher {
 public:
  struct Route {
    int status = 200;
    std::vector<network::HttpHeader> headers;
    std::string body;
  };

  void Add(std::string url, Route route) { routes_[std::move(url)] = std::move(route); }

  base::Result<network::HttpResponse> operator()(const url::Url& url,
                                                 std::string_view cookie_header) {
    const std::string key = url.Serialize();
    requests_.push_back(key);
    cookies_seen_.push_back(std::string(cookie_header));
    const auto it = routes_.find(key);
    if (it == routes_.end()) {
      return base::Err(base::Error::Network("no fake route for " + key));
    }
    network::HttpResponse response;
    response.status_code = it->second.status;
    response.headers = it->second.headers;
    response.body = it->second.body;
    return response;
  }

  std::vector<std::string> requests_;
  std::vector<std::string> cookies_seen_;
  std::map<std::string, Route> routes_;
};

std::string LastCookie(const FakeFetcher& f) {
  return f.cookies_seen_.empty() ? std::string() : f.cookies_seen_.back();
}

// ---------------------------------------------------------------------------
// In-test encoders for image / pdf / wav payloads
// ---------------------------------------------------------------------------

std::string Be32(uint32_t v) {
  std::string out(4, '\0');
  out[0] = static_cast<char>((v >> 24) & 0xFF);
  out[1] = static_cast<char>((v >> 16) & 0xFF);
  out[2] = static_cast<char>((v >> 8) & 0xFF);
  out[3] = static_cast<char>(v & 0xFF);
  return out;
}
// RIFF/WAVE stores sizes and numbers little-endian.
std::string Le16(uint16_t v) {
  return {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
}
std::string Le32(uint32_t v) {
  return {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
          static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
}
void AppendChunk(std::string& out, const char* type, std::string_view data) {
  out += Be32(static_cast<uint32_t>(data.size()));
  out.append(type, 4);
  const size_t crc_start = out.size();
  out.append(data);
  const uLong crc = crc32(0L, reinterpret_cast<const Bytef*>(out.data() + crc_start - 4),
                          static_cast<uInt>(4 + data.size()));
  out += Be32(static_cast<uint32_t>(crc));
}

// A 2x2 RGB PNG (red, green / blue, white), filter 0.
std::string MakePng() {
  const std::string ihdr = Be32(2) + Be32(2) + std::string("\x08\x02\x00\x00\x00", 5);
  const std::string scanlines =
      std::string("\x00\xff\x00\x00\x00\xff\x00", 7) +
      std::string("\x00\x00\x00\xff\xff\xff\xff", 7);
  uLongf bound = compressBound(static_cast<uLong>(scanlines.size()));
  std::vector<Bytef> comp(bound);
  uLongf comp_size = bound;
  compress2(comp.data(), &comp_size,
            reinterpret_cast<const Bytef*>(scanlines.data()),
            static_cast<uLong>(scanlines.size()), 9);
  std::string png = "\x89PNG\r\n\x1a\n";
  AppendChunk(png, "IHDR", ihdr);
  AppendChunk(png, "IDAT", std::string_view(reinterpret_cast<const char*>(comp.data()),
                                            comp_size));
  AppendChunk(png, "IEND", "");
  return png;
}

// A minimal one-page PDF with an uncompressed content stream.
std::string MakePdf(std::string_view text) {
  const size_t content_off =
      0;  // placeholder; recomputed via manual assembly below
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
  const std::string content =
      "BT /F1 12 Tf 10 10 Td (" + std::string(text) + ") Tj ET";
  file += "4 0 obj\n<< /Length " + std::to_string(content.size()) +
          " >>\nstream\n" + content + "\nendstream\nendobj\n";
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
  file += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) +
          "\n%%EOF\n";
  return file;
}

// A tiny 16-bit mono WAV (4 samples), little-endian as the spec requires.
std::string MakeWav() {
  const std::string pcm = std::string("\x00\x00\x01\x00\xff\xff\x00\x00", 8);
  const std::string fmt = Le16(1) + Le16(1) + Le32(8000) + Le32(16000) +
                          Le16(2) + Le16(16);
  std::string wav = "RIFF" + Le32(static_cast<uint32_t>(4 + 8 + fmt.size() + 8 + pcm.size())) +
                    "WAVE" + "fmt " + Le32(static_cast<uint32_t>(fmt.size())) + fmt +
                    "data" + Le32(static_cast<uint32_t>(pcm.size())) + pcm;
  return wav;
}

// ---------------------------------------------------------------------------
// BrowserController
// ---------------------------------------------------------------------------

TEST(BrowserControllerTest, NavigatesToHtmlAndRecordsHistory) {
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
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

// Page <script> execution (Phase 8 M2): inline scripts run on load and can
// mutate the DOM.
TEST(BrowserControllerTest, RunsInlineScriptsOnHtmlLoad) {
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add(
      "http://example.com/",
      FakeFetcher::Route{200, {{"content-type", "text/html"}},
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

TEST(BrowserControllerTest, PageScriptConsoleGoesToDevToolsLog) {
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
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

TEST(BrowserControllerTest, PageScriptErrorIsLoggedAndDoesNotStopPage) {
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
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

TEST(BrowserControllerTest, PumpScriptTimersRunsSetTimeout) {
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
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

TEST(BrowserControllerTest, ExternalScriptsAreNotFetchedYet) {
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
                               "<html><body>"
                               "<script src=\"/app.js\"></script>"
                               "<p>x</p>"
                               "</body></html>"});
  fetch.Add("http://example.com/app.js",
            FakeFetcher::Route{200, {{"content-type", "text/javascript"}},
                               "document.title = 'should not run';"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  // External scripts are not executed yet; only the HTML document was fetched.
  EXPECT_EQ(fetch.requests_.size(), 1u);
  EXPECT_EQ(controller.ActiveTab()->page->document()->Title(), "");
}

// In-page <img> subresources are decoded in parallel on a thread pool and
// injected into the page.
TEST(BrowserControllerTest, InjectsMultiplePageImages) {
  TempProfile tp;
  FakeFetcher fetch;
  const std::string png = MakePng();
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
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
  EXPECT_EQ(fetch.requests_.size(), 4u);  // page + 3 images
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

TEST(BrowserControllerTest, ExtractsCookiesAndSendsThemNextRequest) {
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/login",
            FakeFetcher::Route{200, {{"content-type", "text/html"},
                                     {"set-cookie", "session=abc; Path=/"},
                                     {"set-cookie", "theme=dark"}},
                               "<html><body>x</body></html>"});
  fetch.Add("http://example.com/dashboard",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
                               "<html><body>dash</body></html>"});

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

TEST(BrowserControllerTest, RoutesImageContent) {
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

TEST(BrowserControllerTest, RoutesPdfContent) {
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/doc.pdf",
            FakeFetcher::Route{200, {{"content-type", "application/pdf"}},
                               MakePdf("Extracted PDF Words")});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/doc.pdf").has_value());
  Tab* tab = controller.ActiveTab();
  EXPECT_EQ(tab->content_type, ContentType::kPdf);
  EXPECT_EQ(tab->pdf->page_count, 1);
  EXPECT_THAT(tab->pdf->pages[0].text, testing::HasSubstr("Extracted PDF Words"));
}

TEST(BrowserControllerTest, RoutesAudioContent) {
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

TEST(BrowserControllerTest, RoutesPlainText) {
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/notes.txt",
            FakeFetcher::Route{200, {{"content-type", "text/plain"}},
                               "plain text content"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/notes.txt").has_value());
  EXPECT_EQ(controller.ActiveTab()->content_type, ContentType::kText);
  EXPECT_EQ(*controller.ActiveTab()->raw_text, "plain text content");
}

TEST(BrowserControllerTest, ReportsHttpError) {
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

TEST(BrowserControllerTest, BackAndForward) {
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

TEST(BrowserControllerTest, ResolveInput) {
  TempProfile tp;
  FakeFetcher fetch;
  BrowserController controller(tp.path(), std::ref(fetch));
  EXPECT_EQ(controller.ResolveInput("example.com"), "http://example.com");
  EXPECT_EQ(controller.ResolveInput("http://example.com/x"), "http://example.com/x");
  EXPECT_EQ(controller.ResolveInput(""), "");
}

TEST(BrowserControllerTest, BookmarkActiveTab) {
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}},
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

TEST(BrowserControllerTest, TabsAndNewTab) {
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}}, "<p>t</p>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  const int t1 = controller.NewTab();
  const int t2 = controller.NewTab("http://example.com/");
  EXPECT_EQ(controller.tabs().size(), 2u);
  EXPECT_EQ(controller.active_tab(), 1);  // last created tab is active
  controller.CloseTab(t1);
  EXPECT_EQ(controller.tabs().size(), 1u);
  EXPECT_EQ(controller.ActiveTab()->id, t2);
}

TEST(BrowserControllerTest, LoadsLocalFile) {
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

// ---------------------------------------------------------------------------
// DownloadManager
// ---------------------------------------------------------------------------

TEST(DownloadManagerTest, DownloadsToDirectory) {
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/file.bin",
            FakeFetcher::Route{200, {{"content-type", "application/octet-stream"}},
                               "0123456789"});
  DownloadManager manager(tp.path() + "/dl",
                          [&fetch](const url::Url& u) { return fetch(u, ""); });
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

TEST(DownloadManagerTest, UsesContentDispositionFilename) {
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/dl",
            FakeFetcher::Route{200, {{"content-type", "text/plain"},
                                     {"content-disposition", "attachment; filename=\"report.txt\""}},
                               "data"});
  DownloadManager manager(tp.path() + "/dl",
                          [&fetch](const url::Url& u) { return fetch(u, ""); });
  const auto url = url::Url::Parse("http://example.com/dl");
  ASSERT_TRUE(url.has_value());
  auto result = manager.Start(url.value(), "");
  ASSERT_TRUE(result.has_value());
  EXPECT_NE(result.value().filename.find("report.txt"), std::string::npos);
}

TEST(DownloadManagerTest, RecordsFailure) {
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/bad",
            FakeFetcher::Route{500, {}, "error"});
  DownloadManager manager(tp.path() + "/dl",
                          [&fetch](const url::Url& u) { return fetch(u, ""); });
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

}  // namespace
}  // namespace neko::browser
