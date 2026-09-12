#pragma once

#include "neko/base/status.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace neko::network {

// A DNS client (RFC 1035) with a TTL cache and /etc/hosts support.
//
// Why a project-owned resolver: the engine must control lookup order, caching
// and timeouts (browsers do the same with their built-in resolver), and the
// lookup must be testable without the system resolver.  Socket::Connect uses
// this resolver first and falls back to getaddrinfo, so NSS-only setups
// (mDNS, LDAP, ...) keep working.
//
// Threading: Resolve() is thread-safe (the cache is mutex-guarded); each call
// performs its own UDP transactions, so callers on the browser's fetch pool
// never share socket state.
//
// Documented limitations: A/AAAA and CNAME records only (no SRV/TXT/MX), no
// DNSSEC, no EDNS0, no TCP fallback for truncated answers, and the resolver
// list comes from /etc/resolv.conf (no per-interface configuration).
class DnsResolver
{
public:
  struct Options
  {
    // Resolver addresses, in order; when empty the resolver list is parsed from
    // |resolv_conf_path| and falls back to the well-known public resolvers.
    std::vector<std::string> servers;
    std::string resolv_conf_path = "/etc/resolv.conf";
    std::string hosts_path = "/etc/hosts";
    int port = 53;
    int timeout_ms = 2000;
    int attempts = 2; // per server
    std::size_t max_cache_entries = 512;
    // TTL clamp: a server TTL of 0 is not cached (may still be returned).
    std::chrono::seconds min_ttl{1};
    std::chrono::seconds max_ttl{3600};
    // How long a failed lookup is remembered (Socket::Connect uses this to fall
    // back to the system resolver without repeating the DNS timeout).
    std::chrono::seconds negative_ttl{10};
  };

  // The process-wide resolver (default options).
  static DnsResolver& Default();

  // GCC rejects `Options = {}` as a default argument (the nested aggregate has
  // default member initializers), hence the explicit two-constructor form.
  DnsResolver();
  explicit DnsResolver(Options options);

  // Resolves |host| to IPv4 and IPv6 address strings (in answer order,
  // CNAME chains followed).  A numeric host is returned unchanged.  Errors:
  // Error::Network for DNS failures (with the server's rcode name), timeouts
  // and invalid responses; InvalidArgument for empty hosts.
  base::Result<std::vector<std::string>> Resolve(std::string_view host);

  // Resolves through the cache only (no network); empty when nothing valid is
  // cached.  Used by tests and by callers that need a non-blocking peek.
  std::vector<std::string> LookupCached(std::string_view host) const;

  // Clears the cache (used by tests and when the network changes).
  void ClearCache();
  std::size_t cache_size() const;

  // Test seam: replaces the clock used for TTL bookkeeping.
  void SetClockForTesting(std::function<std::chrono::steady_clock::time_point()> clock);

  const Options& options() const
  {
    return options_;
  }

private:
  struct CacheEntry
  {
    std::vector<std::string> addresses;
    std::chrono::steady_clock::time_point expires;
    // A recent failure (no records / no reachable server): keeps callers from
    // paying the timeout again on every request.
    bool negative = false;
    std::string message;
  };

  std::vector<std::string> ResolveUncached(std::string_view host, base::Error* error);
  std::vector<std::string> HostsFileLookup(std::string_view host) const;
  std::vector<std::string> ServersInOrder() const;

  Options options_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, CacheEntry> cache_;
  std::function<std::chrono::steady_clock::time_point()> clock_;
  // Parsed once: the resolver list from resolv.conf (empty when absent).
  mutable std::vector<std::string> resolv_conf_servers_;
  mutable bool resolv_conf_parsed_ = false;
  mutable std::mutex resolv_conf_mutex_;
};

// Encodes a DNS query for |host| (QTYPE A or AAAA) with |id|.  Exposed for
// tests (the wire format is the part worth checking byte by byte).
std::string EncodeDnsQuery(std::uint16_t id, std::string_view host, std::uint16_t qtype);

// The result of parsing a DNS response message.
struct DnsAnswer
{
  std::uint16_t id = 0;
  bool is_response = false;
  int rcode = 0; // 0 = no error, 3 = NXDOMAIN, 2 = SERVFAIL, ...
  bool truncated = false;
  std::vector<std::string> ipv4;  // A records, in answer order
  std::vector<std::string> ipv6;  // AAAA records
  std::vector<std::string> cname; // CNAME targets, in answer order
  std::uint32_t min_ttl = 0;
};

// Parses |message| (bytes) into |out|.  Returns false when the message is
// malformed (short header, bad name, out-of-range pointer, ...).
bool ParseDnsResponse(std::string_view message, DnsAnswer* out);

// The human-readable name of a DNS response code.
std::string_view DnsRcodeName(int rcode);

} // namespace neko::network
