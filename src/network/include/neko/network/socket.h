#pragma once

#include "neko/base/status.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace neko::network {

// A blocking TCP socket.
//
// POSIX (Linux/macOS) and Windows (Winsock) are both supported: everything
// that differs between the two lives in the module's internal platform shim
// (src/socket_platform.h), not in this interface.  Socket is move-only;
// closing happens on destruction.
class Socket
{
public:
  Socket();
  ~Socket();

  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  // Resolves |host| and connects.  |timeout_ms| bounds the connect and each
  // read.
  static base::Result<Socket> Connect(std::string_view host, uint16_t port, int timeout_ms = 10000);

  base::Result<std::size_t> Send(std::string_view data);
  // Reads up to |max_bytes| bytes, returning whatever is available without
  // waiting for the peer to close.  An empty result means EOF or timeout
  // before any byte arrived; a shorter-than-requested result means the peer
  // closed or the timeout elapsed mid-read.  I/O errors are returned as Err.
  // The HTTP layer uses this to read exactly as many body bytes as the
  // response framing requires instead of waiting for a clean close.
  //
  // Use ReceiveWithOutcome when the caller must tell "timeout" apart from
  // "peer closed" (polling protocols such as WebSocket).
  base::Result<std::string> Receive(std::size_t max_bytes, int timeout_ms = 10000);

  // The outcome of a receive attempt, distinguishing a deadline from EOF.
  struct ReceiveOutcome
  {
    std::string data;       // bytes received (possibly empty)
    bool timed_out = false; // deadline elapsed with no new byte available
  };
  // Like Receive, but reports whether the empty/short result came from the
  // timeout rather than the peer closing.  EOF and I/O errors keep
  // timed_out = false.
  base::Result<ReceiveOutcome> ReceiveWithOutcome(std::size_t max_bytes, int timeout_ms = 10000);
  // Reads until EOF or timeout, returning everything received.
  base::Result<std::string> ReceiveAll(int timeout_ms = 10000);

  // The underlying file descriptor (owned by this socket).  Used to attach a
  // TLS layer; the caller must not close or reuse it.
  int fd() const
  {
    return fd_;
  }

  void Close();
  bool IsOpen() const
  {
    return fd_ >= 0;
  }

private:
  explicit Socket(int fd);

  int fd_;
};

} // namespace neko::network
