#include "neko/network/socket.h"

#include "neko/network/dns.h"

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

namespace {

// Connects to one numeric address ("1.2.3.4" or "::1") over TCP.  Returns the
// connected descriptor, or -1 with |failures| extended with the reason.
int ConnectToAddress(std::string_view address, uint16_t port, int timeout_ms, std::string* failures)
{
  struct addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* results = nullptr;
  const std::string host_text(address);
  const std::string service = std::to_string(port);
  const int rc = ::getaddrinfo(host_text.c_str(), service.c_str(), &hints, &results);
  if (rc != 0) {
    if (!failures->empty()) {
      *failures += "; ";
    }
    *failures += host_text + ": " + ::gai_strerror(rc);
    return -1;
  }
  int fd = -1;
  for (struct addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
    const std::string family = ai->ai_family == AF_INET ? "IPv4"
                               : ai->ai_family == AF_INET6
                                   ? "IPv6"
                                   : "family " + std::to_string(ai->ai_family);
    const auto record_failure = [failures, &family, &host_text](std::string reason) {
      if (!failures->empty()) {
        *failures += "; ";
      }
      *failures += family + " " + host_text + ": " + std::move(reason);
    };
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
      record_failure("socket: " + std::string(std::strerror(errno)));
      continue;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      record_failure("fcntl: " + std::string(std::strerror(errno)));
      ::close(fd);
      fd = -1;
      continue;
    }
    bool connected = false;
    const int c = ::connect(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
    if (c == 0) {
      connected = true;
    } else if (errno == EINPROGRESS) {
      struct pollfd pfd = {fd, static_cast<short>(POLLOUT), 0};
      const int pr = ::poll(&pfd, 1, timeout_ms);
      if (pr > 0) {
        int so_error = 0;
        socklen_t error_len = sizeof(so_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &error_len) == 0) {
          connected = so_error == 0;
          if (!connected) {
            record_failure("connect: " + std::string(std::strerror(so_error)));
          }
        } else {
          record_failure("getsockopt: " + std::string(std::strerror(errno)));
        }
      } else if (pr == 0) {
        record_failure("connect timed out");
      } else {
        record_failure("poll: " + std::string(std::strerror(errno)));
      }
    } else {
      record_failure("connect: " + std::string(std::strerror(errno)));
    }
    ::fcntl(fd, F_SETFL, flags); // restore blocking mode
    if (connected) {
      break;
    }
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(results);
  return fd;
}

bool LooksNumericHost(std::string_view host)
{
  struct in_addr v4
  {
  };
  struct in6_addr v6
  {
  };
  const std::string text(host);
  return ::inet_pton(AF_INET, text.c_str(), &v4) == 1 ||
         ::inet_pton(AF_INET6, text.c_str(), &v6) == 1;
}

} // namespace

