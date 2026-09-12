// DNS client implementation (RFC 1035 message format + RFC 3596 AAAA).
//
// The wire format is implemented by hand (no resolver library) so the engine
// controls caching, timeouts and the lookup order, and so the parser can be
// fuzzed like every other parser in the project.  Everything here treats the
// response bytes as untrusted input: every read is bounds-checked, name
// compression pointers are followed with a visited-set and a depth bound.

#include "neko/network/dns.h"

#include "neko/base/logging.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace neko::network {
namespace {

constexpr std::uint16_t kTypeA = 1;
constexpr std::uint16_t kTypeCNAME = 5;
constexpr std::uint16_t kTypeAAAA = 28;
constexpr std::uint16_t kClassIN = 1;
// A name in a DNS message may not be longer than 255 bytes (§3.1).
constexpr std::size_t kMaxNameLength = 255;
// Compression chains are bounded to keep a malicious message from spinning.
constexpr int kMaxCompressionHops = 16;

std::uint16_t ReadU16(std::string_view data, std::size_t offset)
{
  return static_cast<std::uint16_t>((static_cast<std::uint8_t>(data[offset]) << 8) |
                                    static_cast<std::uint8_t>(data[offset + 1]));
}

std::uint32_t ReadU32(std::string_view data, std::size_t offset)
{
  return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset])) << 24) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 1])) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 2])) << 8) |
         static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 3]));
}

void AppendU16(std::string& out, std::uint16_t value)
{
  out.push_back(static_cast<char>((value >> 8) & 0xff));
  out.push_back(static_cast<char>(value & 0xff));
}

// Reads a (possibly compressed) name starting at |offset|.  |offset| is
// advanced past the name as it appears in the message (a compression pointer
// ends it); the returned name is dotted and lowercased.
bool ReadName(std::string_view message, std::size_t* offset, std::string* out)
{
  out->clear();
  std::size_t pos = *offset;
  std::size_t after_name = 0;
  bool jumped = false;
  int hops = 0;
  while (true) {
    if (pos >= message.size()) {
      return false;
    }
    const auto length = static_cast<std::uint8_t>(message[pos]);
    if (length == 0) {
      ++pos;
      break;
    }
    if ((length & 0xc0) == 0xc0) {
      if (pos + 1 >= message.size()) {
        return false;
      }
      const std::size_t target = (static_cast<std::size_t>(length & 0x3f) << 8) |
                                 static_cast<std::uint8_t>(message[pos + 1]);
      if (!jumped) {
        after_name = pos + 2;
        jumped = true;
      }
      if (target >= message.size() || ++hops > kMaxCompressionHops) {
        return false;
      }
      pos = target;
      continue;
    }
    if ((length & 0xc0) != 0) {
      return false; // reserved label type
    }
    if (pos + 1 + length > message.size()) {
      return false;
    }
    if (!out->empty()) {
      out->push_back('.');
    }
    if (out->size() + length > kMaxNameLength) {
      return false;
    }
    for (std::size_t i = 0; i < length; ++i) {
      out->push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(message[pos + 1 + i]))));
    }
    pos += 1 + length;
  }
  *offset = jumped ? after_name : pos;
  return true;
}

} // namespace

std::string EncodeDnsQuery(std::uint16_t id, std::string_view host, std::uint16_t qtype)
{
  std::string out;
  // Header: id, flags (standard query, recursion desired), qdcount=1.
  AppendU16(out, id);
  AppendU16(out, 0x0100);
  AppendU16(out, 1);
  AppendU16(out, 0);
  AppendU16(out, 0);
  AppendU16(out, 0);
  // QNAME.
  std::size_t start = 0;
  while (start <= host.size()) {
    const std::size_t dot = host.find('.', start);
    const std::string_view label =
        host.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
    if (!label.empty()) {
      if (label.size() > 63) {
        return {}; // a label may not exceed 63 octets
      }
      out.push_back(static_cast<char>(label.size()));
      out.append(label);
    }
    if (dot == std::string_view::npos) {
      break;
    }
    start = dot + 1;
  }
  out.push_back('\0');
  AppendU16(out, qtype);
  AppendU16(out, kClassIN);
  return out;
}

std::string_view DnsRcodeName(int rcode)
{
  switch (rcode) {
  case 0:
    return "NOERROR";
  case 1:
    return "FORMERR";
  case 2:
    return "SERVFAIL";
  case 3:
    return "NXDOMAIN";
  case 4:
    return "NOTIMP";
  case 5:
    return "REFUSED";
  default:
    return "RCODE?";
  }
}

