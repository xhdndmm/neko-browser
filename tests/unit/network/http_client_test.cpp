// neko::network::HttpClient — keep-alive connection reuse.
//
// These tests drive a loopback HTTP/1.1 server that keeps its connections open
// (unlike TestHttpServer in network_test.cpp, which closes after every
// response) and counts them, so "the engine reuses connections per origin" is
// asserted on observable behaviour rather than on internal state.
//
// The server runs on its own thread; every test uses its own HttpClient, so
// pools never leak between cases.

#include "neko/network/http.h"
#include "neko/network/http_client.h"
#include "neko/url/url.h"

#include <atomic>
#include <cstring>
#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace neko::network {
namespace {

// How the server treats a connection after answering a request.
enum class ServerMode
{
  kKeepAlive,       // stay open: the client is expected to reuse it.
  kCloseNoHeader,   // close without saying so (idle timeout / silent drop).
  kCloseHeader,     // announce `Connection: close`.
  kTrailingGarbage, // send bytes past the framed body (never pipeline).
  kChunked,         // answer with a chunked body and stay open.
};

class TestKeepAliveServer
{
public:
  explicit TestKeepAliveServer(ServerMode mode) : mode_(mode)
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
    addr.sin_port = 0; // ephemeral
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
    thread_ = std::thread([this] { Run(); });
  }

  ~TestKeepAliveServer()
  {
    // shutdown() unblocks a thread blocked in accept() on the same socket.
    std::vector<std::thread> handlers;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const int fd : connection_fds_) {
        ::shutdown(fd, SHUT_RDWR);
      }
      handlers.swap(handler_threads_);
    }
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    for (std::thread& handler : handlers) {
      if (handler.joinable()) {
        handler.join();
      }
    }
  }

  TestKeepAliveServer(const TestKeepAliveServer&) = delete;
  TestKeepAliveServer& operator=(const TestKeepAliveServer&) = delete;

  bool IsValid() const
  {
    return listen_fd_ >= 0;
  }
  std::uint16_t port() const
  {
    return port_;
  }
  std::size_t connection_count() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return connections_;
  }
  std::size_t request_count() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_.size();
  }
  std::vector<std::string> requests() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

private:
  void Run()
  {
    for (;;) {
      sockaddr_in client{};
      socklen_t client_len = sizeof(client);
      const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client), &client_len);
      if (fd < 0) {
        return;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++connections_;
        connection_fds_.push_back(fd);
      }
      // One thread per connection: a real server accepts the next connection
      // while an earlier one is still being kept alive (a sequential server
      // would make connection reuse untestable).
      std::lock_guard<std::mutex> lock(mutex_);
      handler_threads_.emplace_back([this, fd] {
        Serve(fd);
        ::close(fd);
      });
    }
  }

  // Reads requests on one connection until the client closes it (or the mode
  // says this connection is done after a single response).
  void Serve(int fd)
  {
    for (;;) {
      std::string request;
      char buffer[4096];
      bool closed = false;
      while (request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
          closed = true;
          break;
        }
        request.append(buffer, static_cast<std::size_t>(n));
      }
      if (closed) {
        return;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(request);
      }
      if (!SendResponse(fd)) {
        return;
      }
      if (mode_ != ServerMode::kKeepAlive && mode_ != ServerMode::kChunked) {
        return; // kCloseNoHeader / kCloseHeader / kTrailingGarbage: one shot
      }
    }
  }

  // Sends the response for the last request.  Returns false when the write
  // failed (the peer is gone).
  bool SendResponse(int fd)
  {
    constexpr std::string_view kBody = "Hello World";
    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n";
    if (mode_ == ServerMode::kChunked) {
      response += "Transfer-Encoding: chunked\r\n\r\n";
      response += "b\r\nHello World\r\n0\r\n\r\n";
    } else {
      response += "Content-Length: " + std::to_string(kBody.size()) + "\r\n";
      if (mode_ == ServerMode::kCloseHeader) {
        response += "Connection: close\r\n";
      }
      response += "\r\n";
      response += kBody;
      if (mode_ == ServerMode::kTrailingGarbage) {
        // Bytes belonging to no response: the connection must not be pooled.
        response += "EXTRA";
      }
    }
    return ::send(fd, response.data(), response.size(), MSG_NOSIGNAL) ==
           static_cast<ssize_t>(response.size());
  }

  ServerMode mode_;
  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::thread thread_;
  mutable std::mutex mutex_;
  std::size_t connections_ = 0;
  std::vector<std::string> requests_;
  std::vector<std::thread> handler_threads_;
  std::vector<int> connection_fds_;
};

std::string UrlFor(const TestKeepAliveServer& server)
{
  return "http://127.0.0.1:" + std::to_string(server.port()) + "/";
}

TEST(HttpClientTest, ReusesOneConnectionForSequentialRequests)
{
  TestKeepAliveServer server(ServerMode::kKeepAlive);
  ASSERT_TRUE(server.IsValid());
  HttpClient client;

  for (int i = 0; i < 3; ++i) {
    const base::Result<HttpResponse> response = client.Get(url::Url::Parse(UrlFor(server)).value());
    ASSERT_TRUE(response.has_value()) << response.error().message();
    EXPECT_EQ(response.value().status_code, 200);
    EXPECT_EQ(response.value().body, "Hello World");
  }

  const HttpClient::Stats stats = client.stats();
  EXPECT_EQ(stats.connections_opened, 1u); // the whole point: one socket for three requests
  EXPECT_EQ(stats.connections_reused, 2u);
  EXPECT_EQ(stats.connections_discarded, 0u);
  EXPECT_EQ(server.connection_count(), 1u);
  EXPECT_EQ(server.request_count(), 3u);
  // The request must not ask the server to close, or reuse would be impossible.
  ASSERT_FALSE(server.requests().empty());
  EXPECT_EQ(server.requests().front().find("Connection: close"), std::string::npos);
}

