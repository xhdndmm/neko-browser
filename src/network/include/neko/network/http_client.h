#pragma once

#include "neko/network/http.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace neko::network {

// A keep-alive HTTP/1.1 client.
//
// One client owns a pool of *idle* connections, keyed by origin (scheme +
// host + port + the TLS/trust scope a connection was dialed under).  A
// request takes a connection from the pool when one is still healthy, and
// the connection goes back to the pool when the response framing says the
// connection is still usable (Content-Length or chunked body, and no
// `Connection: close`).
//
// Why this exists: without reuse every resource pays a full TCP connect plus
// TLS handshake.  On a real page (bilibili.com: 183 subresources) that was
// ~0.3-0.5 s per request and the dominant cost of the load (see ADR 0022).
//
// Threading contract:
//   * Get() may be called from any thread; the pool is mutex-guarded.
//   * A pooled connection is leased to exactly one caller at a time, so the
//     single-thread-per-connection rule of Socket / TlsSocket holds.
//   * The mutex is never held across network I/O.
//
// Failure handling: a *reused* connection may have been closed by the peer
// while idle.  Such a connection is dropped before use (pending-input probe)
// and, if it dies anyway before a single response byte arrived, the request
// is retried once on a fresh connection.  GET is idempotent, so the retry is
// safe and invisible to the caller.
class HttpClient
{
public:
  // Counters for tests and diagnostics.
  struct Stats
  {
    std::size_t requests = 0;              // Get() calls that reached the wire
    std::size_t connections_opened = 0;    // TCP+TLS dials
    std::size_t connections_reused = 0;    // requests served on a pooled connection
    std::size_t connections_discarded = 0; // reused/returned connections closed without reuse
    std::size_t bytes = 0;                 // decoded response bytes delivered to callers
  };

  // Idle connections kept per origin; further returned connections are closed.
  static constexpr std::size_t kMaxIdlePerOrigin = 8;
  // An idle connection older than this is closed instead of reused.
  static constexpr int kIdleTimeoutMs = 60000;

  HttpClient();
  ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  // Same contract as HttpGet(), minus the process-wide pooling.
  base::Result<HttpResponse> Get(const url::Url& url,
                                 int redirect_limit = kDefaultRedirectLimit,
                                 const HeaderProvider& extra_headers = {},
                                 const TlsOptions& tls_options = {});

  Stats stats() const;

  // Closes every idle connection (a clean shutdown hook; also lets tests drop
  // state instead of relying on port reuse).
  void CloseIdleConnections();

private:
  // One request/response exchange, without redirect handling.
  base::Result<HttpResponse>
  GetOnce(const url::Url& url, const HeaderProvider& extra_headers, const TlsOptions& tls_options);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The process-wide client used by HttpGet(): every existing caller
// (stylesheets, scripts, images, fonts, fetch/XHR) gets connection reuse
// without threading a client through the engine.  Embedders that need an
// isolated pool construct their own HttpClient.
HttpClient& SharedHttpClient();

} // namespace neko::network
