#include "neko/network/http.h"
#include "neko/network/socket.h"
#include "neko/url/url.h"

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace neko::network {
namespace {

#ifndef _WIN32
// A tiny single-purpose HTTP server for tests (POSIX only).
class TestHttpServer {
 public:
  explicit TestHttpServer(int max_connections = 8) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return;
    }
    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // ephemeral
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

  ~TestHttpServer() {
    if (listen_fd_ >= 0) {
      // shutdown() unblocks a thread blocked in accept() on the same socket.
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  bool IsValid() const { return listen_fd_ >= 0; }
  uint16_t port() const { return port_; }

  // The raw request headers of every handled request, in order.
  std::vector<std::string> Requests() {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

 private:
  void Run(int max_connections) {
    int handled = 0;
    while (handled < max_connections) {
      sockaddr_in client {};
      socklen_t client_len = sizeof(client);
      const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client), &client_len);
      if (fd < 0) {
        return;
      }
      Handle(fd);
      ::close(fd);
      ++handled;
    }
  }

  void Handle(int fd) {
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
    // Copy into an owned string: the view would point at the (potentially
    // SSO) request buffer while crossing into SendResponse.
    const std::string path(request.substr(path_start + 1, path_end - path_start - 1));
    SendResponse(fd, path);
  }

  void SendResponse(int fd, const std::string& path) {
    std::string response;
    if (path == "/") {
      response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 20\r\n"
                 "\r\n<h1>Hello World</h1>";
    } else if (path == "/redirect") {
      response = "HTTP/1.1 302 Found\r\nLocation: /\r\nContent-Length: 0\r\n\r\n";
    } else if (path == "/chunked") {
      response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
    } else if (path == "/empty") {
      response = "HTTP/1.1 204 No Content\r\n\r\n";
    } else if (path == "/gzip") {
      response = "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 4\r\n"
                 "\r\n\x1f\x8b\x08\x00";
    } else {
      response = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found";
    }
    ::send(fd, response.data(), response.size(), 0);
  }

  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::thread thread_;
  std::mutex mutex_;
  std::vector<std::string> requests_;
};
#endif

TEST(HttpTest, ParseBasicResponse) {
  const auto result =
      ParseHttpResponse("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 5\r\n"
                        "\r\nhello");
  ASSERT_TRUE(result.has_value());
  const HttpResponse& r = result.value();
  EXPECT_EQ(r.status_code, 200);
  EXPECT_EQ(r.reason, "OK");
  EXPECT_EQ(r.GetHeader("content-type"), "text/html");
  EXPECT_EQ(r.body, "hello");
}

TEST(HttpTest, ParseChunkedResponse) {
  const auto result = ParseHttpResponse(
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().status_code, 200);
  EXPECT_EQ(result.value().body, "hello world");
}

TEST(HttpTest, ParseMalformed) {
  EXPECT_FALSE(ParseHttpResponse("not http").has_value());
  EXPECT_FALSE(ParseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\nx").has_value());
}

TEST(HttpTest, RejectsChunkSizeOverflow) {
  // A chunk size of 2^64 wraps to 0 in size_t arithmetic; the decoder must
  // reject it instead of silently truncating the body.
  const auto result = ParseHttpResponse(
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "10000000000000000\r\nhello\r\n0\r\n\r\n");
  EXPECT_FALSE(result.has_value());
}

TEST(HttpTest, RejectsChunkSizeWrapsToSmallValue) {
  // 1 followed by many zeros wraps to a small nonzero value that passes the
  // "chunk fits in remaining data" check; the decoder must still reject it.
  const auto result = ParseHttpResponse(
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "100000000000000000000000000000000000000000000000000\r\n"
      "hello\r\n0\r\n\r\n");
  EXPECT_FALSE(result.has_value());
}

TEST(HttpTest, RejectsContentLengthOverflow) {
  // A 30-digit Content-Length overflows size_t and wraps to a small value;
  // the decoder must reject it instead of slicing the body at a bogus
  // offset.
  const auto result = ParseHttpResponse(
      "HTTP/1.1 200 OK\r\nContent-Length: 999999999999999999999999999999\r\n\r\nhello");
  EXPECT_FALSE(result.has_value());
}

TEST(HttpTest, RejectsConflictingContentLength) {
  // Two different Content-Length values make the body boundary ambiguous
  // (a request-smuggling / response-splitting vector); reject (RFC 7230
  // 3.3.3).
  const auto result = ParseHttpResponse(
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 100\r\n\r\nhello");
  EXPECT_FALSE(result.has_value());
}

TEST(HttpTest, AcceptsIdenticalContentLength) {
  // Duplicate but identical Content-Length values are permitted.
  const auto result = ParseHttpResponse(
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().body, "hello");
}

TEST(HttpTest, RejectsTransferEncodingWithContentLength) {
  // Content-Length alongside Transfer-Encoding is ambiguous and must be
  // rejected (RFC 7230 3.3.3).
  const auto result = ParseHttpResponse(
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n");
  EXPECT_FALSE(result.has_value());
}

TEST(HttpTest, RejectsUnsupportedTransferEncoding) {
  // A non-chunked Transfer-Encoding must not be silently treated as a plain
  // body.
  const auto result = ParseHttpResponse(
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n\x1f\x8b\x08\x00");
  EXPECT_FALSE(result.has_value());
}

TEST(HttpTest, HttpsNotImplemented) {
  const auto url = url::Url::Parse("https://example.com/");
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kNotImplemented);
}

#ifndef _WIN32
TEST(HttpTest, GetFromLocalServer) {
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) + "/";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().status_code, 200);
  EXPECT_EQ(result.value().GetHeader("content-type"), "text/html");
  EXPECT_EQ(result.value().body, "<h1>Hello World</h1>");
}

TEST(HttpTest, RedirectFollowed) {
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) + "/redirect";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().status_code, 200);
  EXPECT_EQ(result.value().body, "<h1>Hello World</h1>");
}

TEST(HttpTest, ChunkedFromServer) {
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) + "/chunked";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().status_code, 200);
  EXPECT_EQ(result.value().body, "hello world");
}

