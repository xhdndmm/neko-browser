// Unit tests for the browser application layer (BrowserController +
// DownloadManager).  Network traffic is faked; content types are exercised
// with in-test encoders.

#include "neko/browser/browser_controller.h"
#include "neko/browser/download_manager.h"
#include "neko/dom/query.h"
#include "neko/graphics/system_fonts.h"
#include "neko/image/image.h"
#include "neko/layout/layout_tree.h"
#include "neko/network/http.h"
#include "neko/storage/file_util.h"
#include "neko/url/url.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <zlib.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <process.h>
#endif

namespace neko::browser {
namespace {

// Temp directory names must be unique per process.  MSVC exposes the process
// id as _getpid (<process.h>), POSIX as getpid (<unistd.h>).
long CurrentProcessId()
{
#ifdef _WIN32
  return static_cast<long>(::_getpid());
#else
  return static_cast<long>(::getpid());
#endif
}

using testing::Eq;

// ---------------------------------------------------------------------------
// Temp profile fixture + fake network
// ---------------------------------------------------------------------------

class TempProfile
{
public:
  TempProfile()
  {
    dir_ =
        std::filesystem::temp_directory_path() /
        ("neko-browser-test-" + std::to_string(CurrentProcessId()) + "-" + std::to_string(++seq_));
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

// Subresource loads (<img>/<video>) now run on the pool after the page
// publishes, so tests poll until the expected state lands.
template <typename Pred> bool WaitForSubresources(Pred pred, int timeout_ms = 5000)
{
  for (int waited = 0; waited < timeout_ms; waited += 10) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred();
}

// Same, for predicates that inspect the live Page.
//
// Per ADR 0020 the Page and its DOM are owned by the worker thread, and pool
// threads mutate them under Page's recursive DOM lock (SetElementImages and
// friends).  A test polling |tab->page| from the main thread was reading that
// state with no lock held, which ThreadSanitizer reports as a race against the
// fetch pool.  Holding the same lock for the duration of each predicate makes
// the polling well defined -- the assertion still runs on the calling thread,
// it is just serialised against the writers, exactly as the product code does.
template <typename Pred>
bool WaitForSubresourcesOn(const Tab& tab, Pred pred, int timeout_ms = 5000)
{
  const auto poll = [&] {
    if (tab.page == nullptr) {
      return pred();
    }
    const std::unique_lock<std::recursive_mutex> dom_lock = tab.page->AcquireDomLock();
    return pred();
  };
  for (int waited = 0; waited < timeout_ms; waited += 10) {
    if (poll()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return poll();
}

// Page::Find is a caller-holds-the-lock accessor (it runs under Layout()'s
// lock in production).  The pool attaches images concurrently with a load, so
// every direct read from a test goes through the lock, and the returned image
// is a copy: the map may rehash when the next image is attached, which would
// leave a bare pointer dangling.
image::Image FindImageCopy(renderer::Page& page, const dom::Element& element, bool* found = nullptr)
{
  const std::unique_lock<std::recursive_mutex> dom_lock = page.AcquireDomLock();
  const image::Image* image = page.Find(element);
  if (found != nullptr) {
    *found = image != nullptr;
  }
  return image != nullptr ? *image : image::Image{};
}

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
    std::lock_guard<std::mutex> lock(mutex_);
    routes_[std::move(url)] = std::move(route);
  }

  // Locked snapshots of the recorded traffic.
  //
  // These used to be public data members read directly by the assertions.  The
  // writer side holds |mutex_|, but the readers did not, so a pool thread
  // pushing a request raced the test's own polling loop -- which is exactly what
  // ThreadSanitizer reported on every subresource-fetching test.  Reading
  // through these accessors makes the whole member set consistently guarded.
  std::vector<std::string> Requests() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

  std::vector<std::string> CookiesSeen() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return cookies_seen_;
  }

  std::size_t RequestCount() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_.size();
  }

  // Returns the Cookie header seen with the request for |url|, or the last one
  // recorded when |url| is empty.
  std::string LastCookieFor(std::string_view url = {}) const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (url.empty()) {
      return cookies_seen_.empty() ? std::string() : cookies_seen_.back();
    }
    for (std::size_t index = 0; index < requests_.size(); ++index) {
      if (requests_[index] == url) {
        return index < cookies_seen_.size() ? cookies_seen_[index] : std::string();
      }
    }
    return {};
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
      // The real fetch decodes data: URLs locally before it opens a socket
      // (see network::DecodeDataUrl), so the fake one mirrors that instead of
      // demanding a route.
      if (url.scheme() == "data") {
        return network::DecodeDataUrl(key);
      }
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

private:
  // Guarded by |mutex_|; read them through Requests()/CookiesSeen()/etc.
  std::vector<std::string> requests_;
  std::vector<std::string> cookies_seen_;
  std::map<std::string, Route> routes_;

  mutable std::mutex mutex_;
};

#ifndef _WIN32
class RedirectCookieServer
{
public:
  RedirectCookieServer()
  {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return;
    }
    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listen_fd_, 2) != 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    socklen_t length = sizeof(address);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([this] { Run(); });
  }

  ~RedirectCookieServer()
  {
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  bool IsValid() const
  {
    return listen_fd_ >= 0;
  }

  uint16_t port() const
  {
    return port_;
  }

  std::vector<std::string> Requests() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

private:
  static void SendAll(int fd, std::string_view data)
  {
    std::size_t sent = 0;
    while (sent < data.size()) {
      const ssize_t count = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
      if (count <= 0) {
        return;
      }
      sent += static_cast<std::size_t>(count);
    }
  }

  static std::string ReadRequest(int fd)
  {
    std::string request;
    char buffer[1024];
    while (request.find("\r\n\r\n") == std::string::npos) {
      const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
      if (count <= 0) {
        break;
      }
      request.append(buffer, static_cast<std::size_t>(count));
    }
    return request;
  }

  void Run()
  {
    for (int request_number = 0; request_number < 2; ++request_number) {
      sockaddr_in client{};
      socklen_t length = sizeof(client);
      const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client), &length);
      if (fd < 0) {
        return;
      }
      const std::string request = ReadRequest(fd);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(request);
      }
      if (request_number == 0) {
        const std::string location = "http://localhost:" + std::to_string(port_) + "/target";
        SendAll(fd,
                "HTTP/1.1 302 Found\r\nLocation: " + location + "\r\nContent-Length: 0\r\n\r\n");
      } else {
        const std::string body = "<html><body>target</body></html>";
        SendAll(fd,
                "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\n\r\n" + body);
      }
      ::shutdown(fd, SHUT_RDWR);
      ::close(fd);
    }
  }

  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::thread thread_;
  mutable std::mutex mutex_;
  std::vector<std::string> requests_;
};
#endif

std::string LastCookie(const FakeFetcher& f)
{
  return f.LastCookieFor();
}

std::string CookieForRequest(const FakeFetcher& f, std::string_view url)
{
  return f.LastCookieFor(url);
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
    // A PDF xref offset is 10 digits, but %zu of a 64-bit size_t can expand to
    // 20 digits plus the NUL, so the buffer must be sized for the worst case
    // rather than for the nominal field width -- otherwise -O2 builds reject it
    // with -Wformat-truncation (and CI builds with -Werror).
    char b[32];
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
  EXPECT_EQ(fetch.RequestCount(), 1u);
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
                               "<link rel=\"stylesheet\" href=\"http://example.com/style.css\">"
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

// window.sessionStorage is tab-scoped (WHATWG HTML 7.1): values written by
// one page survive a navigation in the same tab, while a new tab starts with
// an empty store.
TEST(BrowserControllerTest, SessionStorageSurvivesNavigationAndIsPerTab)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/a",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>A</title></head><body><script>"
                               "sessionStorage.setItem('k', 'tab-value');"
                               "</script></body></html>"});
  const std::string reader = "<html><head><title>B</title></head><body><script>"
                             "document.title = sessionStorage.getItem('k') || 'missing';"
                             "</script></body></html>";
  fetch.Add("http://example.com/b",
            FakeFetcher::Route{200, {{"content-type", "text/html"}}, reader});
  fetch.Add("http://example.com/c",
            FakeFetcher::Route{200, {{"content-type", "text/html"}}, reader});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/a").has_value());
  ASSERT_TRUE(controller.NavigateActive("http://example.com/b").has_value());
  EXPECT_EQ(controller.ActiveTab()->title, "tab-value");

  // A new tab has its own session store.
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/c").has_value());
  EXPECT_EQ(controller.ActiveTab()->title, "missing");
}

// A page script calling window.scrollTo requests a scroll: the controller
// records the pending offset and bumps the GUI-visible latch.
TEST(BrowserControllerTest, ScriptScrollSetsPendingScroll)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body><script>window.scrollTo(0, 150);</script>"
                               "<p>hi</p></body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  EXPECT_FLOAT_EQ(tab->pending_scroll_y, 150.0f);
  EXPECT_EQ(tab->scroll_request_id, 1u);
  // The snapshot exposes the latch so the GUI can apply it to its scroll bar.
  const TabSnapshot snapshot = controller.SnapshotActiveTab();
  EXPECT_EQ(snapshot.scroll_request_id, 1u);
  EXPECT_FLOAT_EQ(snapshot.pending_scroll_y, 150.0f);
}

// CSSOM callbacks outlive RunPageScripts; external stylesheet href lookup must
// not retain a reference to the setup block's local href vector.
TEST(BrowserControllerTest, ExternalStylesheetHrefSurvivesScriptSetup)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><link rel=\"stylesheet\" href=\"/site.css\">"
                               "<script>document.title = document.styleSheets[0].href;</script>"
                               "</head><body>ok</body></html>"});
  fetch.Add("http://example.com/site.css",
            FakeFetcher::Route{200, {{"content-type", "text/css"}}, "body { color: red; }"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  const Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  EXPECT_EQ(tab->title, "http://example.com/site.css");
}

// Finds the first laid-out text run belonging to |target| and returns its
// top-left point (document coordinates, before scroll).
bool FindElementRunPoint(const layout::LayoutBox& box,
                         const dom::Element* target,
                         float& x,
                         float& y)
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
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
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
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
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
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
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
            FakeFetcher::Route{
                200, {{"content-type", "text/html"}}, "<html><body>results</body></html>"});
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
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
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
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
                               "<html><body><script>window._runs = 0;</script></body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->script_runtime, nullptr);
  const auto read = [&]() {
    const auto v = tab->script_runtime->Evaluate("window._runs");
    EXPECT_TRUE(v.has_value());
    return v.has_value() ? v.value().ToNumber().value_or(-1.0) : -1.0;
  };

  // The timers are scheduled here, after the load, for the same reason as in
  // FrameIsProducedAndRefreshedByDomChanges below: a page-inline
  // `setTimeout(..., 1)` races the load (which can outlast the deadline on a
  // slow or instrumented runner), so "did not run yet" would not be
  // deterministic.
  //
  // Not yet due: an hour out, so the pump provably cannot run it.
  ASSERT_TRUE(tab->script_runtime->Evaluate("setTimeout(function(){ window._runs++; }, 3600000)")
                  .has_value());
  controller.PumpScriptTimers();
  EXPECT_DOUBLE_EQ(read(), 0.0);

  // Due: a zero-delay timer is already expired when it is scheduled, so
  // exactly one pump runs it -- no sleeping, no wall-clock bet.
  ASSERT_TRUE(
      tab->script_runtime->Evaluate("setTimeout(function(){ window._runs++; }, 0)").has_value());
  controller.PumpScriptTimers();
  EXPECT_DOUBLE_EQ(read(), 1.0);
}

