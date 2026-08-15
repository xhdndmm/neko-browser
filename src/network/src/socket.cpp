#include "neko/network/socket.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <winsock2.h>
#endif

namespace neko::network {

Socket::Socket() : fd_(-1) {}

Socket::Socket(int fd) : fd_(fd) {}

Socket::~Socket()
{
  Close();
}

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_)
{
  other.fd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept
{
  if (this != &other) {
    Close();
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

void Socket::Close()
{
#ifdef _WIN32
  if (fd_ >= 0) {
    closesocket(fd_);
    fd_ = -1;
  }
#else
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
#endif
}

base::Result<Socket> Socket::Connect(std::string_view host, uint16_t port, int timeout_ms)
{
#ifdef _WIN32
  (void)host;
  (void)port;
  (void)timeout_ms;
  return base::Err(base::Error::NotImplemented("Windows sockets are not implemented yet"));
#else
  struct addrinfo hints
  {
  };
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* results = nullptr;
  const std::string host_str(host);
  const std::string service = std::to_string(port);
  const int rc = ::getaddrinfo(host_str.c_str(), service.c_str(), &hints, &results);
  if (rc != 0) {
    return base::Err(base::Error::Network("getaddrinfo(" + host_str + "): " + ::gai_strerror(rc)));
  }

  int fd = -1;
  for (struct addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
      continue;
    }
    // Non-blocking connect so the caller's timeout applies.
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    bool connected = false;
    const int c = ::connect(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
    if (c == 0) {
      connected = true;
    } else if (errno == EINPROGRESS) {
      struct pollfd pfd
      {
        fd, static_cast<short>(POLLOUT), 0
      };
      const int pr = ::poll(&pfd, 1, timeout_ms);
      if (pr > 0) {
        int so_error = 0;
        socklen_t error_len = sizeof(so_error);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &error_len);
        connected = (so_error == 0);
      }
    }
    ::fcntl(fd, F_SETFL, flags); // restore blocking mode
    if (connected) {
      break;
    }
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(results);
  if (fd < 0) {
    return base::Err(base::Error::Network("connect failed for " + host_str + ":" + service));
  }
  return Socket(fd);
#endif
}

base::Result<std::size_t> Socket::Send(std::string_view data)
{
#ifdef _WIN32
  (void)data;
  return base::Err(base::Error::NotImplemented("Windows sockets are not implemented yet"));
#else
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return base::Err(base::Error::Network("send failed"));
    }
    sent += static_cast<std::size_t>(n);
  }
  return sent;
#endif
}

base::Result<std::string> Socket::Receive(std::size_t max_bytes, int timeout_ms)
{
#ifdef _WIN32
  (void)max_bytes;
  (void)timeout_ms;
  return base::Err(base::Error::NotImplemented("Windows sockets are not implemented yet"));
#else
  std::string out;
  char buffer[16384];
  while (out.size() < max_bytes) {
    struct pollfd pfd
    {
      fd_, static_cast<short>(POLLIN), 0
    };
    const int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr == 0) {
      return out; // timeout: hand back what we have
    }
    if (pr < 0) {
      if (errno == EINTR) {
        continue;
      }
      return base::Err(base::Error::Network("poll failed"));
    }
    const std::size_t want = std::min<std::size_t>(max_bytes - out.size(), sizeof(buffer));
    const ssize_t n = ::recv(fd_, buffer, want, 0);
    if (n == 0) {
      return out; // EOF
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return base::Err(base::Error::Network("recv failed"));
    }
    out.append(buffer, static_cast<std::size_t>(n));
  }
  return out;
#endif
}

base::Result<std::string> Socket::ReceiveAll(int timeout_ms)
{
#ifdef _WIN32
  (void)timeout_ms;
  return base::Err(base::Error::NotImplemented("Windows sockets are not implemented yet"));
#else
  std::string out;
  for (;;) {
    const base::Result<std::string> chunk = Receive(16384, timeout_ms);
    if (!chunk) {
      return chunk;
    }
    if (chunk.value().empty()) {
      return out; // EOF or timeout
    }
    out += chunk.value();
  }
#endif
}

} // namespace neko::network