TEST(HttpClientTest, ReusesConnectionForChunkedResponses)
{
  TestKeepAliveServer server(ServerMode::kChunked);
  ASSERT_TRUE(server.IsValid());
  HttpClient client;

  for (int i = 0; i < 2; ++i) {
    const base::Result<HttpResponse> response = client.Get(url::Url::Parse(UrlFor(server)).value());
    ASSERT_TRUE(response.has_value()) << response.error().message();
    EXPECT_EQ(response.value().body, "Hello World");
  }

  EXPECT_EQ(client.stats().connections_opened, 1u);
  EXPECT_EQ(server.connection_count(), 1u);
}

TEST(HttpClientTest, DoesNotReuseWhenTheResponseAsksForClose)
{
  TestKeepAliveServer server(ServerMode::kCloseHeader);
  ASSERT_TRUE(server.IsValid());
  HttpClient client;

  for (int i = 0; i < 2; ++i) {
    const base::Result<HttpResponse> response = client.Get(url::Url::Parse(UrlFor(server)).value());
    ASSERT_TRUE(response.has_value()) << response.error().message();
    EXPECT_EQ(response.value().body, "Hello World");
  }

  EXPECT_EQ(client.stats().connections_opened, 2u);
  EXPECT_EQ(client.stats().connections_reused, 0u);
  EXPECT_EQ(server.connection_count(), 2u);
}

// The peer closed an idle pooled connection without announcing it (the common
// "keep-alive idle timeout", and what most servers do to a pooled socket).  The
// request must still succeed exactly once.
TEST(HttpClientTest, RetriesOnAFreshConnectionWhenThePooledOneIsGone)
{
  TestKeepAliveServer server(ServerMode::kCloseNoHeader);
  ASSERT_TRUE(server.IsValid());
  HttpClient client;

  for (int i = 0; i < 2; ++i) {
    const base::Result<HttpResponse> response = client.Get(url::Url::Parse(UrlFor(server)).value());
    ASSERT_TRUE(response.has_value()) << response.error().message();
    EXPECT_EQ(response.value().status_code, 200);
    EXPECT_EQ(response.value().body, "Hello World");
  }

  // Either the pending-input probe or the retry-once path catches it; both
  // mean the second request went out on a new connection.
  EXPECT_EQ(client.stats().connections_opened, 2u);
  EXPECT_EQ(client.stats().requests, 2u);
  EXPECT_LE(client.stats().connections_reused, 1u);
  EXPECT_EQ(server.request_count(), 2u);
}

// We never pipeline, so bytes past the framed body mean the peer is not
// speaking the framing we expect: the connection must be dropped, not reused.
TEST(HttpClientTest, DiscardsAConnectionThatLeftTrailingBytes)
{
  TestKeepAliveServer server(ServerMode::kTrailingGarbage);
  ASSERT_TRUE(server.IsValid());
  HttpClient client;

  for (int i = 0; i < 2; ++i) {
    const base::Result<HttpResponse> response = client.Get(url::Url::Parse(UrlFor(server)).value());
    ASSERT_TRUE(response.has_value()) << response.error().message();
    EXPECT_EQ(response.value().body, "Hello World");
  }

  EXPECT_EQ(client.stats().connections_opened, 2u);
  EXPECT_EQ(client.stats().connections_reused, 0u);
}

TEST(HttpClientTest, CloseIdleConnectionsDropsThePool)
{
  TestKeepAliveServer server(ServerMode::kKeepAlive);
  ASSERT_TRUE(server.IsValid());
  HttpClient client;

  const base::Result<HttpResponse> first = client.Get(url::Url::Parse(UrlFor(server)).value());
  ASSERT_TRUE(first.has_value()) << first.error().message();
  EXPECT_EQ(client.stats().connections_opened, 1u);

  client.CloseIdleConnections();

  const base::Result<HttpResponse> second = client.Get(url::Url::Parse(UrlFor(server)).value());
  ASSERT_TRUE(second.has_value()) << second.error().message();
  EXPECT_EQ(second.value().body, "Hello World");
  EXPECT_EQ(client.stats().connections_opened, 2u);
  EXPECT_EQ(client.stats().connections_reused, 0u);
}

// Separate clients must not share a pool: a request's trust/proxy/timeout
// settings are part of the connection's identity.
TEST(HttpClientTest, SeparateClientsDoNotShareConnections)
{
  TestKeepAliveServer server(ServerMode::kKeepAlive);
  ASSERT_TRUE(server.IsValid());
  HttpClient first_client;
  HttpClient second_client;

  const base::Result<HttpResponse> a = first_client.Get(url::Url::Parse(UrlFor(server)).value());
  ASSERT_TRUE(a.has_value()) << a.error().message();
  const base::Result<HttpResponse> b = second_client.Get(url::Url::Parse(UrlFor(server)).value());
  ASSERT_TRUE(b.has_value()) << b.error().message();

  EXPECT_EQ(first_client.stats().connections_opened, 1u);
  EXPECT_EQ(second_client.stats().connections_opened, 1u);
  EXPECT_EQ(server.connection_count(), 2u);
}

TEST(HttpClientTest, RejectsUnsupportedSchemeWithoutTouchingThePool)
{
  HttpClient client;
  const base::Result<HttpResponse> response =
      client.Get(url::Url::Parse("ftp://host/file").value());
  ASSERT_FALSE(response.has_value());
  EXPECT_EQ(client.stats().connections_opened, 0u);
}

} // namespace
} // namespace neko::network

#endif // !_WIN32