TEST(BrowserControllerTest, PumpScriptTimersUntilQuietRunsDeferredWork)
{
  // Regression: the headless entry points (--screenshot / --dump-dom) have no
  // event loop of their own, so they used to observe the page in its
  // "synchronous script only" state -- no setTimeout / setInterval /
  // requestAnimationFrame callback had run.  That is where most of a real
  // page's content lives (framework hydration, lazy chunks, deferred
  // rendering), so every rendering capture was of the wrong page.  This
  // drives the loop until it goes quiet.
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body>"
                               "<script>"
                               "window._a = 0; window._b = 0;"
                               // zero-delay, a short delay, and a chained timer
                               "setTimeout(function(){ window._a++; }, 0);"
                               "setTimeout(function(){ window._a++;"
                               "  setTimeout(function(){ window._b++; }, 0); }, 5);"
                               "</script>"
                               "</body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->script_runtime, nullptr);

  const auto read = [&](const char* expr) {
    const auto v = tab->script_runtime->Evaluate(expr);
    EXPECT_TRUE(v.has_value()) << expr;
    return v.has_value() ? v.value().ToNumber().value_or(-1.0) : -1.0;
  };
  // Nothing has run yet: a single PumpScriptTimers() cannot wait for a
  // deadline, and the chained timer does not exist yet.
  EXPECT_DOUBLE_EQ(read("window._a"), 0.0);
  EXPECT_DOUBLE_EQ(read("window._b"), 0.0);

  // A single call to the "until quiet" pump must run every one of them,
  // including the timer scheduled from inside another timer.
  controller.PumpScriptTimersUntilQuiet();
  EXPECT_DOUBLE_EQ(read("window._a"), 2.0);
  EXPECT_DOUBLE_EQ(read("window._b"), 1.0);
}

TEST(BrowserControllerTest, PumpScriptTimersUntilQuietIsBounded)
{
  // A page that keeps scheduling timers must not stall the caller: the loop is
  // bounded by an iteration cap, and a timer scheduled further out than the
  // quiet budget is treated as "settled" rather than waited for.
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body><script>"
                               "window._n = 0;"
                               "function spin(){ window._n++; setTimeout(spin, 0); }"
                               "setTimeout(spin, 0);"
                               // Far in the future: must not be waited for.
                               "setTimeout(function(){}, 600000);"
                               "</script></body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  const auto start = std::chrono::steady_clock::now();
  controller.PumpScriptTimersUntilQuiet();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  // The iteration cap (1000) is what stops the runaway chain; the 600 s timer
  // is what the quiet budget skips.  Neither may turn into a real wait.
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 5000)
      << "the pump must not block on a runaway timer chain";
}

// ADR 0020: the worker (not the GUI) produces the viewport frame; timers that
// mutate the DOM and hover changes must be reflected in the next snapshot.
TEST(BrowserControllerTest, FrameIsProducedAndRefreshedByDomChanges)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body style=\"margin:0\">"
                               "<a id=\"l\" href=\"/next\">link</a>"
                               // A script element so the page has a runtime, but
                               // one that schedules nothing: see below for why
                               // the mutation is driven from the test instead.
                               "<script>var _neko_unused = 1;</script>"
                               "</body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  // The worker lays the page out for the default viewport and produces a frame.
  controller.PumpScriptTimers();
  TabSnapshot snapshot = controller.SnapshotActiveTab();
  ASSERT_NE(snapshot.frame, nullptr);
  EXPECT_GT(snapshot.frame->width, 0);
  EXPECT_GT(snapshot.frame->height, 0);
  EXPECT_EQ(snapshot.frame->rgba.size(),
            static_cast<std::size_t>(snapshot.frame->width) *
                static_cast<std::size_t>(snapshot.frame->height) * 4u);
  const float initial_height = snapshot.frame_content_height;

  // A timer mutates the DOM; the next pump must rebuild the frame.
  //
  // The mutation is scheduled *here*, from the test, rather than by an inline
  // `setTimeout(fn, 1)` in the page.  The page's own timer races the load: if
  // loading takes longer than the timer's deadline (which it does under
  // ThreadSanitizer, and on a slow machine generally) the timer fires before the
  // first pump and "before" and "after" heights are already equal, so the
  // assertion fails without anything being wrong.  Scheduling it after the first
  // snapshot makes the sequence deterministic: `setTimeout(…, 0)` is due
  // immediately, so exactly one pump runs it.
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->script_runtime, nullptr);
  ASSERT_TRUE(tab->script_runtime
                  ->Evaluate("setTimeout(function(){"
                             "var d = document.createElement('div');"
                             "d.style.height = '2000px';"
                             "document.body.appendChild(d);"
                             "}, 0)")
                  .has_value());
  controller.PumpScriptTimers();
  snapshot = controller.SnapshotActiveTab();
  EXPECT_GT(snapshot.frame_content_height, initial_height);

  // Hovering a hyperlink is reported through the snapshot (drives the GUI
  // cursor without the GUI touching the DOM).
  controller.DispatchHover(controller.ActiveTab()->id, 5.0F, 5.0F);
  controller.PumpScriptTimers();
  snapshot = controller.SnapshotActiveTab();
  EXPECT_FALSE(snapshot.frame_hover_link.empty());
}

// Regression: navigating to a new document must produce a fresh frame, not
// keep the previous page's (a stale frame rendered kaom.net blank).
TEST(BrowserControllerTest, NavigationProducesAFreshFrame)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/a",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body style=\"margin:0\">short</body></html>"});
  fetch.Add("http://example.com/b",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body style=\"margin:0\">"
                               "<div style=\"height:1800px\">tall</div></body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/a").has_value());
  controller.PumpScriptTimers();
  const TabSnapshot a = controller.SnapshotActiveTab();
  ASSERT_NE(a.frame, nullptr);

  ASSERT_TRUE(controller.NavigateActive("http://example.com/b").has_value());
  controller.PumpScriptTimers();
  const TabSnapshot b = controller.SnapshotActiveTab();
  ASSERT_NE(b.frame, nullptr);
  EXPECT_GT(b.frame_content_height, a.frame_content_height);
  EXPECT_NE(b.frame->rgba, a.frame->rgba);
}

// Regression: hovering an element must drive the style engine's :hover so
// hover-only rules expand — e.g. a CSS dropdown like kaom.net's menu.  The
// B refactor (ADR 0020) moved hover handling off the GUI but initially forgot
// to feed it back into the style engine, so :hover never matched.
TEST(BrowserControllerTest, HoverExpandsCssDropdown)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><style>"
                               ".dropdown{position:relative;display:inline-block;}"
                               ".dropdown-content{position:absolute;display:none;}"
                               ".dropdown:hover .dropdown-content{display:block;}"
                               "</style></head><body style=\"margin:0\">"
                               "<div class=\"dropdown\"><button>MENU</button>"
                               "<div class=\"dropdown-content\"><a href=\"#\">ITEM</a></div>"
                               "</div></body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  // Lay the page out (the worker does this when producing the frame) so the
  // hit test has a layout tree to walk.
  controller.PumpScriptTimers();
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->page, nullptr);
  dom::Element* content = dom::QuerySelector(*tab->page->document(), ".dropdown-content");
  ASSERT_NE(content, nullptr);
  style::ComputedStyle computed;
  std::string tag;
  ASSERT_TRUE(tab->page->TryGetComputedStyle(content, computed, tag));
  EXPECT_EQ(computed.display, style::Display::kNone);

  // Hovering the dropdown (top-left; margin:0) expands the menu.
  controller.DispatchHover(tab->id, 5.0F, 5.0F);
  ASSERT_TRUE(tab->page->TryGetComputedStyle(content, computed, tag));
  EXPECT_EQ(computed.display, style::Display::kBlock);

  // Leaving the viewport collapses it again.
  controller.DispatchHoverClear(tab->id);
  ASSERT_TRUE(tab->page->TryGetComputedStyle(content, computed, tag));
  EXPECT_EQ(computed.display, style::Display::kNone);
}

// Regression: a click must reach an expanded dropdown item.  The hit test now
// walks positioned descendants first (they paint on top), and the item's
// hyperlink default action runs — without this the menu was visible but
// unclickable (kaom.net's header menu could not navigate).
TEST(BrowserControllerTest, ClickOnExpandedDropdownItemNavigates)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><style>"
                               ".dropdown{position:relative;display:inline-block;}"
                               ".dropdown-content{position:absolute;display:none;z-index:100;}"
                               ".dropdown:hover .dropdown-content{display:block;}"
                               ".dropdown-content a{display:block;}"
                               "</style></head><body style=\"margin:0\">"
                               "<div class=\"dropdown\"><button>MENU</button>"
                               "<div class=\"dropdown-content\">"
                               "<a href=\"/next\">ITEM-1</a>"
                               "<a href=\"/next\">ITEM-2</a>"
                               "<a href=\"/next\">ITEM-3</a>"
                               "<a href=\"/next\">ITEM-4</a>"
                               "<a href=\"/next\">ITEM-5</a>"
                               "</div></div></body></html>"});
  fetch.Add("http://example.com/next",
            FakeFetcher::Route{200, {{"content-type", "text/html"}}, "<html>next</html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  controller.PumpScriptTimers();
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->page, nullptr);
  dom::Element* menu = dom::QuerySelector(*tab->page->document(), ".dropdown-content");
  ASSERT_NE(menu, nullptr);
  // The LAST item: it lies below the trigger's own box, so reaching it means
  // the pointer has left the bar (the case that used to collapse the menu).
  dom::Element* item = nullptr;
  for (dom::Node* child : menu->ChildNodes()) {
    if (child->node_type() == dom::NodeType::kElement) {
      item = static_cast<dom::Element*>(child);
    }
  }
  ASSERT_NE(item, nullptr);

  // The hidden menu item has no box; hovering the dropdown expands it.
  EXPECT_FALSE(tab->page->ElementBoxGeometry(*item).has_value());
  controller.DispatchHover(tab->id, 5.0F, 5.0F);
  controller.PumpScriptTimers();
  const auto geometry = tab->page->ElementBoxGeometry(*item);
  ASSERT_TRUE(geometry.has_value());

  // Moving the pointer from the trigger onto a menu item that overflows below
  // the trigger's box must keep the ancestor's :hover true — otherwise the
  // menu collapses before the user can reach an item (“必须离开栏” bug).
  controller.DispatchHover(
      tab->id, geometry->x + geometry->width / 2.0F, geometry->y + geometry->height / 2.0F);
  controller.PumpScriptTimers();
  style::ComputedStyle open_style;
  std::string open_tag;
  ASSERT_TRUE(tab->page->TryGetComputedStyle(menu, open_style, open_tag));
  EXPECT_EQ(open_style.display, style::Display::kBlock);

  // Click the item: the hit test must reach the positioned menu and follow the
  // hyperlink.
  controller.DispatchPointerClick(
      tab->id, geometry->x + geometry->width / 2.0F, geometry->y + geometry->height / 2.0F);
  EXPECT_EQ(controller.SnapshotActiveTab().url, "http://example.com/next");
}

// Regression: a page script may remove the hovered element.  Producing the next
// frame must not dereference the freed node — HyperlinkTarget walks a node's
// ancestors, so a stale |hovered_element| crashed the GUI
// (neko_browser_gui SIGSEGV in HyperlinkTarget on kaom.net).
TEST(BrowserControllerTest, HoveredElementRemovedByScriptDoesNotCrash)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><script>"
                               "setTimeout(function(){"
                               "  document.body.innerHTML = '<p>replaced</p>';"
                               "}, 100);"
                               "</script></head>"
                               "<body style=\"margin:0\">"
                               "<a id=\"link\" href=\"/next\" "
                               "style=\"display:block;width:200px;height:40px\">LINK</a>"
                               "</body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  controller.PumpScriptTimers();
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);

  // Hover the link: the frame reports the hyperlink under the pointer.
  controller.DispatchHover(tab->id, 5.0F, 5.0F);
  controller.PumpScriptTimers();
  EXPECT_EQ(controller.SnapshotActiveTab().frame_hover_link, "http://example.com/next");

  // The timer replaces the body; the hovered <a> is freed.  The next frame must
  // re-resolve the hover instead of walking the freed node.
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  controller.PumpScriptTimers();
  EXPECT_TRUE(controller.SnapshotActiveTab().frame_hover_link.empty());
}

