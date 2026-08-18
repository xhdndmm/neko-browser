#include "neko/network/compression.h"
#include "neko/network/http.h"
#include "neko/network/socket.h"
#include "neko/network/tls_socket.h"
#include "neko/url/url.h"

#include <atomic>
#include <gtest/gtest.h>
#include <mutex>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <string>
#include <string_view>
#include <thread>
#include <zlib.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace neko::network {
namespace {

// Compresses |data| with a gzip wrapper (RFC 1952), for building test bodies.
std::string GzipCompress(std::string_view data)
{
  z_stream strm{};
  if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) !=
      Z_OK) {
    return {};
  }
  std::string out;
  out.resize(deflateBound(&strm, static_cast<uLong>(data.size())));
  strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  strm.avail_in = static_cast<uInt>(data.size());
  strm.next_out = reinterpret_cast<Bytef*>(out.data());
  strm.avail_out = static_cast<uInt>(out.size());
  const int ret = deflate(&strm, Z_FINISH);
  deflateEnd(&strm);
  if (ret != Z_STREAM_END) {
    return {};
  }
  out.resize(out.size() - strm.avail_out);
  return out;
}

// Compresses |data| with a zlib (RFC 1950) wrapper.
std::string ZlibCompress(std::string_view data)
{
  z_stream strm{};
  if (deflateInit(&strm, Z_DEFAULT_COMPRESSION) != Z_OK) {
    return {};
  }
  std::string out;
  out.resize(deflateBound(&strm, static_cast<uLong>(data.size())));
  strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  strm.avail_in = static_cast<uInt>(data.size());
  strm.next_out = reinterpret_cast<Bytef*>(out.data());
  strm.avail_out = static_cast<uInt>(out.size());
  const int ret = deflate(&strm, Z_FINISH);
  deflateEnd(&strm);
  if (ret != Z_STREAM_END) {
    return {};
  }
  out.resize(out.size() - strm.avail_out);
  return out;
}

// Compresses |data| with a raw deflate stream (no wrapper), which some
// servers emit for "Content-Encoding: deflate".
std::string RawDeflateCompress(std::string_view data)
{
  z_stream strm{};
  if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    return {};
  }
  std::string out;
  out.resize(deflateBound(&strm, static_cast<uLong>(data.size())));
  strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  strm.avail_in = static_cast<uInt>(data.size());
  strm.next_out = reinterpret_cast<Bytef*>(out.data());
  strm.avail_out = static_cast<uInt>(out.size());
  const int ret = deflate(&strm, Z_FINISH);
  deflateEnd(&strm);
  if (ret != Z_STREAM_END) {
    return {};
  }
  out.resize(out.size() - strm.avail_out);
  return out;
}

#ifndef _WIN32
// A tiny single-purpose HTTP server for tests (POSIX only).
class TestHttpServer
{
public:
  explicit TestHttpServer(int max_connections = 8)
  {
    gzip_body_ = GzipCompress("Hello World");
    deflate_body_ = ZlibCompress("Hello World");
    raw_deflate_body_ = RawDeflateCompress("Hello World");
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
    thread_ = std::thread([this, max_connections] { Run(max_connections); });
  }