bool ParseDnsResponse(std::string_view message, DnsAnswer* out)
{
  if (message.size() < 12) {
    return false;
  }
  out->id = ReadU16(message, 0);
  const std::uint16_t flags = ReadU16(message, 2);
  out->is_response = (flags & 0x8000) != 0;
  out->truncated = (flags & 0x0200) != 0;
  out->rcode = flags & 0x000f;
  const std::uint16_t qdcount = ReadU16(message, 4);
  const std::uint16_t ancount = ReadU16(message, 6);
  const std::uint16_t nscount = ReadU16(message, 8);
  const std::uint16_t arcount = ReadU16(message, 10);

  std::size_t offset = 12;
  for (std::uint16_t i = 0; i < qdcount; ++i) {
    std::string name;
    if (!ReadName(message, &offset, &name) || offset + 4 > message.size()) {
      return false;
    }
    offset += 4; // QTYPE + QCLASS
  }

  // Answer records plus the authority/additional sections (skipped, but their
  // records must be walked so a malformed message is still rejected).
  const std::uint32_t record_count = ancount + nscount + arcount;
  bool first_ttl = true;
  for (std::uint32_t i = 0; i < record_count; ++i) {
    std::string name;
    if (!ReadName(message, &offset, &name)) {
      return false;
    }
    if (offset + 10 > message.size()) {
      return false;
    }
    const std::uint16_t type = ReadU16(message, offset);
    const std::uint16_t rr_class = ReadU16(message, offset + 2);
    const std::uint32_t ttl = ReadU32(message, offset + 4);
    const std::uint16_t rdlength = ReadU16(message, offset + 8);
    offset += 10;
    if (offset + rdlength > message.size()) {
      return false;
    }
    const std::string_view rdata = message.substr(offset, rdlength);
    const bool in_answer = i < ancount;
    if (in_answer && rr_class == kClassIN) {
      if (first_ttl || ttl < out->min_ttl) {
        out->min_ttl = ttl;
        first_ttl = false;
      }
      if (type == kTypeA && rdlength == 4) {
        char text[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, rdata.data(), text, sizeof(text)) != nullptr) {
          out->ipv4.emplace_back(text);
        }
      } else if (type == kTypeAAAA && rdlength == 16) {
        char text[INET6_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET6, rdata.data(), text, sizeof(text)) != nullptr) {
          out->ipv6.emplace_back(text);
        }
      } else if (type == kTypeCNAME) {
        std::size_t cname_offset = offset;
        std::string target;
        if (!ReadName(message, &cname_offset, &target)) {
          return false;
        }
        out->cname.push_back(std::move(target));
      }
    }
    offset += rdlength;
  }
  return true;
}

// ---------------------------------------------------------------------------
// DnsResolver
// ---------------------------------------------------------------------------

namespace {

#ifdef _WIN32
base::Result<std::vector<std::string>>
QueryServer(const DnsResolver::Options&, std::string_view, std::uint16_t, std::uint16_t, DnsAnswer*)
{
  return base::Err(base::Error::NotImplemented("DNS over UDP is not implemented on Windows yet"));
}
#else
// Sends one query to |server| and returns the parsed answer.  The socket is
// connected to the server, so the kernel filters datagrams from other peers
// (plus the id and question checks in the caller).
base::Result<DnsAnswer> QueryServer(const DnsResolver::Options& options,
                                    std::string_view server,
                                    std::string_view host,
                                    std::uint16_t qtype,
                                    std::uint16_t id)
{
  const std::string query = EncodeDnsQuery(id, host, qtype);
  if (query.empty()) {
    return base::Err(base::Error::InvalidArgument("DNS query could not be encoded"));
  }
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return base::Err(base::Error::Io("DNS: socket() failed"));
  }
  struct CloseFd
  {
    int fd;
    ~CloseFd()
    {
      ::close(fd);
    }
  } closer{fd};

  struct sockaddr_in address
  {
  };
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<std::uint16_t>(options.port));
  if (::inet_pton(AF_INET, std::string(server).c_str(), &address.sin_addr) != 1) {
    return base::Err(base::Error::InvalidArgument("DNS: invalid resolver address '" +
                                                  std::string(server) + "'"));
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    return base::Err(base::Error::Io("DNS: connect() to " + std::string(server) + " failed"));
  }
  // Bound each attempt with a receive timeout.
  struct timeval timeout
  {
  };
  timeout.tv_sec = options.timeout_ms / 1000;
  timeout.tv_usec = (options.timeout_ms % 1000) * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  const ssize_t sent = ::send(fd, query.data(), query.size(), 0);
  if (sent != static_cast<ssize_t>(query.size())) {
    return base::Err(base::Error::Io("DNS: send() failed"));
  }

  std::array<char, 4096> buffer{};
  const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
  if (received <= 0) {
    return base::Err(base::Error::Network("DNS: no response from " + std::string(server)));
  }
  DnsAnswer answer;
  if (!ParseDnsResponse(std::string_view(buffer.data(), static_cast<std::size_t>(received)),
                        &answer)) {
    return base::Err(base::Error::Parse("DNS: malformed response from " + std::string(server)));
  }
  return base::Ok(std::move(answer));
}
#endif