// A "file:///abs/path" URL (the form hyperlinks and bookmarks produce) loads
// the absolute path.  Regression test: the leading slash used to be stripped,
// turning it into a relative path that failed to read.
TEST(BrowserControllerTest, FileUrlLoadsAbsolutePath)
{
  TempProfile tp;
  const std::string file = tp.path() + "/file_url.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(
                  file, "<html><head><title>File URL</title></head><body>file body</body></html>")
                  .has_value());

  BrowserController controller(tp.path());
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("file://" + file).has_value());
  const TabSnapshot snapshot = controller.SnapshotActiveTab();
  EXPECT_EQ(snapshot.content_type, ContentType::kHtml);
  ASSERT_TRUE(snapshot.page != nullptr);
  EXPECT_EQ(snapshot.title, "File URL");
}

// A timer that assigns window.location must navigate the tab.  Regression
// test: the pending navigation used to be stored in a stack local of
// LoadBytes, so a timer callback wrote through a dangling pointer — the
// navigation was lost (release) or the heap was corrupted (debug).
TEST(BrowserControllerTest, TimerNavigationIsHonored)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>Start</title></head><body>"
                               "<script>setTimeout(function(){"
                               "  location.href = 'http://example.com/next';"
                               "}, 1);</script>"
                               "</body></html>"});
  fetch.Add("http://example.com/next",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>Next</title></head><body>next</body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  controller.PumpScriptTimers();

  const TabSnapshot snapshot = controller.SnapshotActiveTab();
  EXPECT_EQ(snapshot.url, "http://example.com/next");
  EXPECT_EQ(snapshot.title, "Next");
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

  EXPECT_EQ(fetch.RequestCount(), 2u); // page + script
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

// Inline <script type="module"> runs in the defer phase: after every classic
// script, as an ES module (own scope, strict mode).
TEST(BrowserControllerTest, InlineModuleScriptRunsAfterClassic)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>before</title></head><body>"
                               "<script>window.phase = 'classic';</script>"
                               "<script type=\"module\">"
                               "document.title = window.phase + '-module';"
                               "</script>"
                               "</body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  EXPECT_EQ(controller.ActiveTab()->title, "classic-module");
}

// External <script type="module" src> is fetched, its relative static import
// resolves against the module's own URL, and the same entry URL evaluates
// once even when two elements reference it.
TEST(BrowserControllerTest, ExternalModuleResolvesImportAndDeduplicates)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>before</title></head><body>"
                               "<script type=\"module\" src=\"/main.js\"></script>"
                               "<script type=\"module\" src=\"/main.js\"></script>"
                               "</body></html>"});
  fetch.Add("http://example.com/main.js",
            FakeFetcher::Route{200,
                               {{"content-type", "text/javascript"}},
                               "import { tag } from './dep.js';"
                               "document.title = tag;"});
  fetch.Add("http://example.com/dep.js",
            FakeFetcher::Route{200,
                               {{"content-type", "text/javascript"}},
                               "window.dep_runs = (window.dep_runs ?? 0) + 1;"
                               "export const tag = 'module-' + window.dep_runs;"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  // page + main.js (once — dedup) + dep.js (once — module map cache).
  EXPECT_EQ(fetch.RequestCount(), 3u);
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  EXPECT_EQ(tab->title, "module-1");
  ASSERT_NE(tab->script_runtime, nullptr);
  auto runs = tab->script_runtime->Evaluate("window.dep_runs");
  ASSERT_TRUE(runs.has_value());
  auto num = runs.value().ToNumber();
  ASSERT_TRUE(num.has_value());
  EXPECT_DOUBLE_EQ(num.value(), 1.0);
}

// A module script that fails to load (404) logs an error and does not stop
// the remaining scripts.
TEST(BrowserControllerTest, ModuleFetchFailureDoesNotStopScripts)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>before</title></head><body>"
                               "<script type=\"module\" src=\"/missing.js\"></script>"
                               "<script>document.title = 'still-ran';</script>"
                               "</body></html>"});
  // No route for /missing.js -> 404 from the fake.

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  EXPECT_EQ(controller.ActiveTab()->title, "still-ran");
}

// Dynamic import() in a CLASSIC script resolves against the document URL,
// loads through the network path, and the promise continuation runs before
// the load completes (Evaluate drains the job queue).
TEST(BrowserControllerTest, ClassicScriptDynamicImport)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>before</title></head><body>"
                               "<script>"
                               "import('./lib.js').then(m => { document.title = m.tag; });"
                               "</script>"
                               "</body></html>"});
  fetch.Add("http://example.com/lib.js",
            FakeFetcher::Route{
                200, {{"content-type", "text/javascript"}}, "export const tag = 'dynamic-ok';"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  EXPECT_EQ(fetch.RequestCount(), 2u); // page + dynamically imported lib
  EXPECT_EQ(controller.ActiveTab()->title, "dynamic-ok");
}

// A module that both statically and dynamically imports the same URL fetches
// it once; the dynamic namespace exposes the same exports.
TEST(BrowserControllerTest, ModuleDynamicImportSharesCacheWithStaticImport)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>before</title></head><body>"
                               "<script type=\"module\">"
                               "import { tag } from './shared.js';"
                               "const dyn = await import('./shared.js');" // top-level await
                               "document.title = tag === dyn.tag ? 'same:' + tag : 'diff';"
                               "</script>"
                               "</body></html>"});
  fetch.Add("http://example.com/shared.js",
            FakeFetcher::Route{200,
                               {{"content-type", "text/javascript"}},
                               "window.shared_fetches = (window.shared_fetches ?? 0) + 1;"
                               "export const tag = 'shared-' + window.shared_fetches;"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  EXPECT_EQ(fetch.RequestCount(), 2u); // page + shared.js exactly once
  EXPECT_EQ(controller.ActiveTab()->title, "same:shared-1");
}

// An import map remaps a bare specifier to a vendored file; the mapped
// module loads through the normal network path.
TEST(BrowserControllerTest, ImportMapRemapsBareSpecifier)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>before</title></head><body>"
                               "<script type=\"importmap\">"
                               "{\"imports\": {\"app-state\": \"./vendor/state.js\"}}"
                               "</script>"
                               "<script type=\"module\">"
                               "import { name } from 'app-state';"
                               "document.title = name;"
                               "</script>"
                               "</body></html>"});
  fetch.Add("http://example.com/vendor/state.js",
            FakeFetcher::Route{
                200, {{"content-type", "text/javascript"}}, "export const name = 'mapped-ok';"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  EXPECT_EQ(fetch.RequestCount(), 2u); // page + mapped vendor module
  EXPECT_EQ(controller.ActiveTab()->title, "mapped-ok");
}

// Only the first <script type="importmap"> applies (spec: more than one is
// an error); later ones are ignored, and their JSON is never executed.
TEST(BrowserControllerTest, ImportMapFirstDeclarationWins)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>before</title></head><body>"
                               "<script type=\"importmap\">"
                               "{\"imports\": {\"tag\": \"./first.js\"}}"
                               "</script>"
                               "<script type=\"importmap\">"
                               "{\"imports\": {\"tag\": \"./second.js\"}}"
                               "</script>"
                               "<script type=\"module\">"
                               "import { t } from 'tag';"
                               "document.title = t;"
                               "</script>"
                               "</body></html>"});
  fetch.Add(
      "http://example.com/first.js",
      FakeFetcher::Route{200, {{"content-type", "text/javascript"}}, "export const t = 'first';"});
  fetch.Add(
      "http://example.com/second.js",
      FakeFetcher::Route{200, {{"content-type", "text/javascript"}}, "export const t = 'second';"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  // page + first.js only: the second map never applied.
  EXPECT_EQ(fetch.RequestCount(), 2u);
  EXPECT_EQ(controller.ActiveTab()->title, "first");
}

// XMLHttpRequest loads through the same network path as other subresources;
// the AMD-loader pattern (handlers before send) observes the full lifecycle.
TEST(BrowserControllerTest, XhrFetchesAndDeliversResponse)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>before</title></head><body>"
                               "<script>"
                               "const xhr = new XMLHttpRequest();"
                               "xhr.open('GET', '/api/data');"
                               "xhr.onload = function() {"
                               "  if (xhr.status === 200) { document.title = xhr.responseText; }"
                               "};"
                               "xhr.onerror = function() { document.title = 'xhr-error'; };"
                               "xhr.send();"
                               "</script>"
                               "</body></html>"});
  fetch.Add("http://example.com/api/data",
            FakeFetcher::Route{200, {{"content-type", "text/plain"}}, "payload-42"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  // send() queues the DONE task; the frame pump delivers it (the UI does this
  // on its script timer, like any real browser event loop).
  controller.PumpScriptTimers();

  EXPECT_EQ(fetch.RequestCount(), 2u); // page + XHR request
  EXPECT_EQ(controller.ActiveTab()->title, "payload-42");
}

// An HTTP error status (404) is NOT a transport error: the load handler runs
// and surfaces the status, like browsers.
TEST(BrowserControllerTest, XhrHttpErrorStatusSurfaced)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>before</title></head><body>"
                               "<script>"
                               "const xhr = new XMLHttpRequest();"
                               "xhr.open('GET', '/missing');"
                               "xhr.onload = function() {"
                               "  document.title = 'load-status-' + xhr.status;"
                               "};"
                               "xhr.onerror = function() { document.title = 'onerror'; };"
                               "xhr.send();"
                               "</script>"
                               "</body></html>"});
  fetch.Add("http://example.com/missing",
            FakeFetcher::Route{404, {{"content-type", "text/plain"}}, "not found"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  controller.PumpScriptTimers(); // deliver the queued DONE task

  EXPECT_EQ(controller.ActiveTab()->title, "load-status-404");
}

// A transport-level failure (connection refused) fires onerror with status 0.
TEST(BrowserControllerTest, XhrTransportErrorFiresOnError)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{
                200,
                {{"content-type", "text/html"}},
                "<html><head><title>before</title></head><body>"
                "<script>"
                "const xhr = new XMLHttpRequest();"
                "xhr.open('GET', '/down');"
                "xhr.onload = function() { document.title = 'load'; };"
                "xhr.onerror = function() {"
                "  document.title = 'error-state-' + xhr.readyState + '-status-' + xhr.status;"
                "};"
                "xhr.send();"
                "</script>"
                "</body></html>"});
  // No route for /down -> transport error from the fake.

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  controller.PumpScriptTimers(); // deliver the queued DONE task

  EXPECT_EQ(controller.ActiveTab()->title, "error-state-4-status-0");
}

// Loads the first system sans face: real TTF bytes FreeType accepts.
std::vector<uint8_t> LoadSystemFontBytes()
{
  for (const std::string& path : graphics::FindSystemFonts(graphics::GenericFamily::kSansSerif)) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      continue;
    }
    // rdbuf(), not istreambuf_iterator: GCC 13 reports a false-positive
    // -Wnull-dereference in libstdc++ under -O2 + -Werror (CI tsan job).
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string data = buffer.str();
    std::vector<uint8_t> bytes;
    bytes.reserve(data.size());
    for (char byte : data) {
      bytes.push_back(static_cast<uint8_t>(byte));
    }
    if (!bytes.empty()) {
      return bytes;
    }
  }
  return {};
}

