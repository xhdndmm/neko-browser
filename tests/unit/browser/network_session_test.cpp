// End-to-end tests for the network process (ADR 0016 M3a): a real
// --network-child process fetches from a loopback HTTP server, and cookie
// lookups for redirect hops travel back to the browser-side callback.
//
// The loopback server is POSIX-only (same policy as the network suite);
// Windows CI only verifies that the spawn-rejection paths compile and pass.

#include "neko/browser/browser_controller.h"
#include "neko/browser/network_session.h"
#include "neko/url/url.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#ifndef _WIN32
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <filesystem>
#include <functional>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>
#endif

namespace neko::browser {
namespace {

TEST(NetworkSessionTest, SpawnRejectsEmptyExecutable)
{
  const auto session = NetworkSession::Spawn("");
  EXPECT_FALSE(session.has_value());
}

TEST(NetworkSessionTest, ReportsErrorWhenChildCannotStart)
{
  const auto session = NetworkSession::Spawn("/nonexistent/neko-browser-test");
  if (!session.has_value()) {
    SUCCEED(); // Refusing to spawn outright is also acceptable.
    return;
  }
  // A child that cannot serve the protocol must fail the first request
  // instead of hanging.
  const auto url = url::Url::Parse("http://127.0.0.1:1/");
  ASSERT_TRUE(url.has_value());
  NetworkSession::CookieLookupFn no_lookup;
  EXPECT_FALSE(session.value()->Fetch(url.value(), /*cookie_header=*/{}, no_lookup).has_value());
  EXPECT_FALSE(session.value()->alive());
}

#ifndef _WIN32

std::string Lowercase(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

// A minimal loopback HTTP server: the handler maps a request path to a raw
// response; every request's headers are recorded so cookie forwarding can be
// asserted.
class LoopbackHttpServer
{
public:
  using Handler = std::function<std::string(const std::string& path, const std::string& request)>;

  explicit LoopbackHttpServer(Handler handler, int max_connections = 8)
      : handler_(std::move(handler))
  {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return;
    }
    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listen_fd_, 8) != 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this, max_connections] { Run(max_connections); });
  }

  ~LoopbackHttpServer()
  {
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  LoopbackHttpServer(const LoopbackHttpServer&) = delete;
  LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

  bool valid() const
  {
    return listen_fd_ >= 0;
  }
  std::uint16_t port() const
  {
    return port_;
  }

  std::string Url(const std::string& path) const
  {
    return "http://127.0.0.1:" + std::to_string(port_) + path;
  }

  std::vector<std::string> Requests()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

private:
  void Run(int max_connections)
  {
    for (int handled = 0; handled < max_connections; ++handled) {
      sockaddr_in client{};
      socklen_t client_len = sizeof(client);
      const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client), &client_len);
      if (fd < 0) {
        return;
      }
      Handle(fd);
      ::close(fd);
    }
  }

  void Handle(int fd)
  {
    std::string request;
    char buffer[4096];
    for (;;) {
      const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
      if (n <= 0) {
        break;
      }
      request.append(buffer, static_cast<std::size_t>(n));
      if (request.find("\r\n\r\n") != std::string::npos) {
        break;
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      requests_.push_back(request);
    }
    const std::size_t path_start = request.find(' ');
    const std::size_t path_end = request.find(' ', path_start + 1);
    const std::string path(request.substr(path_start + 1, path_end - path_start - 1));
    const std::string response = handler_(path, request);
    ::send(fd, response.data(), response.size(), 0);
  }

  Handler handler_;
  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::thread thread_;
  std::mutex mutex_;
  std::vector<std::string> requests_;
};