  ~TestHttpServer()
  {
    if (listen_fd_ >= 0) {
      // shutdown() unblocks a thread blocked in accept() on the same socket.
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

  // The raw request headers of every handled request, in order.
  std::vector<std::string> Requests()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

private:
  void Run(int max_connections)
  {
    int handled = 0;
    while (handled < max_connections) {
      sockaddr_in client{};
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
    // Copy into an owned string: the view would point at the (potentially
    // SSO) request buffer while crossing into SendResponse.
    const std::string path(request.substr(path_start + 1, path_end - path_start - 1));
    SendResponse(fd, path);
  }

  void SendResponse(int fd, const std::string& path)
  {
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
      response = "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: " +
                 std::to_string(gzip_body_.size()) + "\r\n\r\n" + gzip_body_;
    } else if (path == "/deflate") {
      response = "HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Length: " +
                 std::to_string(deflate_body_.size()) + "\r\n\r\n" + deflate_body_;
    } else if (path == "/raw-deflate") {
      response = "HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Length: " +
                 std::to_string(raw_deflate_body_.size()) + "\r\n\r\n" + raw_deflate_body_;
    } else if (path == "/double-encoded") {
      // Codings are listed in application order (RFC 7231 §3.1.2.1): gzip
      // applied first, then deflate; the decoder undoes them in reverse.
      const std::string body = ZlibCompress(gzip_body_);
      response = "HTTP/1.1 200 OK\r\nContent-Encoding: gzip, deflate\r\nContent-Length: " +
                 std::to_string(body.size()) + "\r\n\r\n" + body;
    } else if (path == "/bad-gzip") {
      response = "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 4\r\n"
                 "\r\n\x1f\x8b\x08\x00";
    } else if (path == "/huge-cl") {
      // Content-Length above the 512 MiB body limit must be rejected by
      // ClassifyBody before any allocation.
      response = "HTTP/1.1 200 OK\r\nContent-Length: 1073741824\r\n\r\nx";
    } else if (path == "/unframed-oversized") {
      // Exercise the until-close body limit without constructing the entire
      // response in memory at once.
      response = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n";
      ::send(fd, response.data(), response.size(), 0);
      const std::string chunk(1024u * 1024u, 'x');
      // Send a little extra so the client reaches the cap even if the final
      // socket read observes close before consuming the last packet.
      std::size_t remaining = 512u * 1024u * 1024u + 1024u * 1024u;
      while (remaining != 0) {
        const std::size_t count = std::min(remaining, chunk.size());
        const ssize_t sent = ::send(fd, chunk.data(), count, MSG_NOSIGNAL);
        if (sent <= 0) {
          return;
        }
        remaining -= static_cast<std::size_t>(sent);
      }
      return;
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
  std::string gzip_body_;
  std::string deflate_body_;
  std::string raw_deflate_body_;
};
#endif

// ---------------------------------------------------------------------------
// Local TLS test server (POSIX only)
// ---------------------------------------------------------------------------

#ifndef _WIN32
// Generates a self-signed certificate for CN=localhost with a SAN, returning
// its PEM form plus the OpenSSL objects (owned by the caller).
struct TestCert
{
  std::string cert_pem;
  X509* cert = nullptr;
  EVP_PKEY* key = nullptr;
};

TestCert GenerateLocalhostCert()
{
  TestCert out;
  out.key = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", 2048);
  out.cert = X509_new();
  X509_set_version(out.cert, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(out.cert), 1);
  X509_gmtime_adj(X509_getm_notBefore(out.cert), -60);
  X509_gmtime_adj(X509_getm_notAfter(out.cert), 60L * 60 * 24 * 365);
  X509_set_pubkey(out.cert, out.key);
  X509_NAME* name = X509_get_subject_name(out.cert);
  X509_NAME_add_entry_by_txt(
      name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
  X509_set_issuer_name(out.cert, name);
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, out.cert, out.cert, nullptr, nullptr, 0);
  X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, "DNS:localhost");
  if (ext != nullptr) {
    X509_add_ext(out.cert, ext, -1);
    X509_EXTENSION_free(ext);
  }
  X509_sign(out.cert, out.key, EVP_sha256());

  BIO* bio = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(bio, out.cert);
  char* data = nullptr;
  const long len = BIO_get_mem_data(bio, &data);
  out.cert_pem.assign(data, static_cast<std::size_t>(len));
  BIO_free(bio);
  return out;
}

// A minimal single-page TLS server (self-signed certificate) for tests.
class TestTlsServer
{
public:
  TestTlsServer() : cert_(GenerateLocalhostCert())
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

    ctx_ = SSL_CTX_new(TLS_server_method());
    if (ctx_ == nullptr || SSL_CTX_use_certificate(ctx_, cert_.cert) != 1 ||
        SSL_CTX_use_PrivateKey(ctx_, cert_.key) != 1) {
      SSL_CTX_free(ctx_);
      ctx_ = nullptr;
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    thread_ = std::thread([this] { Run(); });
  }

  ~TestTlsServer()
  {
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    if (ctx_ != nullptr) {
      SSL_CTX_free(ctx_);
    }
    X509_free(cert_.cert);
    EVP_PKEY_free(cert_.key);
  }

  bool IsValid() const
  {
    return listen_fd_ >= 0 && ctx_ != nullptr;
  }
  uint16_t port() const
  {
    return port_;
  }
  const std::string& cert_pem() const
  {
    return cert_.cert_pem;
  }

private:
  void Run()
  {
    for (;;) {
      sockaddr_in client{};
      socklen_t client_len = sizeof(client);
      const int cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client), &client_len);
      if (cfd < 0) {
        return;
      }
      SSL* ssl = SSL_new(ctx_);
      if (ssl != nullptr) {
        SSL_set_fd(ssl, cfd);
        if (SSL_accept(ssl) == 1) {
          char buffer[4096];
          std::string request;
          while (request.find("\r\n\r\n") == std::string::npos) {
            const int n = SSL_read(ssl, buffer, sizeof(buffer));
            if (n <= 0) {
              break;
            }
            request.append(buffer, static_cast<std::size_t>(n));
          }
          const std::size_t ps = request.find(' ');
          const std::size_t pe = request.find(' ', ps + 1);
          const std::string path = ps != std::string::npos && pe != std::string::npos
                                       ? request.substr(ps + 1, pe - ps - 1)
                                       : "/";
          std::string response;
          if (path == "/downgrade") {
            // A https server redirecting to plaintext http (SSL stripping).
            response = "HTTP/1.1 302 Found\r\nLocation: http://example.invalid/\r\n"
                       "Content-Length: 0\r\n\r\n";
          } else {
            // "<h1>Hello TLS</h1>" is 18 bytes; Content-Length must match
            // exactly (a mismatch is a truncated response and the client now
            // rejects it).
            response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 18\r\n"
                       "\r\n<h1>Hello TLS</h1>";
          }
          SSL_write(ssl, response.data(), static_cast<int>(response.size()));
          if (path == "/abrupt-close") {
            // Simulate a CDN that closes the TCP connection WITHOUT a
            // close_notify alert after sending the full response (observed
            // on real sites such as sohu/bing).  The client must accept the
            // complete response even though the TLS stream is truncated.
            SSL_free(ssl);
            ::close(cfd);
            continue;
          }
        }
        SSL_shutdown(ssl);
        SSL_free(ssl);
      }
      ::close(cfd);
    }
  }

  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::thread thread_;
  SSL_CTX* ctx_ = nullptr;
  TestCert cert_;
};
#endif

TEST(HttpTest, ParseBasicResponse)
{
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

TEST(HttpTest, ParseChunkedResponse)
{
  const auto result = ParseHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                        "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().status_code, 200);
  EXPECT_EQ(result.value().body, "hello world");
}

TEST(HttpTest, ParseMalformed)
{
  EXPECT_FALSE(ParseHttpResponse("not http").has_value());
  EXPECT_FALSE(ParseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\nx").has_value());
}

TEST(HttpTest, RejectsOverlongStatusCode)
{
  // A status code longer than the 3-digit form must be rejected instead of
  // overflowing the int accumulation.
  EXPECT_FALSE(ParseHttpResponse("HTTP/1.1 999999999999999999999999999999 OK\r\n\r\n").has_value());
  EXPECT_FALSE(ParseHttpResponse("HTTP/1.1 123456 OK\r\n\r\n").has_value());
}

TEST(HttpTest, RejectsChunkSizeOverflow)
{
  // A chunk size of 2^64 wraps to 0 in size_t arithmetic; the decoder must
  // reject it instead of silently truncating the body.
  const auto result = ParseHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                        "10000000000000000\r\nhello\r\n0\r\n\r\n");
  EXPECT_FALSE(result.has_value());
}

TEST(HttpTest, RejectsChunkSizeWrapsToSmallValue)
{
  // 1 followed by many zeros wraps to a small nonzero value that passes the
  // "chunk fits in remaining data" check; the decoder must still reject it.
  const auto result = ParseHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                        "100000000000000000000000000000000000000000000000000\r\n"
                                        "hello\r\n0\r\n\r\n");
  EXPECT_FALSE(result.has_value());
}

TEST(HttpTest, RejectsContentLengthOverflow)
{
  // A 30-digit Content-Length overflows size_t and wraps to a small value;
  // the decoder must reject it instead of slicing the body at a bogus
  // offset.
  const auto result = ParseHttpResponse(
      "HTTP/1.1 200 OK\r\nContent-Length: 999999999999999999999999999999\r\n\r\nhello");
  EXPECT_FALSE(result.has_value());
}

TEST(HttpTest, RejectsConflictingContentLength)
{
  // Two different Content-Length values make the body boundary ambiguous
  // (a request-smuggling / response-splitting vector); reject (RFC 7230
  // 3.3.3).
  const auto result =
      ParseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 100\r\n\r\nhello");
  EXPECT_FALSE(result.has_value());
}

TEST(HttpTest, AcceptsIdenticalContentLength)
{
  // Duplicate but identical Content-Length values are permitted.
  const auto result =
      ParseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().body, "hello");
}