// Standard base64 without line breaks; only used to build data: URLs here.
std::string TestBase64(const std::vector<uint8_t>& bytes)
{
  static constexpr const char* kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  std::size_t i = 0;
  for (; i + 3 <= bytes.size(); i += 3) {
    const uint32_t group =
        (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8) | uint32_t(bytes[i + 2]);
    out.push_back(kAlphabet[(group >> 18) & 0x3f]);
    out.push_back(kAlphabet[(group >> 12) & 0x3f]);
    out.push_back(kAlphabet[(group >> 6) & 0x3f]);
    out.push_back(kAlphabet[group & 0x3f]);
  }
  const std::size_t rest = bytes.size() - i;
  if (rest == 1) {
    const uint32_t group = uint32_t(bytes[i]) << 16;
    out.push_back(kAlphabet[(group >> 18) & 0x3f]);
    out.push_back(kAlphabet[(group >> 12) & 0x3f]);
    out.push_back('=');
    out.push_back('=');
  } else if (rest == 2) {
    const uint32_t group = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8);
    out.push_back(kAlphabet[(group >> 18) & 0x3f]);
    out.push_back(kAlphabet[(group >> 12) & 0x3f]);
    out.push_back(kAlphabet[(group >> 6) & 0x3f]);
    out.push_back('=');
  }
  return out;
}

// @font-face: the font URL is fetched and registered under its family; a
// second declaration of the same src is fetched once.  The fixture serves
// real font bytes (the first system sans face) so FreeType accepts it.
TEST(BrowserControllerTest, FontFaceFetchedAndRegistered)
{
  TempProfile tp;
  FakeFetcher fetch;
  // Real TTF bytes from the system so LoadWebFont parses them.
  const std::vector<uint8_t> font_bytes = LoadSystemFontBytes();
  ASSERT_FALSE(font_bytes.empty()) << "no system font available for the fixture";
  const std::string body(font_bytes.begin(), font_bytes.end());
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><link rel=stylesheet href=/css/site.css>"
                               "</head><body><p>x</p></body></html>"});
  fetch.Add("http://example.com/css/site.css",
            FakeFetcher::Route{200,
                               {{"content-type", "text/css"}},
                               "@font-face { font-family: 'myicon';"
                               " src: url(fonts/icon.ttf) format('truetype'); }"});
  fetch.Add("http://example.com/css/fonts/icon.ttf",
            FakeFetcher::Route{200, {{"content-type", "font/ttf"}}, body});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  ASSERT_TRUE(WaitForSubresources([&fetch] { return fetch.RequestCount() == 3u; }));
  EXPECT_THAT(fetch.Requests(), testing::Contains("http://example.com/css/fonts/icon.ttf"));
}

// A failing @font-face URL is fetched once per document, not once per
// stylesheet pass: the post-script pass re-scans the same declarations, and
// jd.com's dead font host used to be re-fetched (and re-warned) each time.
TEST(BrowserControllerTest, FailedWebFontIsFetchedOncePerDocument)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><style>"
                               "@font-face { font-family: deadfont;"
                               " src: url('/dead.woff2'); }"
                               "</style></head><body><p>x</p></body></html>"});
  // No route for /dead.woff2: that fetch fails.

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->page, nullptr);
  ASSERT_TRUE(WaitForSubresources([&fetch] { return fetch.RequestCount() >= 2u; }));

  std::size_t font_requests = 0;
  for (const std::string& url : fetch.Requests()) {
    if (url == "http://example.com/dead.woff2") {
      ++font_requests;
    }
  }
  EXPECT_EQ(font_requests, 1u);
  // Attempted (so later passes skip it) but not reported as loaded.
  EXPECT_FALSE(tab->page->ClaimWebFont("http://example.com/dead.woff2"));
  EXPECT_FALSE(tab->page->HasWebFont("http://example.com/dead.woff2"));
}

// @font-face with a data: src (the bilibili icon-font case): the bytes are
// decoded locally by the network layer per RFC 2397, registered under the
// family, and never travel over the network.
TEST(BrowserControllerTest, DataUrlFontFaceIsDecodedAndRegistered)
{
  TempProfile tp;
  FakeFetcher fetch;
  const std::vector<uint8_t> font_bytes = LoadSystemFontBytes();
  ASSERT_FALSE(font_bytes.empty()) << "no system font available for the fixture";
  // A literal space after the comma, like the fonts real pages inline.
  const std::string data_url = "data:font/ttf;base64, " + TestBase64(font_bytes);
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><link rel=stylesheet href=/css/site.css>"
                               "</head><body><p>x</p></body></html>"});
  fetch.Add("http://example.com/css/site.css",
            FakeFetcher::Route{200,
                               {{"content-type", "text/css"}},
                               "@font-face { font-family: 'myicon'; src: url(\"" + data_url +
                                   "\") format('truetype'); }"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  const std::string expected_key = "data:font/ttf;base64, " + TestBase64(font_bytes);
  // The budget is generous on purpose: registering the font means decoding it
  // with FreeType, which takes ~800 ms even without instrumentation.  Under
  // ThreadSanitizer that is ~6x slower and the default 5 s budget no longer
  // covers it, so the wait timed out on a correct result.
  ASSERT_TRUE(WaitForSubresources(
      [&controller, &expected_key] {
        Tab* tab = controller.ActiveTab();
        return tab != nullptr && tab->page != nullptr && tab->page->HasWebFont(expected_key);
      },
      /*timeout_ms=*/60000));
  // Page, stylesheet, then the font data: URL - decoded locally, never routed.
  ASSERT_EQ(fetch.RequestCount(), 3u);
  EXPECT_EQ(fetch.Requests()[2], expected_key);
}

TEST(BrowserControllerTest, WebFontDoesNotBlockPagePublication)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><style>"
                               "@font-face { font-family: slowfont; src: url('/slow.ttf'); }"
                               "</style></head><body><p>First paint</p></body></html>"});
  fetch.Add("http://example.com/slow.ttf",
            FakeFetcher::Route{200, {{"content-type", "font/ttf"}}, "not-a-font"});

  std::atomic<bool> font_requested = false;
  std::atomic<bool> release_font = false;
  BrowserController controller(tp.path(),
                               [&](const url::Url& request_url, std::string_view cookie) {
                                 if (request_url.Serialize() == "http://example.com/slow.ttf") {
                                   font_requested.store(true);
                                   while (!release_font.load()) {
                                     std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                   }
                                 }
                                 return fetch(request_url, cookie);
                               });
  struct ReleaseFontOnExit
  {
    std::atomic<bool>& release;
    ~ReleaseFontOnExit()
    {
      release.store(true);
    }
  } release_on_exit{release_font};
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->page, nullptr);
  ASSERT_TRUE(WaitForSubresources([&font_requested] { return font_requested.load(); }));

  release_font.store(true);
  ASSERT_TRUE(WaitForSubresources([&fetch] { return fetch.RequestCount() == 2u; }));
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
  std::ifstream clip(std::string(NEKO_TEST_PAGES_DIR) + "/sample_8x6_h264.mp4", std::ios::binary);
  ASSERT_TRUE(clip.good());
  // rdbuf(), not istreambuf_iterator: GCC 13 reports a false-positive
  // -Wnull-dereference in libstdc++ under -O2 + -Werror (CI tsan job).
  std::ostringstream buffer;
  buffer << clip.rdbuf();
  const std::string bytes = buffer.str();
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

  dom::Element* video = dom::QuerySelector(*tab->page->document(), "#v");
  ASSERT_NE(video, nullptr);
  // The decoded first frame is attached asynchronously after publish.
  ASSERT_TRUE(WaitForSubresourcesOn(
      *tab, [&tab, video] { return tab->page != nullptr && tab->page->Find(*video) != nullptr; }));
  bool found_frame = false;
  const image::Image frame = FindImageCopy(*tab->page, *video, &found_frame);
  ASSERT_TRUE(found_frame);
  EXPECT_EQ(frame.width, 8);
  EXPECT_EQ(frame.height, 6);

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
  auto playing = tab->script_runtime->Evaluate("document.getElementById('v').paused === false");
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
  EXPECT_EQ(fetch.RequestCount(), 2u);
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

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  // All three image requests go out and every <img> gets a decoded 2x2 image
  // (asynchronously after publish).
  const bool ready = WaitForSubresourcesOn(*tab, [&fetch, &tab] {
    if (fetch.RequestCount() < 4u || tab->page == nullptr) {
      return false;
    }
    for (const dom::Element* el : dom::QuerySelectorAll(*tab->page->document(), "img")) {
      if (tab->page->Find(*el) == nullptr) {
        return false;
      }
    }
    return true;
  });
  ASSERT_TRUE(ready);
  EXPECT_EQ(fetch.RequestCount(), 4u); // page + 3 images
  const std::vector<dom::Element*> imgs = dom::QuerySelectorAll(*tab->page->document(), "img");
  ASSERT_EQ(imgs.size(), 3u);
  for (const dom::Element* element : imgs) {
    bool found_image = false;
    const image::Image decoded = FindImageCopy(*tab->page, *element, &found_image);
    ASSERT_TRUE(found_image);
    EXPECT_EQ(decoded.width, 2);
    EXPECT_EQ(decoded.height, 2);
  }
}

TEST(BrowserControllerTest, ReusesOneFetchForDuplicatePageImageUrls)
{
  TempProfile tp;
  FakeFetcher fetch;
  const std::string png = MakePng();
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body>"
                               "<img src=\"/shared.png\"><img src=\"/shared.png\">"
                               "<div style=\"background-image:url(/shared.png)\"></div>"
                               "</body></html>"});
  fetch.Add("http://example.com/shared.png",
            FakeFetcher::Route{200, {{"content-type", "image/png"}}, png});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  const bool ready = WaitForSubresourcesOn(*tab, [&fetch, &tab] {
    const auto images = dom::QuerySelectorAll(*tab->page->document(), "img");
    return images.size() == 2 && tab->page->Find(*images[0]) != nullptr &&
           tab->page->Find(*images[1]) != nullptr && fetch.RequestCount() >= 2u;
  });
  ASSERT_TRUE(ready);

  // One snapshot, not two temporaries: Requests() returns by value, so
  // Requests().begin() and Requests().end() would come from different vectors.
  const auto shared_requests = fetch.Requests();
  EXPECT_EQ(
      std::count(shared_requests.begin(), shared_requests.end(), "http://example.com/shared.png"),
      1);
}

// Lazy loading: a timer callback assigns img.src *after* the initial
// subresource pass; the worker schedules a claimed pass for the new source and
// the element's load event fires once the image is attached (bilibili's feed
// depends on both).
TEST(BrowserControllerTest, FetchesImageAssignedAfterTheInitialPass)
{
  TempProfile tp;
  FakeFetcher fetch;
  const std::string png = MakePng();
  fetch.Add("http://example.com/",
            FakeFetcher::Route{
                200,
                {{"content-type", "text/html"}},
                "<html><body><img id=\"late\">"
                "<script>"
                "var img = document.getElementById('late');"
                "img.onload = function(){ document.body.setAttribute('data-late', 'loaded'); };"
                "setTimeout(function(){ img.src = '/late.png'; }, 0);"
                "</script></body></html>"});
  fetch.Add("http://example.com/late.png",
            FakeFetcher::Route{200, {{"content-type", "image/png"}}, png});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->page, nullptr);

  // Drive the timer; the pump schedules the late fetch pass (the GUI does the
  // same from its 50ms timer).
  controller.PumpScriptTimersUntilQuiet();
  bool ready = false;
  for (int i = 0; i < 400 && !ready; ++i) {
    // The pass runs on the pool; pumps deliver the image events it queues.
    controller.PumpScriptTimers();
    dom::Element* body = dom::QuerySelector(*tab->page->document(), "body");
    const std::vector<dom::Element*> images = dom::QuerySelectorAll(*tab->page->document(), "img");
    // Page::Find is a caller-holds-the-lock accessor (it runs under Layout()'s
    // lock); a pool thread may be attaching images concurrently, so the test
    // must take the DOM lock like the controller does.
    bool attached = false;
    {
      const std::unique_lock<std::recursive_mutex> dom_lock = tab->page->AcquireDomLock();
      attached = !images.empty() && tab->page->Find(*images[0]) != nullptr;
    }
    ready = body != nullptr && attached && body->GetAttribute("data-late").value_or("") == "loaded";
    if (!ready) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  EXPECT_TRUE(ready) << "late-assigned image never fetched/attached/load-dispatched";
  EXPECT_GE(fetch.RequestCount(), 2u); // document + late.png
}