TEST(HttpTest, NotFound) {
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) + "/missing";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().status_code, 404);
}

TEST(HttpTest, CompressionNotImplemented) {
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) + "/gzip";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kNotImplemented);
}

TEST(HttpTest, ExtraHeadersSent) {
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) + "/";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value(), 5, [](const url::Url&) {
    return std::vector<HttpHeader>{{ "cookie", "session=abc123" }};
  });
  ASSERT_TRUE(result.has_value());
  const auto requests = server.Requests();
  ASSERT_EQ(requests.size(), 1u);
  // Header names are emitted verbatim from the provider (lowercase here;
  // HTTP header names are case-insensitive on the wire).
  EXPECT_NE(requests[0].find("cookie: session=abc123"), std::string::npos);
}

TEST(HttpTest, ExtraHeadersRecomputedPerRedirectHop) {
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) + "/redirect";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  int calls = 0;
  const auto result = HttpGet(url.value(), 5, [&calls](const url::Url& target) {
    ++calls;
    // Cookie scoped to the redirect target host.
    return std::vector<HttpHeader>{ { "cookie", "host=" + target.host() } };
  });
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().status_code, 200);
  EXPECT_GE(calls, 2);  // initial request + redirect hop
  const auto requests = server.Requests();
  ASSERT_EQ(requests.size(), 2u);
  // The cookie header on the redirected request targets the final host.
  EXPECT_NE(requests[1].find("cookie: host="), std::string::npos);
}

TEST(SocketTest, ConnectRefused) {
  // Find a port that is very likely closed by binding and releasing.
  const auto socket = Socket::Connect("127.0.0.1", 1, 500);
  EXPECT_FALSE(socket.has_value());
}
#endif

}  // namespace
}  // namespace neko::network
