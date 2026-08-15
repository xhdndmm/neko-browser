// TLS client socket wrapping OpenSSL behind the neko::network interface.

#include "neko/network/tls_socket.h"

#include "neko/network/socket.h"

#include <cerrno>
#include <cstring>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <string>
#include <string_view>
#include <utility>

#ifndef _WIN32
#include <poll.h>
#include <sys/socket.h>
#else
#include <winsock2.h>
#endif

namespace neko::network {
namespace {

using SslCtxPtr = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using SslPtr = std::unique_ptr<SSL, decltype(&SSL_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;

// Collects and drains the OpenSSL error queue into a readable string.
std::string SslErrorString()
{
  std::string msg;
  unsigned long code = 0;
  while ((code = ERR_get_error()) != 0) {
    if (!msg.empty()) {
      msg += "; ";
    }
    msg += ERR_error_string(code, nullptr);
  }
  return msg.empty() ? "unknown TLS error" : msg;
}

// Bounds the socket's blocking operations (used by the TLS BIO during the
// handshake and by SSL_read/SSL_write) so a stalled peer cannot hang the
// caller beyond |timeout_ms|.
base::Result<void> SetSocketTimeouts(int fd, int timeout_ms)
{
#ifdef _WIN32
  DWORD tv = static_cast<DWORD>(timeout_ms);
  if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv)) !=
          0 ||
      ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv)) !=
          0) {
    return base::Err(base::Error::Network("cannot set socket timeouts"));
  }
#else
  struct timeval tv
  {
  };
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
      ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
    return base::Err(base::Error::Network("cannot set socket timeouts"));
  }
#endif
  return base::Error();
}

} // namespace

struct TlsSocket::Impl
{
  Socket tcp;    // owns the TCP fd (closed on destruction)
  SslCtxPtr ctx; // owns the SSL_CTX
  SslPtr ssl;    // borrows the fd from |tcp|
  int timeout_ms = 10000;

  Impl() : ctx(nullptr, SSL_CTX_free), ssl(nullptr, SSL_free) {}
};

TlsSocket::TlsSocket() : impl_(std::make_unique<Impl>()) {}

TlsSocket::~TlsSocket()
{
  Close();
}

TlsSocket::TlsSocket(TlsSocket&& other) noexcept = default;

TlsSocket& TlsSocket::operator=(TlsSocket&& other) noexcept = default;

base::Result<TlsSocket>
TlsSocket::Connect(std::string_view host, uint16_t port, const TlsOptions& options)
{
  TlsSocket socket;
  socket.impl_->timeout_ms = options.timeout_ms;

  // Plain TCP first; the TLS layer rides on top of this fd.
  base::Result<Socket> tcp = Socket::Connect(host, port, options.timeout_ms);
  if (!tcp) {
    return base::Err(tcp.error());
  }
  socket.impl_->tcp = std::move(tcp.value());

  const base::Result<void> timeouts = SetSocketTimeouts(socket.impl_->tcp.fd(), options.timeout_ms);
  if (!timeouts) {
    return base::Err(timeouts.error());
  }

  SslCtxPtr ctx(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
  if (!ctx) {
    return base::Err(base::Error::Network("TLS: cannot create context: " + SslErrorString()));
  }
  // Require TLS >= 1.2 (rejects SSLv3 / TLS 1.0 / TLS 1.1).
  SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
  // Verify the peer against the system trust store.
  SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
  if (SSL_CTX_set_default_verify_paths(ctx.get()) != 1) {
    return base::Err(base::Error::Network("TLS: no system trust store: " + SslErrorString()));
  }
  if (!options.extra_ca_cert_pem.empty()) {
    BioPtr bio(BIO_new_mem_buf(options.extra_ca_cert_pem.data(),
                               static_cast<int>(options.extra_ca_cert_pem.size())),
               BIO_free);
    if (!bio) {
      return base::Err(base::Error::Network("TLS: cannot load extra CA"));
    }
    X509* cert = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr);
    if (cert == nullptr) {
      return base::Err(
          base::Error::Network("TLS: invalid extra CA certificate: " + SslErrorString()));
    }
    X509Ptr cert_owner(cert, X509_free);
    X509_STORE* store = SSL_CTX_get_cert_store(ctx.get());
    if (store == nullptr || X509_STORE_add_cert(store, cert) != 1) {
      return base::Err(
          base::Error::Network("TLS: cannot add extra CA certificate: " + SslErrorString()));
    }
  }

  SslPtr ssl(SSL_new(ctx.get()), SSL_free);
  if (!ssl) {
    return base::Err(base::Error::Network("TLS: cannot create session: " + SslErrorString()));
  }
  const std::string host_str(host);
  // SNI and hostname verification (both must use the requested hostname).
  // SSL_set_tlsext_host_name is a macro containing an old-style cast, so call
  // the underlying SSL_ctrl directly to keep the strict warning set clean.
  if (SSL_ctrl(ssl.get(),
               SSL_CTRL_SET_TLSEXT_HOSTNAME,
               TLSEXT_NAMETYPE_host_name,
               const_cast<char*>(host_str.c_str())) != 1) {
    return base::Err(base::Error::Network("TLS: cannot set SNI"));
  }
  if (SSL_set1_host(ssl.get(), host_str.c_str()) != 1) {
    return base::Err(base::Error::Network("TLS: cannot set hostname check"));
  }
  if (SSL_set_fd(ssl.get(), socket.impl_->tcp.fd()) != 1) {
    return base::Err(base::Error::Network("TLS: cannot attach socket"));
  }

  const int hs = SSL_connect(ssl.get());
  if (hs != 1) {
    const int err = SSL_get_error(ssl.get(), hs);
    if (err == SSL_ERROR_SSL) {
      // The most common failure is certificate verification; surface the
      // OpenSSL message (e.g. "certificate verify failed").
      return base::Err(base::Error::Network("TLS handshake / certificate verification failed for " +
                                            host_str + ": " + SslErrorString()));
    }
    return base::Err(
        base::Error::Network("TLS handshake failed for " + host_str + ": " + SslErrorString()));
  }

  socket.impl_->ctx = std::move(ctx);
  socket.impl_->ssl = std::move(ssl);
  return socket;
}