// The GUI lazy-loading loop end to end: an IntersectionObserver observes an
// element below the fold; the user scrolls (SetTabScrollOffset, exactly what
// WebView's scroll bar reports), the controller refreshes the observers, the
// page's callback assigns img.src, the late pass fetches it, and the produced
// frame actually contains the fetched pixels.  bilibili's feed hangs its image
// mounting off this exact chain, and the pixels (not just the DOM state) are
// what the user sees -- a fetch that completes without marking the tab dirty
// leaves the placeholder gray on screen.
TEST(BrowserControllerTest, ScrollingTriggersObserverDrivenImageFetch)
{
  TempProfile tp;
  FakeFetcher fetch;
  const std::string png = MakePng();
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body>"
                               "<div style=\"height:600px\"></div>"
                               "<img id=\"late\" data-src=\"/late.png\""
                               "     style=\"width:10px;height:10px\">"
                               "<script>"
                               // The callback only assigns src: it deliberately mutates
                               // nothing else, so the repaint must come from the image
                               // pipeline itself, not from a coincidental DOM change.
                               "var obs = new IntersectionObserver(function(entries){"
                               "  if (entries[entries.length-1].isIntersecting) {"
                               "    var img = document.getElementById('late');"
                               "    img.src = img.getAttribute('data-src');"
                               "  }"
                               "});"
                               "obs.observe(document.getElementById('late'));"
                               "</script></body></html>"});
  fetch.Add("http://example.com/late.png",
            FakeFetcher::Route{200, {{"content-type", "image/png"}}, png});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->page, nullptr);

  // Before scrolling the sprite is below the viewport: no observer callback
  // ran, so the image was never assigned (and never requested).
  controller.PumpScriptTimers();
  {
    dom::Element* img = dom::QuerySelector(*tab->page->document(), "#late");
    ASSERT_NE(img, nullptr);
    EXPECT_FALSE(img->GetAttribute("src").has_value());
  }

  // Scroll it into view — the browser half of the GUI's scroll bar handler.
  controller.SetTabScrollOffset(tab->id, 400.0F);
  controller.PumpScriptTimersUntilQuiet();

  bool attached = false;
  for (int i = 0; i < 400 && !attached; ++i) {
    controller.PumpScriptTimers();
    dom::Element* img = dom::QuerySelector(*tab->page->document(), "#late");
    // Page::Find is a caller-holds-the-lock accessor; see the note in the
    // late-assigned-image test above.
    {
      const std::unique_lock<std::recursive_mutex> dom_lock = tab->page->AcquireDomLock();
      attached = img != nullptr && tab->page->Find(*img) != nullptr;
    }
    if (!attached) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  ASSERT_TRUE(attached) << "observer-driven lazy image never reached the page";

  // The next pump must rebuild the viewport frame and the fetched pixels must
  // be in it.  The image is an inline replaced box on the line below the
  // spacer (body margin included), so scan the band it can occupy rather than
  // hard-coding one pixel.
  controller.PumpScriptTimers();
  const TabSnapshot snapshot = controller.SnapshotActiveTab();
  ASSERT_NE(snapshot.frame, nullptr);
  ASSERT_GT(snapshot.frame->width, 40);
  ASSERT_GT(snapshot.frame->height, 260);
  bool found_image_pixels = false;
  for (int y = 190; y < 260 && !found_image_pixels; ++y) {
    for (int x = 0; x < 40; ++x) {
      const std::size_t idx =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(snapshot.frame->width) +
           static_cast<std::size_t>(x)) *
          4;
      if (snapshot.frame->rgba[idx] > 200 && snapshot.frame->rgba[idx + 1] < 60 &&
          snapshot.frame->rgba[idx + 2] < 60) {
        found_image_pixels = true;
        break;
      }
    }
  }
  EXPECT_TRUE(found_image_pixels) << "lazy image pixels never reached the frame";
  EXPECT_GE(fetch.RequestCount(), 2u); // document + late.png
}

// A changed viewport scroll offset fires the document-level "scroll" event
// (WHATWG HTML: the target is the Document, which is also where window-level
// listeners registered in this engine live).  Infinite-scroll feeds load
// their next page from that listener; without the event the page never
// grows and the user cannot scroll further.
TEST(BrowserControllerTest, ScrollEventFiresWhenTheOffsetChanges)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body style=\"height:3000px\">"
                               "<div id=\"marker\">init</div>"
                               "<script>"
                               "window.addEventListener('scroll', function(){"
                               "  document.getElementById('marker')"
                               "    .setAttribute('data-scroll', String(window.scrollY));"
                               "});"
                               "document.addEventListener('scroll', function(){"
                               "  document.getElementById('marker')"
                               "    .setAttribute('data-doc-scroll', String(window.scrollY));"
                               "});"
                               "</script></body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->page, nullptr);
  controller.PumpScriptTimers();

  {
    dom::Element* marker = dom::QuerySelector(*tab->page->document(), "#marker");
    ASSERT_NE(marker, nullptr);
    EXPECT_FALSE(marker->GetAttribute("data-scroll").has_value());
  }

  // The browser half of the GUI's scrollbar handler.
  controller.SetTabScrollOffset(tab->id, 250.0F);
  {
    dom::Element* marker = dom::QuerySelector(*tab->page->document(), "#marker");
    ASSERT_NE(marker, nullptr);
    const std::optional<std::string_view> value = marker->GetAttribute("data-scroll");
    ASSERT_TRUE(value.has_value()) << "scroll listener did not run";
    EXPECT_EQ(*value, "250");
    // A listener registered on the document (the event target) runs too.
    const std::optional<std::string_view> doc_value = marker->GetAttribute("data-doc-scroll");
    ASSERT_TRUE(doc_value.has_value()) << "document scroll listener did not run";
    EXPECT_EQ(*doc_value, "250");
  }

  // Re-applying the same offset is not a scroll: no second event.
  dom::Element* marker = dom::QuerySelector(*tab->page->document(), "#marker");
  ASSERT_NE(marker, nullptr);
  marker->SetAttribute("data-scroll", "reset");
  marker->SetAttribute("data-doc-scroll", "reset");
  controller.SetTabScrollOffset(tab->id, 250.0F);
  EXPECT_EQ(marker->GetAttribute("data-scroll").value_or(""), "reset")
      << "an unchanged offset must not fire scroll";
  //...but a different offset does.
  controller.SetTabScrollOffset(tab->id, 400.0F);
  EXPECT_EQ(marker->GetAttribute("data-scroll").value_or(""), "400");
}

// Timer batches only re-run the cascade when the DOM actually changed in a
// style-affecting way.  A pure rAF/poll tick used to trigger a full restyle +
// relayout on every 50 ms pump (measured >600 ms per pass on bilibili), and a
// batch assigning img.src must feed the late-fetch pass without a restyle.
TEST(BrowserControllerTest, TimerBatchesOnlyRestyleWhenTheCascadeChanged)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body style=\"margin:0\">"
                               "<div id=\"box\" style=\"width:10px;height:10px\"></div>"
                               "<script>var _unused = 1;</script>"
                               "</body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->page, nullptr);
  controller.PumpScriptTimers();
  dom::Element* box = dom::QuerySelector(*tab->page->document(), "#box");
  ASSERT_NE(box, nullptr);
  const std::uint64_t before = tab->page->layout_version();

  // A pure-JS timer tick: nothing changed, so no restyle/relayout may run.
  ASSERT_TRUE(
      tab->script_runtime->Evaluate("setTimeout(function(){ window._tick = 1; }, 0)").has_value());
  controller.PumpScriptTimers();
  EXPECT_EQ(tab->page->layout_version(), before);

  // A timer that changes styles must restyle and relayout.  This exercises
  // the per-property setter path (style.width), which used to not even mark
  // the DOM dirty.
  ASSERT_TRUE(tab->script_runtime
                  ->Evaluate("setTimeout(function(){"
                             "document.getElementById('box').style.width = '50px';"
                             "}, 0)")
                  .has_value());
  controller.PumpScriptTimers();
  EXPECT_NE(tab->page->layout_version(), before);
  const auto geometry = tab->page->ElementBoxGeometry(*box);
  ASSERT_TRUE(geometry.has_value());
  EXPECT_NEAR(geometry->width, 50.0F, 1.0F);
}

// <script type="application/json"> is a data block (HTML §4.12.1): never
// executed as code, and it does not stop the executable scripts around it.
TEST(BrowserControllerTest, NonJsScriptTypesAreNotExecuted)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>before</title></head><body>"
                               "<script type=\"application/json\">"
                               "{\"key\": \"value\", \"list\": [1, 2, 3]}"
                               "</script>"
                               "<script>document.title = 'classic-ran';</script>"
                               "<SCRIPT TYPE=\"TEXT/JSON\">{broken as js}</SCRIPT>"
                               "</body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  // The classic script ran; the JSON islands produced no errors and no
  // navigation away from the page.
  EXPECT_EQ(controller.ActiveTab()->title, "classic-ran");
}

// new Image([width[, height]]) creates a detached <img> element that script
// can insert into the document.
TEST(BrowserControllerTest, ImageConstructorCreatesImgElement)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><title>t</title></head><body>"
                               "<script>"
                               "const img = new Image(100, 50);"
                               "img.src = '/x.png';"
                               "document.body.appendChild(img);"
                               "window.tag = img.tagName;"
                               "</script>"
                               "</body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->script_runtime, nullptr);
  auto tag = tab->script_runtime->Evaluate("window.tag");
  ASSERT_TRUE(tag.has_value());
  auto ts = tag.value().ToString();
  ASSERT_TRUE(ts.has_value());
  EXPECT_EQ(ts.value(), "IMG");
  const std::vector<dom::Element*> imgs = dom::QuerySelectorAll(*tab->page->document(), "img");
  ASSERT_EQ(imgs.size(), 1u);
  const dom::Element* img = imgs[0];
  std::optional<std::string_view> width = img->GetAttribute("width");
  ASSERT_TRUE(width.has_value());
  EXPECT_EQ(width.value(), "100");
  std::optional<std::string_view> height = img->GetAttribute("height");
  ASSERT_TRUE(height.has_value());
  EXPECT_EQ(height.value(), "50");
}

// <img src="data:image/png;base64,..."> decodes without a network request.
TEST(BrowserControllerTest, DataUrlImageIsDecodedWithoutNetwork)
{
  TempProfile tp;
  FakeFetcher fetch;
  // Base64 of MakePng()'s bytes (computed in-test to stay in sync).
  const std::string png = MakePng();
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string b64;
  b64.reserve((png.size() + 2) / 3 * 4);
  for (std::size_t i = 0; i < png.size(); i += 3) {
    const unsigned n =
        static_cast<unsigned>(static_cast<unsigned char>(png[i])) << 16 |
        static_cast<unsigned>(i + 1 < png.size() ? static_cast<unsigned char>(png[i + 1]) : 0)
            << 8 |
        (i + 2 < png.size() ? static_cast<unsigned char>(png[i + 2]) : 0);
    b64 += kAlphabet[(n >> 18) & 63];
    b64 += kAlphabet[(n >> 12) & 63];
    b64 += i + 1 < png.size() ? kAlphabet[(n >> 6) & 63] : '=';
    b64 += i + 2 < png.size() ? kAlphabet[n & 63] : '=';
  }
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body><img src=\"data:image/png;base64," + b64 +
                                   "\"></body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  // Only the page itself was fetched — the data URL needed no request.
  EXPECT_LE(fetch.RequestCount(), 1u);
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  const bool decoded_ok = WaitForSubresourcesOn(*tab, [&tab] {
    if (tab->page == nullptr) {
      return false;
    }
    const std::vector<dom::Element*> elements =
        dom::QuerySelectorAll(*tab->page->document(), "img");
    return elements.size() == 1u && tab->page->Find(*elements[0]) != nullptr;
  });
  ASSERT_TRUE(decoded_ok);
  const std::vector<dom::Element*> imgs = dom::QuerySelectorAll(*tab->page->document(), "img");
  ASSERT_EQ(imgs.size(), 1u);
  bool found_image = false;
  const image::Image decoded = FindImageCopy(*tab->page, *imgs[0], &found_image);
  ASSERT_TRUE(found_image);
  EXPECT_EQ(decoded.width, 2);
  EXPECT_EQ(decoded.height, 2);
}