TEST(HttpTest, RejectsTransferEncodingWithContentLength)
{
  // Content-Length alongside Transfer-Encoding is ambiguous and must be
  // rejected (RFC 7230 3.3.3).
  const auto result =
      ParseHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n"
                        "5\r\nhello\r\n0\r\n\r\n");
  EXPECT_FALSE(result.has_value());
}

TEST(HttpTest, RejectsUnsupportedTransferEncoding)
{
  // A non-chunked Transfer-Encoding must not be silently treated as a plain
  // body.
  const auto result =
      ParseHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n\x1f\x8b\x08\x00");
  EXPECT_FALSE(result.has_value());
}

TEST(HttpTest, UnsupportedSchemeIsNotImplemented)
{
  const auto url = url::Url::Parse("ftp://example.com/file");
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kNotImplemented);
}

#ifndef _WIN32
TEST(TlsTest, GetFromLocalTlsServer)
{
  TestTlsServer server;
  ASSERT_TRUE(server.IsValid());
  TlsOptions options;
  options.extra_ca_cert_pem = server.cert_pem();
  const std::string host = "https://localhost:" + std::to_string(server.port()) + "/";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value(), 5, {}, options);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(result.value().status_code, 200);
  EXPECT_EQ(result.value().body, "<h1>Hello TLS</h1>");
}