bool LooksNumeric(std::string_view host)
{
  if (host.find(':') != std::string_view::npos) {
    struct in6_addr v6
    {
    };
    return ::inet_pton(AF_INET6, std::string(host).c_str(), &v6) == 1;
  }
  struct in_addr v4
  {
  };
  return ::inet_pton(AF_INET, std::string(host).c_str(), &v4) == 1;
}

std::string LowerAscii(std::string_view text)
{
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

} // namespace

DnsResolver::DnsResolver() = default;

DnsResolver::DnsResolver(Options options) : options_(std::move(options)) {}

DnsResolver& DnsResolver::Default()
{
  static DnsResolver resolver;
  return resolver;
}

void DnsResolver::SetClockForTesting(std::function<std::chrono::steady_clock::time_point()> clock)
{
  std::lock_guard<std::mutex> lock(mutex_);
  clock_ = std::move(clock);
}

std::vector<std::string> DnsResolver::LookupCached(std::string_view host) const
{
  const auto now = clock_ ? clock_() : std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = cache_.find(LowerAscii(host));
  if (it == cache_.end() || it->second.expires <= now) {
    return {};
  }
  return it->second.addresses;
}

void DnsResolver::ClearCache()
{
  std::lock_guard<std::mutex> lock(mutex_);
  cache_.clear();
}

std::size_t DnsResolver::cache_size() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return cache_.size();
}

std::vector<std::string> DnsResolver::ServersInOrder() const
{
  if (!options_.servers.empty()) {
    return options_.servers;
  }
  {
    std::lock_guard<std::mutex> lock(resolv_conf_mutex_);
    if (!resolv_conf_parsed_) {
      resolv_conf_parsed_ = true;
      std::ifstream input(options_.resolv_conf_path);
      std::string line;
      while (std::getline(input, line)) {
        std::istringstream stream(line);
        std::string keyword;
        std::string value;
        stream >> keyword >> value;
        if (keyword == "nameserver" && !value.empty()) {
          resolv_conf_servers_.push_back(value);
        }
      }
    }
  }
  if (!resolv_conf_servers_.empty()) {
    return resolv_conf_servers_;
  }
  // No resolv.conf (or no nameserver lines): the well-known public resolvers
  // keep the engine working in minimal containers.
  return {"1.1.1.1", "8.8.8.8"};
}

std::vector<std::string> DnsResolver::HostsFileLookup(std::string_view host) const
{
  std::ifstream input(options_.hosts_path);
  if (!input) {
    return {};
  }
  const std::string wanted = LowerAscii(host);
  std::string line;
  while (std::getline(input, line)) {
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line = line.substr(0, comment);
    }
    std::istringstream stream(line);
    std::string address;
    if (!(stream >> address)) {
      continue;
    }
    std::string name;
    while (stream >> name) {
      if (LowerAscii(name) == wanted) {
        return {address};
      }
    }
  }
  return {};
}

