#include "neko/network/socket.h"

#include "neko/network/dns.h"

#include "socket_platform.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

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
  platform::CloseSocket(fd_);
  fd_ = -1;
}

namespace {

// Appends |reason| to the "; "-separated failure list built while trying the
// resolved addresses.
void AppendFailure(std::string* failures, std::string reason)
{
  if (!failures->empty()) {
    *failures += "; ";
  }
  *failures += std::move(reason);
}

std::string DescribeFamily(int family)
{
  if (family == AF_INET) {
    return "IPv4";
  }
  if (family == AF_INET6) {
    return "IPv6";
  }
  return "family " + std::to_string(family);
}

// The numeric text of a resolved address, for error messages.
std::string DescribeAddress(const struct sockaddr* address)
{
  if (address->sa_family == AF_INET) {
    const auto* v4 = reinterpret_cast<const struct sockaddr_in*>(address);
    std::uint8_t bytes[4] = {};
    std::memcpy(bytes, &v4->sin_addr, sizeof(bytes));
    return platform::FormatIpv4(bytes);
  }
  if (address->sa_family == AF_INET6) {
    const auto* v6 = reinterpret_cast<const struct sockaddr_in6*>(address);
    std::uint8_t bytes[16] = {};
    std::memcpy(bytes, &v6->sin6_addr, sizeof(bytes));
    return platform::FormatIpv6(bytes);
  }
  return "unknown address";
}

// Connects |fd| to one resolved address, applying |timeout_ms| to the
// non-blocking connect and restoring blocking mode afterwards.  Returns true
// when connected; otherwise |failure| describes the reason.
bool ConnectWithTimeout(int fd,
                        const struct addrinfo& resolved,
                        int timeout_ms,
                        std::string* failure)
{
  if (!platform::SetNonBlocking(fd, true)) {
    *failure = "set non-blocking mode: " + platform::ErrorText(platform::LastError());
    return false;
  }
  bool connected = false;
  if (platform::ConnectSocket(
          fd, resolved.ai_addr, static_cast<platform::SockLen>(resolved.ai_addrlen)) == 0) {
    connected = true;
  } else {
    const int error = platform::LastError();
    if (platform::IsInProgress(error)) {
      const int ready =
          platform::WaitSocket(fd, /*want_read=*/false, /*want_write=*/true, timeout_ms);
      if (ready > 0) {
        const int pending = platform::PendingSocketError(fd);
        if (pending < 0) {
          *failure = "getsockopt: " + platform::ErrorText(platform::LastError());
        } else if (pending != 0) {
          *failure = "connect: " + platform::ErrorText(pending);
        } else {
          connected = true;
        }
      } else if (ready == 0) {
        *failure = "connect timed out";
      } else {
        *failure = "wait: " + platform::ErrorText(platform::LastError());
      }
    } else {
      *failure = "connect: " + platform::ErrorText(error);
    }
  }
  (void)platform::SetNonBlocking(fd, false); // restore blocking mode
  return connected;
}

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
    AppendFailure(failures, host_text + ": " + platform::GaiErrorText(rc));
    return -1;
  }
  int fd = -1;
  for (struct addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
    const std::string family = DescribeFamily(ai->ai_family);
    const int candidate = platform::CreateSocket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (candidate < 0) {
      AppendFailure(failures,
                    family + " " + host_text +
                        ": socket: " + platform::ErrorText(platform::LastError()));
      continue;
    }
    std::string failure;
    if (ConnectWithTimeout(candidate, *ai, timeout_ms, &failure)) {
      fd = candidate;
      break;
    }
    AppendFailure(failures, family + " " + host_text + ": " + failure);
    platform::CloseSocket(candidate);
  }
  ::freeaddrinfo(results);
  return fd;
}

bool LooksNumericHost(std::string_view host)
{
  std::uint8_t bytes[16] = {};
  return platform::ParseIpv4(host, bytes) || platform::ParseIpv6(host, bytes);
}

} // namespace