TEST(TlsTest, AcceptsCompleteResponseAfterAbruptClose)
{
  // Regression: real CDN-fronted sites (sohu.com, cn.bing.com) close the TCP
  // connection without a TLS close_notify after sending the full response.
  // OpenSSL reports this as "unexpected eof while reading"; the client must
  // still accept the (length-validated) response instead of failing the
  // whole request.  The HTTP layer's Content-Length check is the integrity
  // backstop, so a genuinely truncated body is still rejected.
  TestTlsServer server;
  ASSERT_TRUE(server.IsValid());
  TlsOptions options;
  options.extra_ca_cert_pem = server.cert_pem();
  const std::string host = "https://localhost:" + std::to_string(server.port()) + "/abrupt-close";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value(), 5, {}, options);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(result.value().status_code, 200);
  EXPECT_EQ(result.value().body, "<h1>Hello TLS</h1>");
}

TEST(HttpTest, RejectsTruncatedContentLengthBody)
{
  // A response whose body is shorter than Content-Length must be rejected
  // (truncation): the TLS layer hands back whatever a truncated close
  // delivered, and this check is what stops a truncated response from being
  // accepted silently.
  EXPECT_FALSE(ParseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort").has_value());
}

TEST(HttpTest, RejectsOversizedContentLength)
{
  // A Content-Length above the body limit must be rejected before any
  // allocation (DoS guard); HttpGet must not try to read a 1 GiB body.
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) + "/huge-cl";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().message().find("content-length"), std::string::npos);
}

