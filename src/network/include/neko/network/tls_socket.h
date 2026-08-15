#pragma once

#include "neko/base/status.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace neko::network {

// Optional TLS connection settings.
struct TlsOptions
{
  // PEM certificate of an additional trust anchor.  Certificate verification
  // is always on; this only adds a CA to the trust store (used by tests with
  // a locally-generated CA).  Empty by default.
  std::string extra_ca_cert_pem;

  // Bounds the TCP connect, the TLS handshake and each read.
  int timeout_ms = 10000;
};

// A TLS (SSL/TLS) client socket over a TCP connection, wrapping OpenSSL
// behind this project-owned interface.  Performs the TLS handshake with SNI,
// and verifies the server certificate against the system trust store plus any
// |extra_ca_cert_pem| anchor, including hostname verification.
//
// TlsSocket is move-only, like Socket.  It is NOT thread-safe: one connection
// per object, used from one thread.
class TlsSocket
{
public:
  TlsSocket();
  ~TlsSocket();

  TlsSocket(TlsSocket&& other) noexcept;
  TlsSocket& operator=(TlsSocket&& other) noexcept;
  TlsSocket(const TlsSocket&) = delete;
  TlsSocket& operator=(const TlsSocket&) = delete;

  // Resolves |host|, connects a TCP socket and performs a TLS handshake.
  // |host| is also used for SNI and hostname verification.  Returns a Network
  // error when the handshake or certificate verification fails.
  static base::Result<TlsSocket>
  Connect(std::string_view host, uint16_t port, const TlsOptions& options = {});

  base::Result<std::size_t> Send(std::string_view data);
  // Reads up to |max_bytes| bytes, returning whatever is available without
  // waiting for a clean TLS close.  An empty result means the peer closed or
  // the timeout elapsed before any byte arrived.  A peer that closes the TCP
  // connection without a close_notify alert is treated as EOF here: the
  // completeness of the HTTP message is validated by the HTTP layer (via
  // Content-Length / the chunked terminator), which keeps a truncated-stream
  // attack from being accepted silently.
  base::Result<std::string> Receive(std::size_t max_bytes, int timeout_ms = 10000);
  // Reads until a clean TLS close, EOF or |timeout_ms|, returning everything
  // received so far.
  base::Result<std::string> ReceiveAll(int timeout_ms = 10000);

  void Close();
  bool IsOpen() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace neko::network