// <link rel="stylesheet" href="data:text/css,..."> is applied: external
// stylesheets accept data: URLs, decoded locally by the network layer.
TEST(BrowserControllerTest, DataUrlStylesheetIsApplied)
{
  TempProfile tp;
  // The default fetch (the network stack) is used: a data: URL needs no
  // network, so this test stays hermetic.
  BrowserController controller(tp.path());
  const int tab = controller.NewTab();
  const std::string html =
      "<html><head><link rel=\"stylesheet\" href=\"data:text/css,%23box%7Bwidth%3A123px%3B"
      "height%3A45px%7D\"></head><body><div id=\"box\"></div></body></html>";
  const auto loaded =
      controller.LoadDocument(tab, html, "text/html", "https://example.test/stylesheet.html");
  ASSERT_TRUE(loaded.has_value()) << loaded.error().message();
  Tab* tab_ptr = controller.FindTab(tab);
  ASSERT_NE(tab_ptr, nullptr);
  ASSERT_NE(tab_ptr->page, nullptr);
  tab_ptr->page->Layout(800, 600);
  dom::Element* box = dom::QuerySelector(*tab_ptr->page->document(), "#box");
  ASSERT_NE(box, nullptr);
  const auto geometry = tab_ptr->page->ElementBoxGeometry(*box);
  ASSERT_TRUE(geometry.has_value());
  EXPECT_NEAR(geometry->width, 123.0f, 1.0f);
  EXPECT_NEAR(geometry->height, 45.0f, 1.0f);
}

// Page zoom (Ctrl+=/Ctrl+-/Ctrl+0) in the in-process path: the factor scales
// the layout the controller publishes, survives navigation, and the keyboard
// steps follow the browser ladder.
// Find-in-page (Ctrl+F) in the in-process path: the query produces a match
// list, stepping wraps around it, the page is asked to scroll to the current
// match through the GUI latch, and navigation clears the session.
// The GUI's viewport report drives the in-process page: the layout is redone at
// the new width, window.innerWidth follows it and the page's scripts get the
// `resize` event (browsers do the same on a window resize).  A repeated report
// of the same size is a no-op.
TEST(BrowserControllerTest, WindowResizeRelayoutsAndFiresResizeEvent)
{
  TempProfile tp;
  FakeFetcher fetch;
  const std::string html = "<html><body><script>"
                           "window.__resizes = 0;"
                           "window.addEventListener('resize', function () { window.__resizes++; });"
                           "</script><p id=text>resize probe</p></body></html>";
  fetch.Add("http://viewport.test/",
            FakeFetcher::Route{200, {{"content-type", "text/html"}}, html});
  BrowserController controller(tp.path(), std::ref(fetch));
  const int tab = controller.NewTab();
  ASSERT_TRUE(controller.Navigate(tab, "http://viewport.test/").has_value());
  Tab* tab_ptr = controller.FindTab(tab);
  ASSERT_NE(tab_ptr, nullptr);
  ASSERT_NE(tab_ptr->script_runtime, nullptr);
  ASSERT_NE(tab_ptr->page, nullptr);

  // Before the GUI reports anything, the page reports its own layout viewport.
  const auto number = [&tab_ptr](const std::string& code) {
    auto result = tab_ptr->script_runtime->Evaluate(code);
    EXPECT_TRUE(result.has_value()) << code;
    if (!result.has_value()) {
      return -1.0;
    }
    auto value = result.value().ToNumber();
    return value.has_value() ? value.value() : -1.0;
  };
  EXPECT_EQ(number("window.innerWidth"), static_cast<double>(tab_ptr->page->viewport_css_width()));
  EXPECT_EQ(number("window.__resizes"), 0.0);

  controller.SetTabViewport(tab, 1024, 700);
  EXPECT_FLOAT_EQ(tab_ptr->page->viewport_css_width(), 1024.0F);
  EXPECT_EQ(number("window.innerWidth"), 1024.0);
  EXPECT_EQ(number("window.innerHeight"), 700.0);
  EXPECT_EQ(number("window.__resizes"), 1.0);

  // Re-reporting the same size does not re-fire the event.
  controller.SetTabViewport(tab, 1024, 700);
  EXPECT_EQ(number("window.__resizes"), 1.0);

  // A real change fires again.
  controller.SetTabViewport(tab, 640, 480);
  EXPECT_EQ(number("window.__resizes"), 2.0);
  EXPECT_EQ(number("window.innerWidth"), 640.0);
  EXPECT_FLOAT_EQ(tab_ptr->page->viewport_css_width(), 640.0F);
}

TEST(BrowserControllerTest, FindInTabMatchesStepsAndScrolls)
{
  TempProfile tp;
  FakeFetcher fetch;
  const std::string html = "<html><body><p>alpha beta alpha</p><p>gamma</p></body></html>";
  fetch.Add("http://find.test/one", FakeFetcher::Route{200, {{"content-type", "text/html"}}, html});
  fetch.Add("http://find.test/two", FakeFetcher::Route{200, {{"content-type", "text/html"}}, html});
  BrowserController controller(tp.path(), std::ref(fetch));
  const int tab = controller.NewTab();
  ASSERT_TRUE(controller.Navigate(tab, "http://find.test/one").has_value());
  Tab* tab_ptr = controller.FindTab(tab);
  ASSERT_NE(tab_ptr, nullptr);
  ASSERT_NE(tab_ptr->page, nullptr);
  tab_ptr->page->Layout(800, 600);

  // Two occurrences of "alpha"; the first is current and the page is asked to
  // scroll to it (the latch the GUI consumes).
  TabSnapshot before = controller.SnapshotTab(tab);
  EXPECT_TRUE(before.find_query.empty());
  EXPECT_EQ(controller.FindInTab(tab, "alpha"), 2);
  TabSnapshot found = controller.SnapshotTab(tab);
  EXPECT_EQ(found.find_query, "alpha");
  EXPECT_EQ(found.find_match_count, 2);
  EXPECT_EQ(found.find_current_index, 0);
  EXPECT_GT(found.find_current_match.width, 0.0F);
  EXPECT_GT(found.scroll_request_id, before.scroll_request_id);

  // Stepping walks the list and wraps around in both directions.
  EXPECT_EQ(controller.FindInTab(tab, "alpha", 1), 2);
  EXPECT_EQ(controller.SnapshotTab(tab).find_current_index, 1);
  EXPECT_EQ(controller.FindInTab(tab, "alpha", 1), 2);
  EXPECT_EQ(controller.SnapshotTab(tab).find_current_index, 0);
  EXPECT_EQ(controller.FindInTab(tab, "alpha", -1), 2);
  EXPECT_EQ(controller.SnapshotTab(tab).find_current_index, 1);

  // The rectangles belong to different occurrences.
  const auto second = controller.SnapshotTab(tab).find_current_match;
  EXPECT_EQ(controller.FindInTab(tab, "g", 0), 1);
  const auto gamma = controller.SnapshotTab(tab).find_current_match;
  EXPECT_NE(gamma.y, second.y);

  // No matches and the empty query both clear the state.
  EXPECT_EQ(controller.FindInTab(tab, "absent"), 0);
  TabSnapshot missing = controller.SnapshotTab(tab);
  EXPECT_EQ(missing.find_query, "absent");
  EXPECT_EQ(missing.find_match_count, 0);
  EXPECT_EQ(missing.find_current_index, -1);
  EXPECT_EQ(controller.FindInTab(tab, "alpha"), 2);
  EXPECT_EQ(controller.FindInTab(tab, ""), 0);
  EXPECT_TRUE(controller.SnapshotTab(tab).find_query.empty());

  // A navigation describes a new document, so the session is dropped.
  EXPECT_EQ(controller.FindInTab(tab, "alpha"), 2);
  ASSERT_TRUE(controller.Navigate(tab, "http://find.test/two").has_value());
  TabSnapshot reloaded = controller.SnapshotTab(tab);
  EXPECT_TRUE(reloaded.find_query.empty());
  EXPECT_EQ(reloaded.find_match_count, 0);
}

TEST(BrowserControllerTest, NextZoomFactorStepsTheBrowserLadder)
{
  EXPECT_FLOAT_EQ(NextZoomFactor(1.0F, 1), 1.1F);
  EXPECT_FLOAT_EQ(NextZoomFactor(1.1F, 1), 1.25F);
  EXPECT_FLOAT_EQ(NextZoomFactor(1.0F, -1), 0.9F);
  EXPECT_FLOAT_EQ(NextZoomFactor(0.9F, -1), 0.8F);
  // Between two steps: snap to the neighbour in the requested direction.
  EXPECT_FLOAT_EQ(NextZoomFactor(1.3F, 1), 1.5F);
  EXPECT_FLOAT_EQ(NextZoomFactor(1.3F, -1), 1.25F);
  // The ends stay put (SetTabZoom clamps the same bounds).
  EXPECT_FLOAT_EQ(NextZoomFactor(renderer::kMinUserZoom, -1), renderer::kMinUserZoom);
  EXPECT_FLOAT_EQ(NextZoomFactor(renderer::kMaxUserZoom, 1), renderer::kMaxUserZoom);
  EXPECT_FLOAT_EQ(NextZoomFactor(1.5F, 0), 1.5F);
}