TEST(HttpTest, RejectsOversizedUnframedBody)
{
  // Without Content-Length or chunked framing, the connection-close path must
  // still enforce the aggregate response body limit.
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) +
                           "/unframed-oversized";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().message(), "response body too large");
}

TEST(TlsTest, HttpsToHttpRedirectIsRefused)
{
  // A https server must not silently follow a redirect to plaintext http
  // (SSL stripping); the request is refused with an error instead.
  TestTlsServer server;
  ASSERT_TRUE(server.IsValid());
  TlsOptions options;
  options.extra_ca_cert_pem = server.cert_pem();
  const std::string host = "https://localhost:" + std::to_string(server.port()) + "/downgrade";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value(), 5, {}, options);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kNotImplemented);
  EXPECT_NE(result.error().message().find("downgrade"), std::string::npos);
}

TEST(TlsTest, UntrustedCertificateIsRejected)
{
  // Without passing the server's self-signed certificate as a trust anchor,
  // verification must fail: the default is to reject untrusted peers.
  TestTlsServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "https://localhost:" + std::to_string(server.port()) + "/";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kNetwork);
}

TEST(TlsTest, HostnameMismatchIsRejected)
{
  // The certificate is valid for "localhost" only; connecting by IP must fail
  // hostname verification even with the trust anchor supplied.
  TestTlsServer server;
  ASSERT_TRUE(server.IsValid());
  TlsOptions options;
  options.extra_ca_cert_pem = server.cert_pem();
  const std::string host = "https://127.0.0.1:" + std::to_string(server.port()) + "/";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value(), 5, {}, options);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kNetwork);
}
#endif

#ifndef _WIN32
TEST(HttpTest, GetFromLocalServer)
{
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

TEST(HttpTest, RedirectFollowed)
{
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) + "/redirect";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().status_code, 200);
  EXPECT_EQ(result.value().body, "<h1>Hello World</h1>");
  // The response must expose the final (post-redirect) URL so callers can
  // resolve relative references against the real document location.
  EXPECT_EQ(result.value().final_url, "http://127.0.0.1:" + std::to_string(server.port()) + "/");
}

TEST(HttpTest, ChunkedFromServer)
{
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

TEST(HttpTest, NotFound)
{
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) + "/missing";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().status_code, 404);
}

TEST(HttpTest, GzipDecodedFromServer)
{
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) + "/gzip";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().status_code, 200);
  EXPECT_EQ(result.value().body, "Hello World");
}

TEST(HttpTest, DeflateDecodedFromServer)
{
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) + "/deflate";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().status_code, 200);
  EXPECT_EQ(result.value().body, "Hello World");
}

TEST(HttpTest, RawDeflateDecodedFromServer)
{
  // Some servers emit a raw (wrapper-less) deflate stream for
  // "Content-Encoding: deflate"; the client must still decode it.
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) + "/raw-deflate";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().body, "Hello World");
}

TEST(HttpTest, DoubleEncodedDecodedFromServer)
{
  // Content-Encoding: gzip, deflate — encodings applied in order, decoded in
  // reverse (RFC 7231 §3.1.2.1).
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) + "/double-encoded";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().body, "Hello World");
}

TEST(HttpTest, BadGzipIsParseError)
{
  // A gzip Content-Encoding whose body is not a valid gzip stream must be an
  // explicit error, never corrupted content.
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) + "/bad-gzip";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kParse);
}

TEST(CompressionTest, GzipRoundTrip)
{
  const std::string body = "the quick brown fox jumps over the lazy dog";
  const std::string compressed = GzipCompress(body);
  ASSERT_FALSE(compressed.empty());
  const auto result = InflateGzip(compressed);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), body);
}