TEST(NetworkSessionTest, FetchThroughChildReturnsTheResponse)
{
  LoopbackHttpServer server([](const std::string& path, const std::string&) {
    if (path == "/hello") {
      return std::string("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nX-Test: 1\r\n"
                         "Content-Length: 9\r\n\r\nhello raw");
    }
    return std::string("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
  });
  ASSERT_TRUE(server.valid());

  auto session = NetworkSession::Spawn(NEKO_BROWSER_BIN);
  ASSERT_TRUE(session.has_value()) << session.error().message();
  const auto url = url::Url::Parse(server.Url("/hello"));
  ASSERT_TRUE(url.has_value());

  NetworkSession::CookieLookupFn no_lookup;
  const auto response = session.value()->Fetch(url.value(), /*cookie_header=*/{}, no_lookup);
  ASSERT_TRUE(response.has_value()) << response.error().message();
  EXPECT_EQ(response.value().status_code, 200);
  EXPECT_EQ(response.value().GetHeader("x-test"), "1");
  EXPECT_EQ(response.value().body, "hello raw");
  EXPECT_NE(response.value().final_url.find("/hello"), std::string::npos);
}

TEST(NetworkSessionTest, RedirectHopsAskTheBrowserForCookies)
{
  LoopbackHttpServer server([](const std::string& path, const std::string&) {
    if (path == "/a") {
      return std::string("HTTP/1.1 302 Found\r\nLocation: /b\r\nContent-Length: 0\r\n\r\n");
    }
    if (path == "/b") {
      return std::string("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone");
    }
    return std::string("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
  });
  ASSERT_TRUE(server.valid());

  auto session = NetworkSession::Spawn(NEKO_BROWSER_BIN);
  ASSERT_TRUE(session.has_value()) << session.error().message();
  const auto url = url::Url::Parse(server.Url("/a"));
  ASSERT_TRUE(url.has_value());

  int lookup_calls = 0;
  NetworkSession::CookieLookupFn lookup = [&lookup_calls](const url::Url& target) {
    ++lookup_calls;
    // The child must ask for both the first hop and the redirect target.
    EXPECT_TRUE(target.host() == "127.0.0.1");
    return std::string("sid=secret");
  };
  const auto response = session.value()->Fetch(url.value(), /*cookie_header=*/{}, lookup);
  ASSERT_TRUE(response.has_value()) << response.error().message();
  EXPECT_EQ(response.value().status_code, 200);
  EXPECT_EQ(response.value().body, "done");
  EXPECT_GE(lookup_calls, 2);

  const std::vector<std::string> requests = server.Requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_NE(requests[0].find("GET /a"), std::string::npos);
  EXPECT_NE(requests[1].find("GET /b"), std::string::npos);
  for (const std::string& request : requests) {
    EXPECT_NE(Lowercase(request).find("cookie: sid=secret"), std::string::npos)
        << "request did not carry the cookie:\n"
        << request;
  }
}

TEST(NetworkSessionTest, ConnectionFailureIsReportedNotFatal)
{
  // A loopback port with no listener: bind an ephemeral port, then close it so
  // connecting is refused.
  int probe = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(probe, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ASSERT_EQ(::bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
  socklen_t len = sizeof(addr);
  ASSERT_EQ(::getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &len), 0);
  const std::uint16_t port = ntohs(addr.sin_port);
  ::close(probe);

  auto session = NetworkSession::Spawn(NEKO_BROWSER_BIN);
  ASSERT_TRUE(session.has_value()) << session.error().message();
  const auto url = url::Url::Parse("http://127.0.0.1:" + std::to_string(port) + "/");
  ASSERT_TRUE(url.has_value());

  NetworkSession::CookieLookupFn no_lookup;
  const auto response = session.value()->Fetch(url.value(), /*cookie_header=*/{}, no_lookup);
  EXPECT_FALSE(response.has_value());
  // The child answered with an error, so the session itself is still usable.
  EXPECT_TRUE(session.value()->alive());
}

TEST(NetworkSessionTest, ShutdownThenFetchFails)
{
  auto session = NetworkSession::Spawn(NEKO_BROWSER_BIN);
  ASSERT_TRUE(session.has_value()) << session.error().message();
  session.value()->Shutdown();
  EXPECT_FALSE(session.value()->alive());

  const auto url = url::Url::Parse("http://127.0.0.1:1/");
  ASSERT_TRUE(url.has_value());
  NetworkSession::CookieLookupFn no_lookup;
  EXPECT_FALSE(session.value()->Fetch(url.value(), {}, no_lookup).has_value());
}

TEST(NetworkProcessControllerTest, JarCookiesFlowThroughNavigations)
{
  LoopbackHttpServer server([](const std::string& path, const std::string&) {
    std::string body;
    std::string extra;
    if (path == "/one") {
      body = "<html><body>one</body></html>";
      extra = "Set-Cookie: sid=jar; Path=/\r\n";
    } else if (path == "/two") {
      body = "<html><body>two</body></html>";
    } else {
      return std::string("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
    }
    return "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n" + extra +
           "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
  });
  ASSERT_TRUE(server.valid());

  const std::string profile = std::string(testing::TempDir()) + "/neko_network_process_controller";
  std::filesystem::create_directories(profile);
  BrowserController controller(profile,
                               /*fetch=*/{},
                               RendererOptions{},
                               NetworkOptions{/*enabled=*/true, /*executable=*/NEKO_BROWSER_BIN});
  ASSERT_TRUE(controller.Load().has_value());

  const int tab = controller.NewTab(server.Url("/one"));
  ASSERT_TRUE(controller.Navigate(tab, server.Url("/two")).has_value());
  const TabSnapshot snapshot = controller.SnapshotTab(tab);
  EXPECT_EQ(snapshot.url, server.Url("/two"));

  // The first navigation had an empty jar (no Cookie header); the second one
  // carries the cookie the child could only get by asking the browser.
  const std::vector<std::string> requests = server.Requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(Lowercase(requests[0]).find("cookie:"), std::string::npos);
  EXPECT_NE(Lowercase(requests[1]).find("cookie: sid=jar"), std::string::npos) << requests[1];
}

#endif // !_WIN32

} // namespace
} // namespace neko::browser