TEST(BrowserControllerTest, ZoomScalesInProcessLayoutAndSurvivesNavigation)
{
  TempProfile tp;
  FakeFetcher fetch;
  const std::string html =
      "<html><body style=\"margin:0\"><div id=box "
      "style=\"background-color:#ff0000;width:100px;height:50px\"></div></body></html>";
  fetch.Add("http://zoom.test/one", FakeFetcher::Route{200, {{"content-type", "text/html"}}, html});
  fetch.Add("http://zoom.test/two", FakeFetcher::Route{200, {{"content-type", "text/html"}}, html});
  BrowserController controller(tp.path(), std::ref(fetch));
  const int tab = controller.NewTab();
  ASSERT_TRUE(controller.Navigate(tab, "http://zoom.test/one").has_value());
  Tab* tab_ptr = controller.FindTab(tab);
  ASSERT_NE(tab_ptr, nullptr);
  ASSERT_NE(tab_ptr->page, nullptr);
  tab_ptr->page->Layout(800, 600);
  dom::Element* box = dom::QuerySelector(*tab_ptr->page->document(), "#box");
  ASSERT_NE(box, nullptr);
  const auto at_100 = tab_ptr->page->ElementBoxGeometry(*box);
  ASSERT_TRUE(at_100.has_value());

  EXPECT_FLOAT_EQ(controller.SetTabZoom(tab, 2.0F), 2.0F);
  EXPECT_FLOAT_EQ(controller.TabZoom(tab), 2.0F);
  const auto at_200 = tab_ptr->page->ElementBoxGeometry(*box);
  ASSERT_TRUE(at_200.has_value());
  EXPECT_NEAR(at_200->width, at_100->width * 2.0F, 1.0F);
  EXPECT_NEAR(at_200->height, at_100->height * 2.0F, 1.0F);

  // Steps are relative to the current factor and the API clamps the bounds.
  EXPECT_FLOAT_EQ(controller.ZoomInTab(tab), 2.5F);
  EXPECT_FLOAT_EQ(controller.ZoomOutTab(tab), 2.0F);
  EXPECT_FLOAT_EQ(controller.SetTabZoom(tab, 100.0F), renderer::kMaxUserZoom);
  EXPECT_FLOAT_EQ(controller.SetTabZoom(tab, 0.0F), renderer::kMinUserZoom);
  EXPECT_FLOAT_EQ(controller.SetTabZoom(tab, 2.0F), 2.0F);

  // A zoomed tab keeps its zoom across navigations (browser behavior).
  ASSERT_TRUE(controller.Navigate(tab, "http://zoom.test/two").has_value());
  EXPECT_FLOAT_EQ(controller.TabZoom(tab), 2.0F);
  Tab* reloaded = controller.FindTab(tab);
  ASSERT_NE(reloaded, nullptr);
  ASSERT_NE(reloaded->page, nullptr);
  EXPECT_FLOAT_EQ(reloaded->page->user_zoom(), 2.0F);

  EXPECT_FLOAT_EQ(controller.ResetTabZoom(tab), 1.0F);
  EXPECT_FLOAT_EQ(reloaded->page->user_zoom(), 1.0F);
  // Unknown tabs report 100% instead of inventing state.
  EXPECT_FLOAT_EQ(controller.TabZoom(4242), 1.0F);
  EXPECT_FLOAT_EQ(controller.SetTabZoom(4242, 3.0F), 1.0F);
}

TEST(BrowserControllerTest, PercentEncodedSvgDataUrlIsDecodedWithoutNetwork)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{
                200,
                {{"content-type", "text/html"}},
                "<html><body><img src=\"data:image/svg+xml,%3Csvg%20xmlns%3D'http%3A%2F%2F"
                "www.w3.org%2F2000%2Fsvg'%20width%3D'3'%20height%3D'2'%3E%3Crect%20width%3D'3'%20"
                "height%3D'2'%20fill%3D'%23ff0000'%2F%3E%3C%2Fsvg%3E\"></body></html>"});

  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  EXPECT_LE(fetch.RequestCount(), 1u);
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  const bool decoded_ok = WaitForSubresourcesOn(*tab, [&tab] {
    if (tab->page == nullptr) {
      return false;
    }
    const std::vector<dom::Element*> elements =
        dom::QuerySelectorAll(*tab->page->document(), "img");
    return elements.size() == 1u && tab->page->Find(*elements[0]) != nullptr;
  });
  ASSERT_TRUE(decoded_ok);
  const std::vector<dom::Element*> imgs = dom::QuerySelectorAll(*tab->page->document(), "img");
  ASSERT_EQ(imgs.size(), 1u);
  const image::Image* decoded = tab->page->Find(*imgs[0]);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->width, 3);
  EXPECT_EQ(decoded->height, 2);
}

// A 1x1, two-frame animated GIF (red 5cs, green 10cs, looping forever),
// written out byte by byte: the browser tests must not depend on an encoder.
// The bytes are hex-escaped individually because C++ hex escapes are greedy
// (a raw 0x00 byte would also terminate the literal).
std::string AnimatedGifBytes()
{
  // |size| is explicit: the GIF contains NUL bytes, so the literal cannot be
  // treated as a C string.
  return std::string("\x47\x49\x46\x38\x39\x61\x01\x00\x01\x00\xf0\x00"
                     "\x00\xff\x00\x00\x00\xff\x00\x21\xff\x0b\x4e\x45"
                     "\x54\x53\x43\x41\x50\x45\x32\x2e\x30\x03\x01\x00"
                     "\x00\x00\x21\xf9\x04\x04\x05\x00\x00\x00\x2c\x00"
                     "\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02\x44\x01"
                     "\x00\x21\xf9\x04\x04\x0a\x00\x00\x00\x2c\x00\x00"
                     "\x00\x00\x01\x00\x01\x00\x00\x02\x02\x4c\x01\x00"
                     "\x3b",
                     85);
}

TEST(BrowserControllerTest, DirectGifNavigationPlaysFrames)
{
  TempProfile tp;
  FakeFetcher fetch;
  const std::string gif = AnimatedGifBytes();
  ASSERT_GT(gif.size(), 60u);
  fetch.Add("http://example.com/anim.gif",
            FakeFetcher::Route{200, {{"content-type", "image/gif"}}, gif});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/anim.gif").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  ASSERT_NE(tab->image, nullptr);
  EXPECT_EQ(tab->content_type, ContentType::kImage);
  ASSERT_NE(tab->gif_animation, nullptr);
  EXPECT_GE(tab->gif_animation->frames.size(), 2u);
  // First frame: red.
  EXPECT_EQ(tab->image->rgba[0], 255);
  EXPECT_EQ(tab->image->rgba[1], 0);

  // The frame clock drives playback, but the wall-clock instant a pump lands on
  // is not deterministic on a loaded CI machine: the 50 ms/100 ms frames loop
  // every 150 ms, so a late pump can land back on the red frame.  Poll until
  // the display reaches the green frame instead of assuming the sleep lands
  // inside its window.
  bool turned_green = false;
  for (int i = 0; i < 400 && !turned_green; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    controller.PumpScriptTimers();
    turned_green = tab->image != nullptr && tab->image->rgba[1] == 255;
  }
  ASSERT_TRUE(turned_green);
  ASSERT_NE(tab->image, nullptr);
  EXPECT_EQ(tab->image->rgba[0], 0);
  EXPECT_EQ(tab->image->rgba[1], 255);
  EXPECT_GT(tab->image_frame, 0u);
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
  // fetched alongside the page.  One snapshot: Requests() returns by value, so
  // separate calls would yield iterators into different vectors.
  const auto sheet_requests = fetch.Requests();
  EXPECT_NE(std::find(sheet_requests.begin(), sheet_requests.end(), "http://example.com/style.css"),
            sheet_requests.end());
  EXPECT_EQ(
      std::count(sheet_requests.begin(), sheet_requests.end(), "http://example.com/style.css"), 1);
  EXPECT_EQ(
      std::count(sheet_requests.begin(), sheet_requests.end(), "http://example.com/favicon.ico"),
      0);

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

TEST(BrowserControllerTest, SendsCookiesToAuthenticatedSubresources)
{
  TempProfile tp;
  FakeFetcher fetch;
  const std::string page_url = "http://example.com/";
  const std::string css_url = "http://example.com/style.css";
  const std::string image_url = "http://example.com/image.png";
  fetch.Add(page_url,
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><head><link rel=\"stylesheet\" href=\"/style.css\"></head>"
                               "<body><img src=\"/image.png\"></body></html>"});
  fetch.Add(css_url, FakeFetcher::Route{200, {{"content-type", "text/css"}}, "body {}"});
  fetch.Add(image_url, FakeFetcher::Route{200, {{"content-type", "image/png"}}, MakePng()});

  BrowserController controller(tp.path(), std::ref(fetch));
  const auto origin = url::Url::Parse(page_url);
  ASSERT_TRUE(origin.has_value());
  ASSERT_TRUE(controller.cookies().SetCookieFromHeader(origin.value(), "session=abc; Path=/", 1));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive(page_url).has_value());

  EXPECT_NE(CookieForRequest(fetch, page_url).find("session=abc"), std::string::npos);
  // The stylesheet loads synchronously pre-publish; the image asynchronously
  // after — wait until its request (with cookie) has been recorded.
  ASSERT_TRUE(WaitForSubresources(
      [&] { return CookieForRequest(fetch, image_url).find("session=abc") != std::string::npos; }));
  EXPECT_NE(CookieForRequest(fetch, css_url).find("session=abc"), std::string::npos);
  EXPECT_NE(CookieForRequest(fetch, image_url).find("session=abc"), std::string::npos);
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
  fetch.Add(
      "http://example.com/a.css",
      FakeFetcher::Route{200, {{"content-type", "text/css"}}, "#t { color: rgb(255, 0, 0); }"});
  fetch.Add(
      "http://example.com/b.css",
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
  EXPECT_EQ(fetch.CookiesSeen()[0], "");
  // Second navigation sends both cookies.
  ASSERT_TRUE(controller.NavigateActive("http://example.com/dashboard").has_value());
  const std::string cookie = LastCookie(fetch);
  EXPECT_NE(cookie.find("session=abc"), std::string::npos);
  EXPECT_NE(cookie.find("theme=dark"), std::string::npos);
}

TEST(BrowserControllerTest, PageScriptCannotCreateHttpOnlyCookie)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<script>document.cookie='visible=yes; Path=/';"
                               "document.cookie='secret=no; HttpOnly; Path=/';</script>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());

  const auto cookies = controller.cookies().All();
  ASSERT_EQ(cookies.size(), 1u);
  EXPECT_EQ(cookies[0].name, "visible");
  EXPECT_FALSE(cookies[0].http_only);
}

#ifndef _WIN32
TEST(BrowserControllerTest, DoesNotSendSourceCookieToRedirectTarget)
{
  TempProfile tp;
  RedirectCookieServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string source = "http://127.0.0.1:" + std::to_string(server.port()) + "/start";
  const auto source_url = url::Url::Parse(source);
  ASSERT_TRUE(source_url.has_value());

  BrowserController controller(tp.path());
  ASSERT_TRUE(
      controller.cookies().SetCookieFromHeader(source_url.value(), "session=secret; Path=/", 1));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive(source).has_value());

  const auto requests = server.Requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_NE(requests[0].find("cookie: session=secret"), std::string::npos);
  EXPECT_EQ(requests[1].find("cookie:"), std::string::npos);
}
#endif

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

TEST(BrowserControllerTest, ResolveInputSearchesUnknownInput)
{
  TempProfile tp;
  FakeFetcher fetch;
  BrowserController controller(tp.path(), std::ref(fetch));
  // A single word, or anything with whitespace, is a search query through the
  // default engine (DuckDuckGo); the query is percent-encoded.
  EXPECT_EQ(controller.ResolveInput("weather"), "https://duckduckgo.com/?q=weather");
  EXPECT_EQ(controller.ResolveInput("hello world"), "https://duckduckgo.com/?q=hello%20world");
  EXPECT_EQ(controller.ResolveInput("中文 搜索"),
            "https://duckduckgo.com/?q=%E4%B8%AD%E6%96%87%20%E6%90%9C%E7%B4%A2");
  // Path-like input stays a local path; bare hostnames still get http://.
  EXPECT_EQ(controller.ResolveInput("tests/pages/x.html"), "tests/pages/x.html");
  EXPECT_EQ(controller.ResolveInput("example.com"), "http://example.com");

  // The engine id redirects the target...
  controller.SetPreference("search_engine", "bing");
  EXPECT_EQ(controller.ResolveInput("weather"), "https://www.bing.com/search?q=weather");
  // ...and an explicit template overrides the built-in engine entirely.
  controller.SetPreference("search_engine_template", "https://example.org/find?q=%s&lang=zh");
  EXPECT_EQ(controller.ResolveInput("a b"), "https://example.org/find?q=a%20b&lang=zh");
}