base::Result<Socket> Socket::Connect(std::string_view host, uint16_t port, int timeout_ms)
{
  if (!platform::EnsureInitialized()) {
    return base::Err(base::Error::Network("cannot initialize the platform socket stack"));
  }
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
    const std::string address = DescribeAddress(ai->ai_addr);
    const std::string family = DescribeFamily(ai->ai_family);
    const auto record_failure = [&failures, &family, &address](std::string reason) {
      AppendFailure(&failures, family + " " + address + ": " + std::move(reason));
    };
    const int candidate = platform::CreateSocket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (candidate < 0) {
      record_failure("socket: " + platform::ErrorText(platform::LastError()));
      continue;
    }
    std::string failure;
    if (ConnectWithTimeout(candidate, *ai, timeout_ms, &failure)) {
      fd = candidate;
      break;
    }
    record_failure(std::move(failure));
    platform::CloseSocket(candidate);
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
}

base::Result<std::size_t> Socket::Send(std::string_view data)
{
  if (!platform::EnsureInitialized()) {
    return base::Err(base::Error::Network("cannot initialize the platform socket stack"));
  }
  std::size_t sent = 0;
  while (sent < data.size()) {
    const int n = platform::SendBytes(fd_, data.data() + sent, data.size() - sent);
    if (n <= 0) {
      if (n < 0 && platform::IsInterrupted(platform::LastError())) {
        continue;
      }
      return base::Err(base::Error::Network("send failed"));
    }
    sent += static_cast<std::size_t>(n);
  }
  return sent;
}

base::Result<std::string> Socket::Receive(std::size_t max_bytes, int timeout_ms)
{
  std::string out;
  char buffer[16384];
  while (out.size() < max_bytes) {
    const int ready =
        platform::WaitSocket(fd_, /*want_read=*/true, /*want_write=*/false, timeout_ms);
    if (ready == 0) {
      return out; // timeout: hand back what we have
    }
    if (ready < 0) {
      if (platform::IsInterrupted(platform::LastError())) {
        continue;
      }
      return base::Err(base::Error::Network("wait for readable failed"));
    }
    const std::size_t want = std::min<std::size_t>(max_bytes - out.size(), sizeof(buffer));
    const int n = platform::ReceiveBytes(fd_, buffer, want);
    if (n == 0) {
      return out; // EOF
    }
    if (n < 0) {
      if (platform::IsInterrupted(platform::LastError())) {
        continue;
      }
      return base::Err(base::Error::Network("recv failed"));
    }
    out.append(buffer, static_cast<std::size_t>(n));
  }
  return out;
}

base::Result<Socket::ReceiveOutcome> Socket::ReceiveWithOutcome(std::size_t max_bytes,
                                                                int timeout_ms)
{
  ReceiveOutcome outcome;
  char buffer[16384];
  while (outcome.data.size() < max_bytes) {
    const int ready =
        platform::WaitSocket(fd_, /*want_read=*/true, /*want_write=*/false, timeout_ms);
    if (ready == 0) {
      outcome.timed_out = true;
      return outcome;
    }
    if (ready < 0) {
      if (platform::IsInterrupted(platform::LastError())) {
        continue;
      }
      return base::Err(base::Error::Network("wait for readable failed"));
    }
    const std::size_t want = std::min<std::size_t>(max_bytes - outcome.data.size(), sizeof(buffer));
    const int n = platform::ReceiveBytes(fd_, buffer, want);
    if (n == 0) {
      return outcome; // EOF (timed_out stays false)
    }
    if (n < 0) {
      if (platform::IsInterrupted(platform::LastError())) {
        continue;
      }
      return base::Err(base::Error::Network("recv failed"));
    }
    outcome.data.append(buffer, static_cast<std::size_t>(n));
  }
  return outcome;
}

base::Result<std::string> Socket::ReceiveAll(int timeout_ms)
{
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
}

} // namespace neko::network