std::vector<std::string> DnsResolver::ResolveUncached(std::string_view host, base::Error* error)
{
  if (LooksNumeric(host)) {
    return {std::string(host)};
  }
  std::vector<std::string> from_hosts = HostsFileLookup(host);
  if (!from_hosts.empty()) {
    return from_hosts;
  }

  // Random query id per attempt: a response is only accepted when it echoes it.
  std::random_device device;
  std::uniform_int_distribution<int> distribution(0, 0xffff);
  const std::uint16_t id = static_cast<std::uint16_t>(distribution(device));

  std::vector<std::string> addresses;
  std::string last_error;
  std::uint32_t min_ttl = 0;
  bool first = true;
  std::string current_host = LowerAscii(host);

  // CNAME chains are followed up to a small bound (a loop must not hang).
  for (int hop = 0; hop < 8; ++hop) {
    DnsAnswer answer;
    bool resolved = false;
    bool answered = false;
    for (const std::string& server : ServersInOrder()) {
      for (int attempt = 0; attempt < std::max(1, options_.attempts); ++attempt) {
        auto query = QueryServer(
            options_, server, current_host, kTypeA, static_cast<std::uint16_t>(id + hop));
        if (!query.has_value()) {
          last_error = query.error().message();
          continue;
        }
        if (query.value().id != static_cast<std::uint16_t>(id + hop)) {
          last_error = "DNS: response id mismatch (ignored)";
          continue;
        }
        if (!query.value().is_response) {
          last_error = "DNS: response bit missing (ignored)";
          continue;
        }
        answered = true;
        answer = std::move(query.value());
        break;
      }
      if (answered) {
        break;
      }
    }
    if (!answered) {
      if (error != nullptr) {
        *error =
            base::Error::Network(last_error.empty() ? "DNS: no resolver reachable" : last_error);
      }
      return {};
    }
    if (answer.rcode != 0) {
      if (error != nullptr) {
        *error = base::Error::Network("DNS: " + std::string(DnsRcodeName(answer.rcode)) + " for " +
                                      current_host);
      }
      // Remember the failure (see the negative-cache note below).
      const auto now = clock_ ? clock_() : std::chrono::steady_clock::now();
      std::lock_guard<std::mutex> lock(mutex_);
      if (cache_.size() < options_.max_cache_entries) {
        cache_[LowerAscii(host)] = CacheEntry{{},
                                              now + options_.negative_ttl,
                                              true,
                                              error != nullptr ? error->message() : std::string()};
      }
      return {};
    }
    if (first) {
      min_ttl = answer.min_ttl;
      first = false;
    } else {
      min_ttl = std::min(min_ttl, answer.min_ttl);
    }
    addresses = answer.ipv4;
    if (!answer.ipv6.empty()) {
      addresses.insert(addresses.end(), answer.ipv6.begin(), answer.ipv6.end());
    }
    if (!addresses.empty()) {
      resolved = true;
    }
    if (resolved) {
      break;
    }
    if (answer.cname.empty()) {
      break;
    }
    current_host = answer.cname.front();
  }

  // One AAAA query for the resolved name: IPv6 addresses are offered after the
  // IPv4 ones, so a dual-stack host still prefers IPv4 (no happy-eyeballs
  // racing yet — documented).
  if (!addresses.empty()) {
    for (const std::string& server : ServersInOrder()) {
      auto v6 = QueryServer(
          options_, server, current_host, kTypeAAAA, static_cast<std::uint16_t>(id + 100));
      if (v6.has_value() && v6.value().id == static_cast<std::uint16_t>(id + 100) &&
          v6.value().is_response && v6.value().rcode == 0 && !v6.value().ipv6.empty()) {
        addresses.insert(addresses.end(), v6.value().ipv6.begin(), v6.value().ipv6.end());
        break;
      }
    }
  }
  if (addresses.empty() && error != nullptr) {
    *error = base::Error::Network("DNS: no address records for " + std::string(host));
  }

  // Cache with the clamped TTL (0 disables caching, per the option comment).
  const auto now = clock_ ? clock_() : std::chrono::steady_clock::now();
  if (!addresses.empty() && min_ttl > 0) {
    const auto ttl = std::chrono::seconds(
        std::clamp<std::int64_t>(min_ttl, options_.min_ttl.count(), options_.max_ttl.count()));
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_.size() >= options_.max_cache_entries) {
      cache_.clear(); // simple policy: drop everything rather than grow
    }
    cache_[LowerAscii(host)] = CacheEntry{addresses, now + ttl, false, {}};
  } else if (addresses.empty()) {
    // Remember the failure briefly so the caller can fall back cheaply.
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_.size() < options_.max_cache_entries) {
      cache_[LowerAscii(host)] = CacheEntry{{},
                                            now + options_.negative_ttl,
                                            true,
                                            error != nullptr ? error->message() : std::string()};
    }
  }
  return addresses;
}

base::Result<std::vector<std::string>> DnsResolver::Resolve(std::string_view host)
{
  if (host.empty()) {
    return base::Err(base::Error::InvalidArgument("DNS: empty host name"));
  }
  const std::string key = LowerAscii(host);
  {
    const auto now = clock_ ? clock_() : std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = cache_.find(key);
    if (it != cache_.end()) {
      if (it->second.expires > now) {
        if (it->second.negative) {
          return base::Err(base::Error::Network(it->second.message.empty()
                                                    ? "DNS: recent lookup failure for " + key
                                                    : it->second.message));
        }
        return base::Ok(it->second.addresses);
      }
      cache_.erase(it);
    }
  }
  base::Error error;
  std::vector<std::string> addresses = ResolveUncached(host, &error);
  if (addresses.empty()) {
    return base::Err(error);
  }
  return base::Ok(std::move(addresses));
}

} // namespace neko::network
