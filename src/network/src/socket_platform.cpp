// Platform socket shim implementation (see socket_platform.h).

#include "socket_platform.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <system_error>

#ifndef _WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace neko::network::platform {

#ifdef _WIN32
namespace {

// Winsock handles are kernel handle values; they fit in an int in practice
// (which is also the type OpenSSL's BIO_new_socket uses for them).  The int
// descriptor stored by Socket is converted back here.
SOCKET ToSocket(int fd)
{
  return static_cast<SOCKET>(static_cast<unsigned int>(fd));
}

} // namespace
#endif

bool EnsureInitialized()
{
#ifdef _WIN32
  // Function-local static: initialized once, thread-safely, on first use.
  static const bool initialized = [] {
    WSADATA data = {};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
#else
  return true;
#endif
}

void CloseSocket(int fd)
{
  if (fd < 0) {
    return;
  }
#ifdef _WIN32
  ::closesocket(ToSocket(fd));
#else
  ::close(fd);
#endif
}

int CreateSocket(int family, int type, int protocol)
{
  if (!EnsureInitialized()) {
    return -1;
  }
#ifdef _WIN32
  const SOCKET handle = ::socket(family, type, protocol);
  if (handle == INVALID_SOCKET) {
    return -1;
  }
  return static_cast<int>(handle);
#else
  return ::socket(family, type, protocol);
#endif
}

bool SetNonBlocking(int fd, bool enabled)
{
#ifdef _WIN32
  u_long mode = enabled ? 1UL : 0UL;
  return ::ioctlsocket(ToSocket(fd), FIONBIO, &mode) == 0;
#else
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  const int updated = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  return ::fcntl(fd, F_SETFL, updated) == 0;
#endif
}

bool SetSocketTimeouts(int fd, int timeout_ms)
{
#ifdef _WIN32
  // Winsock reads a DWORD (milliseconds) from the option buffer.
  const std::uint32_t milliseconds = static_cast<std::uint32_t>(timeout_ms);
  const auto* value = reinterpret_cast<const char*>(&milliseconds);
  return ::setsockopt(ToSocket(fd), SOL_SOCKET, SO_RCVTIMEO, value, sizeof(milliseconds)) == 0 &&
         ::setsockopt(ToSocket(fd), SOL_SOCKET, SO_SNDTIMEO, value, sizeof(milliseconds)) == 0;
#else
  struct timeval timeout = {};
  timeout.tv_sec = static_cast<time_t>(timeout_ms / 1000);
  timeout.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
  return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
         ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
#endif
}

int ConnectSocket(int fd, const struct sockaddr* address, SockLen length)
{
  if (!EnsureInitialized()) {
    return -1;
  }
#ifdef _WIN32
  return ::connect(ToSocket(fd), address, static_cast<int>(length));
#else
  return ::connect(fd, address, length);
#endif
}

int WaitSocket(int fd, bool want_read, bool want_write, int timeout_ms)
{
  if (!EnsureInitialized()) {
    return -1;
  }
#ifdef _WIN32
  // select() is used instead of WSAPoll: WSAPoll has well-known defects for
  // failed connections on Windows versions before 10 2004, while select has
  // been reliable since the first Winsock release.
  fd_set read_set;
  fd_set write_set;
  FD_ZERO(&read_set);
  FD_ZERO(&write_set);
  if (want_read) {
    FD_SET(ToSocket(fd), &read_set);
  }
  if (want_write) {
    FD_SET(ToSocket(fd), &write_set);
  }
  struct timeval timeout = {};
  struct timeval* timeout_ptr = nullptr;
  if (timeout_ms >= 0) {
    timeout.tv_sec = static_cast<long>(timeout_ms / 1000);
    timeout.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
    timeout_ptr = &timeout;
  }
  fd_set* read_ptr = want_read ? &read_set : nullptr;
  fd_set* write_ptr = want_write ? &write_set : nullptr;
  return ::select(0, read_ptr, write_ptr, nullptr, timeout_ptr);
#else
  struct pollfd pfd = {};
  pfd.fd = fd;
  pfd.events = static_cast<short>((want_read ? POLLIN : 0) | (want_write ? POLLOUT : 0));
  return ::poll(&pfd, 1, timeout_ms);
#endif
}

int PendingSocketError(int fd)
{
#ifdef _WIN32
  const SOCKET raw = ToSocket(fd);
#else
  const int raw = fd;
#endif
  int error = 0;
  SockLen length = static_cast<SockLen>(sizeof(error));
  if (::getsockopt(raw, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0) {
    return -1;
  }
  return error;
}

int SendBytes(int fd, const char* data, std::size_t length)
{
  if (!EnsureInitialized()) {
    return -1;
  }
  // Winsock takes an int length; the callers send in bounded chunks, so the
  // clamp only matters for a >2 GiB string_view.
  const int bounded =
      static_cast<int>(std::min<std::size_t>(length, static_cast<std::size_t>(INT_MAX)));
#ifdef _WIN32
  return ::send(ToSocket(fd), data, bounded, 0);
#else
  return static_cast<int>(::send(fd, data, static_cast<std::size_t>(bounded), 0));
#endif
}

int ReceiveBytes(int fd, char* data, std::size_t length)
{
  if (!EnsureInitialized()) {
    return -1;
  }
  const int bounded =
      static_cast<int>(std::min<std::size_t>(length, static_cast<std::size_t>(INT_MAX)));
#ifdef _WIN32
  return ::recv(ToSocket(fd), data, bounded, 0);
#else
  return static_cast<int>(::recv(fd, data, static_cast<std::size_t>(bounded), 0));
#endif
}

int LastError()
{
#ifdef _WIN32
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

bool IsInProgress(int error)
{
#ifdef _WIN32
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
  return error == EINPROGRESS;
#endif
}

bool IsInterrupted(int error)
{
#ifdef _WIN32
  return error == WSAEINTR;
#else
  return error == EINTR;
#endif
}

std::string ErrorText(int error)
{
#ifdef _WIN32
  // WSA codes are not in the kernel's message table, so FormatMessage (what
  // std::system_category uses) often has no text for them; fall back to the
  // numeric code, which is what documentation and search engines key on.
  const std::string text = std::system_category().message(error);
  if (text.empty() || text.find("Unknown error") != std::string::npos) {
    return "socket error " + std::to_string(error);
  }
  return text;
#else
  return std::strerror(error);
#endif
}

std::string GaiErrorText(int error)
{
#ifdef _WIN32
  return ::gai_strerrorA(error);
#else
  return ::gai_strerror(error);
#endif
}

bool ParseIpv4(std::string_view text, std::uint8_t out[4])
{
  const std::string address(text);
#ifdef _WIN32
  return ::InetPtonA(AF_INET, address.c_str(), out) == 1;
#else
  return ::inet_pton(AF_INET, address.c_str(), out) == 1;
#endif
}

bool ParseIpv6(std::string_view text, std::uint8_t out[16])
{
  const std::string address(text);
#ifdef _WIN32
  return ::InetPtonA(AF_INET6, address.c_str(), out) == 1;
#else
  return ::inet_pton(AF_INET6, address.c_str(), out) == 1;
#endif
}

std::string FormatIpv4(const std::uint8_t bytes[4])
{
  char text[INET_ADDRSTRLEN] = {};
#ifdef _WIN32
  const bool converted = ::InetNtopA(AF_INET, bytes, text, sizeof(text)) != nullptr;
#else
  const bool converted = ::inet_ntop(AF_INET, bytes, text, sizeof(text)) != nullptr;
#endif
  return converted ? std::string(text) : std::string();
}

std::string FormatIpv6(const std::uint8_t bytes[16])
{
  char text[INET6_ADDRSTRLEN] = {};
#ifdef _WIN32
  const bool converted = ::InetNtopA(AF_INET6, bytes, text, sizeof(text)) != nullptr;
#else
  const bool converted = ::inet_ntop(AF_INET6, bytes, text, sizeof(text)) != nullptr;
#endif
  return converted ? std::string(text) : std::string();
}

} // namespace neko::network::platform