base::Result<Socket> Socket::Connect(std::string_view host, uint16_t port, int timeout_ms)
{
#ifdef _WIN32
  (void)host;
  (void)port;
  (void)timeout_ms;
  return base::Err(base::Error::NotImplemented("Windows sockets are not implemented yet"));
#else
  const std::string host_str(host);
  // Built-in DNS first (project-owned client with TTL caching and /etc/hosts
  // support, see neko/network/dns.h), then getaddrinfo as the fallback so
  // NSS-only setups (mDNS, LDAP, VPN plugins) keep working.  A failed lookup is
  // remembered briefly by the resolver, so the fallback does not pay the DNS
  // timeout on every request.
  const auto dns_addresses = DnsResolver::Default().Resolve(host);
  if (dns_addresses.has_value()) {
    int dns_fd = -1;
    std::string dns_failures;
    for (const std::string& address : dns_addresses.value()) {
      const int candidate = ConnectToAddress(address, port, timeout_ms, &dns_failures);
      if (candidate >= 0) {
        dns_fd = candidate;
        break;
      }
    }
    if (dns_fd >= 0) {
      return Socket(dns_fd);
    }
    if (LooksNumericHost(host)) {
      // A numeric host that does not connect must not fall through to
      // getaddrinfo (it would resolve to the same address anyway).
      return base::Err(base::Error::Network("connect(" + host_str + "): " + dns_failures));
    }
  }

  struct addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  const int rc = ::getaddrinfo(host_str.c_str(), service.c_str(), &hints, &results);
  if (rc != 0) {
    return base::Err(base::Error::Network("getaddrinfo(" + host_str + "): " + ::gai_strerror(rc)));
  }

  int fd = -1;
  std::string failures;
  for (struct addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
    char numeric_host[NI_MAXHOST] = {};
    const int name_rc = ::getnameinfo(ai->ai_addr,
                                      static_cast<socklen_t>(ai->ai_addrlen),
                                      numeric_host,
                                      sizeof(numeric_host),
                                      nullptr,
                                      0,
                                      NI_NUMERICHOST);
    const std::string address = name_rc == 0 ? numeric_host : "unknown address";
    const std::string family = ai->ai_family == AF_INET ? "IPv4"
                               : ai->ai_family == AF_INET6
                                   ? "IPv6"
                                   : "family " + std::to_string(ai->ai_family);
    const auto record_failure = [&failures, &family, &address](std::string reason) {
      if (!failures.empty()) {
        failures += "; ";
      }
      failures += family + " " + address + ": " + std::move(reason);
    };
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
      record_failure("socket: " + std::string(std::strerror(errno)));
      continue;
    }
    // Non-blocking connect so the caller's timeout applies.
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      record_failure("fcntl: " + std::string(std::strerror(errno)));
      ::close(fd);
      fd = -1;
      continue;
    }
    bool connected = false;
    const int c = ::connect(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
    if (c == 0) {
      connected = true;
    } else if (errno == EINPROGRESS) {
      struct pollfd pfd = {fd, static_cast<short>(POLLOUT), 0};
      const int pr = ::poll(&pfd, 1, timeout_ms);
      if (pr > 0) {
        int so_error = 0;
        socklen_t error_len = sizeof(so_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &error_len) == 0) {
          connected = (so_error == 0);
          if (!connected) {
            record_failure("connect: " + std::string(std::strerror(so_error)));
          }
        } else {
          record_failure("getsockopt: " + std::string(std::strerror(errno)));
        }
      } else if (pr == 0) {
        record_failure("connect timed out");
      } else {
        record_failure("poll: " + std::string(std::strerror(errno)));
      }
    } else {
      record_failure("connect: " + std::string(std::strerror(errno)));
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
    std::string message = "connect failed for " + host_str + ":" + service;
    if (!failures.empty()) {
      message += " (attempts: " + failures + ")";
    }
    return base::Err(base::Error::Network(std::move(message)));
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
    struct pollfd pfd = {fd_, static_cast<short>(POLLIN), 0};
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

base::Result<Socket::ReceiveOutcome> Socket::ReceiveWithOutcome(std::size_t max_bytes,
                                                                int timeout_ms)
{
#ifdef _WIN32
  (void)max_bytes;
  (void)timeout_ms;
  return base::Err(base::Error::NotImplemented("Windows sockets are not implemented yet"));
#else
  ReceiveOutcome outcome;
  char buffer[16384];
  while (outcome.data.size() < max_bytes) {
    struct pollfd pfd = {fd_, static_cast<short>(POLLIN), 0};
    const int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr == 0) {
      outcome.timed_out = true;
      return outcome;
    }
    if (pr < 0) {
      if (errno == EINTR) {
        continue;
      }
      return base::Err(base::Error::Network("poll failed"));
    }
    const std::size_t want = std::min<std::size_t>(max_bytes - outcome.data.size(), sizeof(buffer));
    const ssize_t n = ::recv(fd_, buffer, want, 0);
    if (n == 0) {
      return outcome; // EOF (timed_out stays false)
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return base::Err(base::Error::Network("recv failed"));
    }
    outcome.data.append(buffer, static_cast<std::size_t>(n));
  }
  return outcome;
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