base::Result<std::size_t> TlsSocket::Send(std::string_view data)
{
  if (impl_->ssl == nullptr) {
    return base::Err(base::Error::Network("TLS: not connected"));
  }
  std::size_t sent = 0;
  while (sent < data.size()) {
    const int n =
        SSL_write(impl_->ssl.get(), data.data() + sent, static_cast<int>(data.size() - sent));
    if (n <= 0) {
      const int err = SSL_get_error(impl_->ssl.get(), n);
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        continue; // blocking socket with timeouts: retry
      }
      return base::Err(base::Error::Network("TLS send failed: " + SslErrorString()));
    }
    sent += static_cast<std::size_t>(n);
  }
  return sent;
}

base::Result<std::string> TlsSocket::Receive(std::size_t max_bytes, int timeout_ms)
{
  if (impl_->ssl == nullptr) {
    return base::Err(base::Error::Network("TLS: not connected"));
  }
  std::string out;
  char buffer[16384];
  while (out.size() < max_bytes) {
    // Hand back any data that is already buffered inside the TLS layer
    // before waiting on the socket.
    if (SSL_pending(impl_->ssl.get()) <= 0) {
#ifdef _WIN32
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(impl_->tcp.fd(), &readfds);
      timeval tv{};
      tv.tv_sec = timeout_ms / 1000;
      tv.tv_usec = (timeout_ms % 1000) * 1000;
      const int pr = ::select(impl_->tcp.fd() + 1, &readfds, nullptr, nullptr, &tv);
#else
      struct pollfd pfd
      {
        impl_->tcp.fd(), static_cast<short>(POLLIN), 0
      };
      const int pr = ::poll(&pfd, 1, timeout_ms);
#endif
      if (pr == 0) {
        return out; // timeout: hand back what we have
      }
      if (pr < 0) {
        if (errno == EINTR) {
          continue;
        }
        return base::Err(base::Error::Network("TLS poll failed"));
      }
    }
    const std::size_t want = std::min<std::size_t>(max_bytes - out.size(), sizeof(buffer));
    const int n = SSL_read(impl_->ssl.get(), buffer, static_cast<int>(want));
    if (n > 0) {
      out.append(buffer, static_cast<std::size_t>(n));
      continue;
    }
    const int err = SSL_get_error(impl_->ssl.get(), n);
    if (err == SSL_ERROR_ZERO_RETURN) {
      return out; // clean TLS shutdown
    }
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
      continue;
    }
    if (err == SSL_ERROR_SSL || (err == SSL_ERROR_SYSCALL && n == 0)) {
      // The peer closed the TCP connection without a complete close_notify
      // (RFC 8448 §6.1).  Some real servers (CDN-fronted sites) do this
      // after sending the full response.  Hand back what we have; the HTTP
      // layer validates the message framing (Content-Length / chunked
      // terminator), so a genuinely truncated response is still rejected
      // there rather than accepted silently.
      return out;
    }
    return base::Err(base::Error::Network("TLS read failed: " + SslErrorString()));
  }
  return out;
}

base::Result<std::string> TlsSocket::ReceiveAll(int timeout_ms)
{
  if (impl_->ssl == nullptr) {
    return base::Err(base::Error::Network("TLS: not connected"));
  }
  std::string out;
  for (;;) {
    const base::Result<std::string> chunk = Receive(16384, timeout_ms);
    if (!chunk) {
      return chunk;
    }
    if (chunk.value().empty()) {
      return out; // clean close, truncated close or timeout
    }
    out += chunk.value();
  }
}

void TlsSocket::Close()
{
  if (impl_ == nullptr) {
    return;
  }
  if (impl_->ssl != nullptr) {
    // Best-effort close_notify; the socket is closed right after anyway.
    SSL_shutdown(impl_->ssl.get());
  }
  impl_->ssl.reset();
  impl_->ctx.reset();
  impl_->tcp.Close();
}

bool TlsSocket::IsOpen() const
{
  return impl_ != nullptr && impl_->ssl != nullptr;
}

} // namespace neko::network