TEST(CompressionTest, DeflateZlibRoundTrip)
{
  const std::string body = std::string(1000, 'x');
  const std::string compressed = ZlibCompress(body);
  ASSERT_FALSE(compressed.empty());
  const auto result = InflateDeflate(compressed);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), body);
}

TEST(CompressionTest, DeflateRawRoundTrip)
{
  const std::string body = std::string(1000, 'y');
  const std::string compressed = RawDeflateCompress(body);
  ASSERT_FALSE(compressed.empty());
  const auto result = InflateDeflate(compressed);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), body);
}

TEST(CompressionTest, GzipRejectsGarbage)
{
  const auto result = InflateGzip("\x1f\x8b\x08\x00garbage");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kParse);
}

TEST(CompressionTest, DecodeIdentityNoop)
{
  const auto result = DecodeContentEncodings("identity", "plain body");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), "plain body");
}

TEST(CompressionTest, DecodeEmptyNoop)
{
  const auto result = DecodeContentEncodings("", "plain body");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), "plain body");
}

TEST(CompressionTest, DecodeChain)
{
  // Header lists codings in application order: gzip first, then deflate.
  // The decoder must undo deflate first, then gzip.
  const std::string body = "nested encoding payload";
  const std::string gzipped = GzipCompress(body);
  const std::string deflated = ZlibCompress(gzipped);
  const auto result = DecodeContentEncodings("gzip, deflate", deflated);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), body);
}

TEST(CompressionTest, DecodeRejectsUnknownCoding)
{
  const auto result = DecodeContentEncodings("br", "something");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kParse);
}

TEST(HttpTest, ExtraHeadersSent)
{
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) + "/";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value(), 5, [](const url::Url&) {
    return std::vector<HttpHeader>{{"cookie", "session=abc123"}};
  });
  ASSERT_TRUE(result.has_value());
  const auto requests = server.Requests();
  ASSERT_EQ(requests.size(), 1u);
  // Header names are emitted verbatim from the provider (lowercase here;
  // HTTP header names are case-insensitive on the wire).
  EXPECT_NE(requests[0].find("cookie: session=abc123"), std::string::npos);
}

TEST(HttpTest, ExtraHeadersRecomputedPerRedirectHop)
{
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) + "/redirect";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  int calls = 0;
  const auto result = HttpGet(url.value(), 5, [&calls](const url::Url& target) {
    ++calls;
    // Cookie scoped to the redirect target host.
    return std::vector<HttpHeader>{{"cookie", "host=" + target.host()}};
  });
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().status_code, 200);
  EXPECT_GE(calls, 2); // initial request + redirect hop
  const auto requests = server.Requests();
  ASSERT_EQ(requests.size(), 2u);
  // The cookie header on the redirected request targets the final host.
  EXPECT_NE(requests[1].find("cookie: host="), std::string::npos);
}

TEST(HttpTest, RequestTargetIsPercentEncoded)
{
  TestHttpServer server;
  ASSERT_TRUE(server.IsValid());
  // Spaces are legal in a parsed path but must be percent-encoded on the
  // wire so they cannot smuggle bytes into the request line.
  const std::string host = "http://127.0.0.1:" + std::to_string(server.port()) + "/a b/c?q=x y";
  const auto url = url::Url::Parse(host);
  ASSERT_TRUE(url.has_value());
  const auto result = HttpGet(url.value());
  ASSERT_TRUE(result.has_value());
  const auto requests = server.Requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_NE(requests[0].find("GET /a%20b/c?q=x%20y HTTP/1.1"), std::string::npos);
}

TEST(SocketTest, ConnectRefused)
{
  // Find a port that is very likely closed by binding and releasing.
  const auto socket = Socket::Connect("127.0.0.1", 1, 500);
  EXPECT_FALSE(socket.has_value());
}
#endif

} // namespace
} // namespace neko::network