TEST(BrowserControllerTest, PreferencesPersistAcrossControllers)
{
  TempProfile tp;
  {
    BrowserController controller(tp.path());
    controller.SetPreference("home_page", "https://example.net/");
    EXPECT_EQ(controller.GetPreference("home_page"), "https://example.net/");
    EXPECT_EQ(controller.GetPreference("unset", "fallback"), "fallback");
  }
  {
    BrowserController controller(tp.path());
    ASSERT_TRUE(controller.Load().has_value());
    EXPECT_EQ(controller.GetPreference("home_page"), "https://example.net/");
    EXPECT_EQ(controller.SnapshotPreferences().size(), 1u);
  }
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
  ASSERT_TRUE(storage::WriteFileAtomic(file,
                                       "<html><title>F</title><body>"
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
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
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
            FakeFetcher::Route{
                200, {{"content-type", "text/html"}}, "<html><body>results</body></html>"});
  fetch.Add(
      "http://example.com/",
      FakeFetcher::Route{200,
                         {{"content-type", "text/html"}},
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

TEST(BrowserControllerTest, FocusAndTypeIntoTextarea)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               "<html><body><textarea id=\"q\">hi</textarea></body></html>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  tab->page->Layout(800, 600);
  dom::Element* textarea = dom::QuerySelector(*tab->page->document(), "#q");
  ASSERT_NE(textarea, nullptr);
  float x = 0;
  float y = 0;
  ASSERT_TRUE(FindElementRunPoint(*tab->page->layout_root(), textarea, x, y));

  controller.DispatchPointerClick(tab->id, x, y);
  EXPECT_EQ(tab->focused_element, textarea);
  EXPECT_TRUE(controller.DispatchKeyboard(tab->id, "keydown", "你", "KeyN"));
  EXPECT_EQ(textarea->TextContent(), "hi你");
  EXPECT_TRUE(controller.DispatchKeyboard(tab->id, "keydown", "Backspace", "Backspace"));
  EXPECT_EQ(textarea->TextContent(), "hi");
  EXPECT_TRUE(controller.DispatchKeyboard(tab->id, "keydown", "Enter", "Enter"));
  EXPECT_EQ(textarea->TextContent(), "hi\n");
}

TEST(BrowserControllerTest, ElementGeometryApisReflectLayout)
{
  TempProfile tp;
  FakeFetcher fetch;
  // Default (content-box) sizing: width:100px is the content width, so the
  // border box adds 2px borders + 4px padding each side.
  fetch.Add(
      "http://example.com/",
      FakeFetcher::Route{200,
                         {{"content-type", "text/html"}},
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
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
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
  fetch.Add(
      "http://example.com/nav",
      FakeFetcher::Route{200, {{"content-type", "text/html"}}, "<html><body>nav</body></html>"});
  fetch.Add("http://example.com/",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
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
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
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
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
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
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
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

// ---------------------------------------------------------------------------
// Password manager
// ---------------------------------------------------------------------------

// The login page used by the password-manager tests: a form with a username,
// a password field (optionally pre-filled) and a submit button.
std::string LoginPageHtml(const std::string& prefill_password = "")
{
  const std::string pw_attr = prefill_password.empty() ? "" : " value=\"" + prefill_password + "\"";
  return "<html><body><h1>Sign in</h1>"
         "<form action=\"/welcome\">"
         "<input name=\"user\" type=\"text\">"
         "<input name=\"pw\" type=\"password\"" +
         pw_attr +
         ">"
         "<button type=\"submit\">Sign in</button>"
         "</form></body></html>";
}

// Fills the form's username/password and submits it by clicking the submit
// button (mirrors a user signing in).
void SubmitLoginForm(BrowserController& controller,
                     Tab& tab,
                     const std::string& username,
                     const std::string& password)
{
  tab.page->Layout(800, 600);
  const std::vector<dom::Element*> inputs = dom::QuerySelectorAll(*tab.page->document(), "input");
  dom::Element* user = nullptr;
  dom::Element* pw = nullptr;
  for (dom::Element* input : inputs) {
    if (std::string(input->GetAttribute("type").value_or("")) == "password") {
      pw = input;
    } else if (user == nullptr) {
      user = input;
    }
  }
  ASSERT_NE(pw, nullptr);
  if (user != nullptr) {
    user->SetAttribute("value", username);
  }
  pw->SetAttribute("value", password);
  dom::Element* button = dom::QuerySelector(*tab.page->document(), "button");
  ASSERT_NE(button, nullptr);
  float x = 0;
  float y = 0;
  ASSERT_TRUE(FindElementRunPoint(*tab.page->layout_root(), button, x, y));
  EXPECT_TRUE(controller.DispatchPointerClick(tab.id, x, y));
}

TEST(BrowserControllerTest, PasswordManagerCapturesAndFillsSavedLogin)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/login",
            FakeFetcher::Route{200, {{"content-type", "text/html"}}, LoginPageHtml()});
  fetch.Add("http://example.com/welcome",
            FakeFetcher::Route{
                200, {{"content-type", "text/html"}}, "<html><body>Welcome</body></html>"});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/login").has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);

  SubmitLoginForm(controller, *tab, "alice", "s3cret");
  // The submission captured the login (and navigated to /welcome).
  ASSERT_TRUE(tab->pending_credential.has_value());
  EXPECT_EQ(tab->pending_credential->origin, "http://example.com");
  EXPECT_EQ(tab->pending_credential->username, "alice");
  EXPECT_EQ(tab->pending_credential->password, "s3cret");
  // The snapshot exposes only the displayable summary, never the password.
  const TabSnapshot snap = controller.SnapshotTab(tab->id);
  EXPECT_TRUE(snap.has_pending_credential);
  EXPECT_EQ(snap.pending_credential_origin, "http://example.com");
  EXPECT_EQ(snap.pending_credential_username, "alice");

  // Confirm the save: the pending state clears and the login is listed.
  controller.SavePendingCredential(tab->id);
  EXPECT_FALSE(tab->pending_credential.has_value());
  const std::vector<SavedLogin> saved = controller.SavedLogins();
  ASSERT_EQ(saved.size(), 1u);
  EXPECT_EQ(saved[0].origin, "http://example.com");
  EXPECT_EQ(saved[0].username, "alice");

  // Revisit the login page: both fields come back pre-filled.
  ASSERT_TRUE(controller.NavigateActive("http://example.com/login").has_value());
  Tab* tab2 = controller.ActiveTab();
  ASSERT_NE(tab2, nullptr);
  const std::vector<dom::Element*> inputs = dom::QuerySelectorAll(*tab2->page->document(), "input");
  ASSERT_EQ(inputs.size(), 2u);
  EXPECT_EQ(std::string(inputs[0]->GetAttribute("value").value_or("")), "alice");
  EXPECT_EQ(std::string(inputs[1]->GetAttribute("value").value_or("")), "s3cret");
}

TEST(BrowserControllerTest, PasswordManagerPersistsAcrossControllers)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/login",
            FakeFetcher::Route{200, {{"content-type", "text/html"}}, LoginPageHtml()});
  fetch.Add("http://example.com/welcome",
            FakeFetcher::Route{
                200, {{"content-type", "text/html"}}, "<html><body>Welcome</body></html>"});
  int tab_id = 0;
  {
    BrowserController controller(tp.path(), std::ref(fetch));
    controller.NewTab();
    ASSERT_TRUE(controller.NavigateActive("http://example.com/login").has_value());
    tab_id = controller.ActiveTab()->id;
    SubmitLoginForm(controller, *controller.ActiveTab(), "alice", "s3cret");
    controller.SavePendingCredential(tab_id);

    // An explicit dismissal drops the capture without saving it.
    ASSERT_TRUE(controller.NavigateActive("http://example.com/login").has_value());
    SubmitLoginForm(controller, *controller.ActiveTab(), "bob", "other");
    controller.DismissPendingCredential(tab_id);
    EXPECT_FALSE(controller.ActiveTab()->pending_credential.has_value());
    EXPECT_EQ(controller.SavedLogins().size(), 1u);
  }

  // A fresh controller on the same profile loads the saved login from disk.
  BrowserController second(tp.path(), std::ref(fetch));
  ASSERT_TRUE(second.Load().has_value());
  const std::vector<SavedLogin> saved = second.SavedLogins();
  ASSERT_EQ(saved.size(), 1u);
  EXPECT_EQ(saved[0].username, "alice");

  // Removing it persists the removal.
  EXPECT_TRUE(second.RemoveSavedLogin("http://example.com", "alice"));
  EXPECT_TRUE(second.SavedLogins().empty());
  BrowserController third(tp.path(), std::ref(fetch));
  ASSERT_TRUE(third.Load().has_value());
  EXPECT_TRUE(third.SavedLogins().empty());
}

TEST(BrowserControllerTest, PasswordManagerDoesNotFillForeignOrigin)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/login",
            FakeFetcher::Route{200, {{"content-type", "text/html"}}, LoginPageHtml()});
  fetch.Add("http://example.com/welcome",
            FakeFetcher::Route{
                200, {{"content-type", "text/html"}}, "<html><body>Welcome</body></html>"});
  fetch.Add("http://other.example/login",
            FakeFetcher::Route{200, {{"content-type", "text/html"}}, LoginPageHtml()});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/login").has_value());
  SubmitLoginForm(controller, *controller.ActiveTab(), "alice", "s3cret");
  controller.SavePendingCredential(controller.ActiveTab()->id);

  // The same form on another origin stays untouched (exact origin match).
  ASSERT_TRUE(controller.NavigateActive("http://other.example/login").has_value());
  const std::vector<dom::Element*> inputs =
      dom::QuerySelectorAll(*controller.ActiveTab()->page->document(), "input");
  ASSERT_EQ(inputs.size(), 2u);
  EXPECT_EQ(std::string(inputs[0]->GetAttribute("value").value_or("")), "");
  EXPECT_EQ(std::string(inputs[1]->GetAttribute("value").value_or("")), "");
}

TEST(BrowserControllerTest, PasswordManagerDoesNotOverwriteExistingValue)
{
  TempProfile tp;
  FakeFetcher fetch;
  fetch.Add("http://example.com/login",
            FakeFetcher::Route{200, {{"content-type", "text/html"}}, LoginPageHtml()});
  fetch.Add("http://example.com/welcome",
            FakeFetcher::Route{
                200, {{"content-type", "text/html"}}, "<html><body>Welcome</body></html>"});
  // The same login page, but the password comes pre-filled by the page (or the
  // user already typed something).
  fetch.Add("http://example.com/login2",
            FakeFetcher::Route{200,
                               {{"content-type", "text/html"}},
                               LoginPageHtml(/*prefill_password=*/"typed-by-user")});
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  ASSERT_TRUE(controller.NavigateActive("http://example.com/login").has_value());
  SubmitLoginForm(controller, *controller.ActiveTab(), "alice", "s3cret");
  controller.SavePendingCredential(controller.ActiveTab()->id);

  ASSERT_TRUE(controller.NavigateActive("http://example.com/login2").has_value());
  const std::vector<dom::Element*> inputs =
      dom::QuerySelectorAll(*controller.ActiveTab()->page->document(), "input");
  ASSERT_EQ(inputs.size(), 2u);
  EXPECT_EQ(std::string(inputs[0]->GetAttribute("value").value_or("")), "");
  EXPECT_EQ(std::string(inputs[1]->GetAttribute("value").value_or("")), "typed-by-user");
}

TEST(BrowserControllerTest, PasswordManagerSkipsOriginlessDocuments)
{
  TempProfile tp;
  FakeFetcher fetch;
  BrowserController controller(tp.path(), std::ref(fetch));
  controller.NewTab();
  const int tab_id = controller.ActiveTab()->id;
  ASSERT_TRUE(
      controller.LoadDocument(tab_id, LoginPageHtml(), "text/html", "file:///tmp/neko-login.html")
          .has_value());
  Tab* tab = controller.ActiveTab();
  ASSERT_NE(tab, nullptr);
  SubmitLoginForm(controller, *tab, "alice", "s3cret");
  // file: documents have an opaque origin: the submission is never captured.
  EXPECT_FALSE(tab->pending_credential.has_value());
  EXPECT_TRUE(controller.SavedLogins().empty());
}

} // namespace
} // namespace neko::browser
