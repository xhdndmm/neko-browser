// DNS client tests: wire format, a local UDP DNS server (A/AAAA/CNAME,
// compression pointers, rcodes, malformed responses), the TTL cache with an
// injected clock, negative caching, the hosts file and DnsResolver::Resolve
// integration.

#include "neko/network/dns.h"
#include "neko/network/socket.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace neko::network {
namespace {

#ifndef _WIN32

// A minimal DNS server answering from a table of canned messages.  The handler
// receives the raw query and fills |response|; returning false drops the query
// (the client then times out).
class TestDnsServer
{
public:
  using Handler = std::function<bool(std::string_view query, std::string* response)>;

  explicit TestDnsServer(Handler handler) : handler_(std::move(handler))
  {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
      return;
    }
    struct sockaddr_in address
    {
    };
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      ::close(fd_);
      fd_ = -1;
      return;
    }
    socklen_t length = sizeof(address);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      ::close(fd_);
      fd_ = -1;
      return;
    }
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([this] { Run(); });
  }

  ~TestDnsServer()
  {
    stop_ = true;
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  bool valid() const
  {
    return fd_ >= 0;
  }
  uint16_t port() const
  {
    return port_;
  }
  int queries() const
  {
    return queries_.load();
  }

private:
  void Run()
  {
    while (!stop_) {
      struct timeval timeout
      {
      };
      timeout.tv_usec = 20000; // 20 ms so stop_ is noticed promptly
      ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      char buffer[2048];
      struct sockaddr_in from
      {
      };
      socklen_t from_length = sizeof(from);
      const ssize_t received = ::recvfrom(
          fd_, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &from_length);
      if (received <= 0) {
        continue;
      }
      ++queries_;
      last_query_.assign(buffer, static_cast<std::size_t>(received));
      std::string response;
      if (!handler_(last_query_, &response) || response.empty()) {
        continue;
      }
      ::sendto(fd_,
               response.data(),
               response.size(),
               0,
               reinterpret_cast<sockaddr*>(&from),
               from_length);
    }
  }

  int fd_ = -1;
  uint16_t port_ = 0;
  Handler handler_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<int> queries_{0};
  std::string last_query_;
};

std::string NameAt(std::string_view message, std::size_t offset)
{
  // The query's QNAME sits right after the 12-byte header.
  std::string name;
  std::size_t pos = offset;
  while (pos < message.size() && message[pos] != '\0') {
    const auto length = static_cast<std::size_t>(static_cast<std::uint8_t>(message[pos]));
    if (pos + 1 + length > message.size()) {
      break;
    }
    if (!name.empty()) {
      name.push_back('.');
    }
    name.append(message.substr(pos + 1, length));
    pos += 1 + length;
  }
  return name;
}

std::uint16_t QueryId(std::string_view message)
{
  return static_cast<std::uint16_t>((static_cast<std::uint8_t>(message[0]) << 8) |
                                    static_cast<std::uint8_t>(message[1]));
}

std::uint16_t QueryType(std::string_view message)
{
  // QTYPE is the two bytes after the QNAME + terminator.
  std::size_t pos = 12;
  while (pos < message.size() && message[pos] != '\0') {
    pos += 1 + static_cast<std::size_t>(static_cast<std::uint8_t>(message[pos]));
  }
  pos += 1;
  if (pos + 2 > message.size()) {
    return 0;
  }
  return static_cast<std::uint16_t>((static_cast<std::uint8_t>(message[pos]) << 8) |
                                    static_cast<std::uint8_t>(message[pos + 1]));
}

void AppendU16(std::string& out, std::uint16_t value)
{
  out.push_back(static_cast<char>((value >> 8) & 0xff));
  out.push_back(static_cast<char>(value & 0xff));
}

void AppendU32(std::string& out, std::uint32_t value)
{
  out.push_back(static_cast<char>((value >> 24) & 0xff));
  out.push_back(static_cast<char>((value >> 16) & 0xff));
  out.push_back(static_cast<char>((value >> 8) & 0xff));
  out.push_back(static_cast<char>(value & 0xff));
}

void AppendName(std::string& out, std::string_view name)
{
  std::size_t start = 0;
  while (start < name.size()) {
    const std::size_t dot = name.find('.', start);
    const std::string_view label =
        name.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
    out.push_back(static_cast<char>(label.size()));
    out.append(label);
    if (dot == std::string_view::npos) {
      break;
    }
    start = dot + 1;
  }
  out.push_back('\0');
}

// Builds a response echoing the query's id/question, with the given answers
// (the answer name uses a compression pointer back to the question, which the
// parser must follow).
std::string MakeResponse(std::string_view query,
                         std::uint16_t answer_type,
                         std::string_view answer_data,
                         std::uint32_t ttl,
                         int rcode = 0,
                         std::string_view cname = {})
{
  std::string out;
  AppendU16(out, QueryId(query));
  AppendU16(out, static_cast<std::uint16_t>(0x8180 | (rcode & 0x000f))); // response + RD + RA
  AppendU16(out, 1);                                                     // qdcount
  const std::uint16_t answers = cname.empty() ? 1 : 2;
  AppendU16(out, answers);
  AppendU16(out, 0);
  AppendU16(out, 0);
  // Question: copy the QNAME/QTYPE/QCLASS verbatim.
  std::size_t pos = 12;
  while (pos < query.size() && query[pos] != '\0') {
    pos += 1 + static_cast<std::size_t>(static_cast<std::uint8_t>(query[pos]));
  }
  pos += 1 + 4;
  out.append(query.substr(12, pos - 12));
  if (!cname.empty()) {
    out.push_back(static_cast<char>(0xc0));
    out.push_back(0x0c); // pointer to the question name
    AppendU16(out, 5);   // CNAME
    AppendU16(out, 1);
    AppendU32(out, ttl);
    std::string target;
    AppendName(target, cname);
    AppendU16(out, static_cast<std::uint16_t>(target.size()));
    out.append(target);
  }
  out.push_back(static_cast<char>(0xc0));
  out.push_back(0x0c);
  AppendU16(out, answer_type);
  AppendU16(out, 1);
  AppendU32(out, ttl);
  AppendU16(out, static_cast<std::uint16_t>(answer_data.size()));
  out.append(answer_data);
  return out;
}

const char kIp1[] = {'\x5d', '\xb8', '\xd8', '\x22'}; // 93.184.216.34
const char kIp2[] = {'\x0a', '\x00', '\x00', '\x01'}; // 10.0.0.1

#endif // !_WIN32

std::string TempFile(const std::string& name, const std::string& content)
{
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("neko-dns-test-" + name);
  std::ofstream output(path);
  output << content;
  output.close();
  return path.string();
}

// ---------------------------------------------------------------------------
// Wire format
// ---------------------------------------------------------------------------

TEST(DnsWireTest, QueryEncodingFollowsRfc1035)
{
  const std::string query = EncodeDnsQuery(0x1234, "www.example.test", 1);
  ASSERT_GE(query.size(), 12u + 18u);
  EXPECT_EQ(static_cast<std::uint8_t>(query[0]), 0x12);
  EXPECT_EQ(static_cast<std::uint8_t>(query[1]), 0x34);
  EXPECT_EQ(static_cast<std::uint8_t>(query[2]), 0x01); // recursion desired
  EXPECT_EQ(static_cast<std::uint8_t>(query[3]), 0x00);
  EXPECT_EQ(static_cast<std::uint8_t>(query[4]), 0x00); // qdcount high byte
  EXPECT_EQ(static_cast<std::uint8_t>(query[5]), 0x01); // one question
  EXPECT_EQ(static_cast<std::uint8_t>(query[12]), 3);   // "www"
  EXPECT_EQ(query.substr(13, 3), "www");
  EXPECT_EQ(static_cast<std::uint8_t>(query[16]), 7); // "example"
  EXPECT_EQ(query.substr(17, 7), "example");
  EXPECT_EQ(query[query.size() - 1], 1); // QCLASS IN low byte
  EXPECT_EQ(static_cast<std::uint8_t>(query[query.size() - 4]), 0);
  EXPECT_EQ(static_cast<std::uint8_t>(query[query.size() - 3]), 1); // QTYPE A
  EXPECT_EQ(static_cast<std::uint8_t>(query[query.size() - 2]), 0);
  EXPECT_EQ(static_cast<std::uint8_t>(query[query.size() - 1]), 1); // QCLASS IN
  // A label longer than 63 octets cannot be encoded.
  EXPECT_TRUE(EncodeDnsQuery(1, std::string(64, 'a') + ".test", 1).empty());
}

TEST(DnsWireTest, ParsesAnswersAndCompressedNames)
{
  const std::string query = EncodeDnsQuery(0x2020, "www.example.test", 1);
  const std::string ip(kIp1, 4);
  const std::string response = MakeResponse(query, 1, ip, 300);
  DnsAnswer answer;
  ASSERT_TRUE(ParseDnsResponse(response, &answer));
  EXPECT_EQ(answer.id, 0x2020);
  EXPECT_TRUE(answer.is_response);
  EXPECT_EQ(answer.rcode, 0);
  ASSERT_EQ(answer.ipv4.size(), 1u);
  EXPECT_EQ(answer.ipv4[0], "93.184.216.34");
  EXPECT_EQ(answer.min_ttl, 300u);

  // A CNAME answer plus the final A record, with a compression pointer.
  const std::string chained = MakeResponse(query, 1, ip, 60, 0, "cdn.example.test");
  DnsAnswer cname_answer;
  ASSERT_TRUE(ParseDnsResponse(chained, &cname_answer));
  ASSERT_EQ(cname_answer.cname.size(), 1u);
  EXPECT_EQ(cname_answer.cname[0], "cdn.example.test");
  ASSERT_EQ(cname_answer.ipv4.size(), 1u);
  EXPECT_EQ(cname_answer.min_ttl, 60u);

  // An AAAA answer (16 bytes) lands in |ipv6|.
  const std::string v6_bytes = std::string("\x20\x01\x0d\xb8", 4) + std::string(12, '\0');
  const std::string v6_response = MakeResponse(
      query,
      28,
      std::string("\x20\x01\x0d\xb8\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01", 16),
      30);
  DnsAnswer v6_answer;
  ASSERT_TRUE(ParseDnsResponse(v6_response, &v6_answer));
  ASSERT_EQ(v6_answer.ipv6.size(), 1u);
  EXPECT_EQ(v6_answer.ipv6[0], "2001:db8::1");

  // RCODEs are surfaced.
  DnsAnswer nxdomain;
  ASSERT_TRUE(ParseDnsResponse(MakeResponse(query, 1, "", 0, 3), &nxdomain));
  EXPECT_EQ(nxdomain.rcode, 3);
  EXPECT_EQ(DnsRcodeName(nxdomain.rcode), "NXDOMAIN");
}

TEST(DnsWireTest, RejectsMalformedMessages)
{
  DnsAnswer answer;
  EXPECT_FALSE(ParseDnsResponse("", &answer));
  EXPECT_FALSE(ParseDnsResponse(std::string(11, '\0'), &answer));
  // A response with an out-of-range compression pointer must be rejected.
  std::string message = MakeResponse(EncodeDnsQuery(7, "a.test", 1), 1, std::string(kIp1, 4), 10);
  // Corrupt the answer name pointer to point past the end.
  message[message.size() - 6] = static_cast<char>(0xff);
  message[message.size() - 5] = static_cast<char>(0xff);
  EXPECT_FALSE(ParseDnsResponse(message, &answer));
  // A truncated answer (rdlength pointing past the message) is rejected too.
  std::string truncated = MakeResponse(EncodeDnsQuery(8, "a.test", 1), 1, std::string(kIp1, 4), 10);
  truncated.resize(truncated.size() - 3);
  EXPECT_FALSE(ParseDnsResponse(truncated, &answer));
}

// ---------------------------------------------------------------------------
// Resolver against a local UDP server
// ---------------------------------------------------------------------------

#ifndef _WIN32
DnsResolver::Options OptionsFor(const TestDnsServer& server)
{
  DnsResolver::Options options;
  options.servers = {"127.0.0.1"};
  options.port = server.port();
  options.timeout_ms = 500;
  options.attempts = 1;
  options.hosts_path = "/nonexistent-hosts-file";
  options.resolv_conf_path = "/nonexistent-resolv.conf";
  return options;
}

TEST(DnsResolverTest, ResolvesThroughUdpAndCachesUntilTheTtlExpires)
{
  const std::string ip(kIp1, 4);
  TestDnsServer server([&ip](std::string_view query, std::string* response) {
    const std::uint16_t type = QueryType(query);
    if (type == 28) {
      return false; // no AAAA records: the client just gets nothing
    }
    *response = MakeResponse(query, 1, ip, 60);
    return true;
  });
  ASSERT_TRUE(server.valid());

  DnsResolver resolver(OptionsFor(server));
  auto clock =
      std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
  resolver.SetClockForTesting([clock] { return *clock; });

  const auto first = resolver.Resolve("www.example.test");
  ASSERT_TRUE(first.has_value()) << first.error().message();
  ASSERT_FALSE(first.value().empty());
  EXPECT_EQ(first.value()[0], "93.184.216.34");
  const int queries_after_first = server.queries();
  EXPECT_GE(queries_after_first, 1);

  // Cached: no new query, same answer.
  const auto second = resolver.Resolve("WWW.Example.Test"); // case-insensitive
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(server.queries(), queries_after_first);
  EXPECT_EQ(resolver.cache_size(), 1u);

  // After the TTL the entry expires and the server is asked again.
  *clock += std::chrono::seconds(61);
  const auto third = resolver.Resolve("www.example.test");
  ASSERT_TRUE(third.has_value());
  EXPECT_GT(server.queries(), queries_after_first);
}

TEST(DnsResolverTest, FollowsCnameChains)
{
  const std::string ip(kIp2, 4);
  TestDnsServer server([&ip](std::string_view query, std::string* response) {
    const std::string name = NameAt(query, 12);
    if (name == "www.example.test") {
      *response = MakeResponse(query, 1, "", 30, 0, "cdn.example.test");
      return true;
    }
    if (name == "cdn.example.test" && QueryType(query) == 1) {
      *response = MakeResponse(query, 1, ip, 30);
      return true;
    }
    return false; // AAAA and anything else: dropped
  });
  ASSERT_TRUE(server.valid());

  DnsResolver resolver(OptionsFor(server));
  const auto resolved = resolver.Resolve("www.example.test");
  ASSERT_TRUE(resolved.has_value()) << resolved.error().message();
  ASSERT_EQ(resolved.value().size(), 1u);
  EXPECT_EQ(resolved.value()[0], "10.0.0.1");
}

TEST(DnsResolverTest, NxdomainIsCachedNegatively)
{
  TestDnsServer server([](std::string_view query, std::string* response) {
    *response = MakeResponse(query, 1, "", 0, 3); // NXDOMAIN
    return true;
  });
  ASSERT_TRUE(server.valid());
  DnsResolver resolver(OptionsFor(server));
  auto clock =
      std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
  resolver.SetClockForTesting([clock] { return *clock; });

  const auto first = resolver.Resolve("missing.example.test");
  ASSERT_FALSE(first.has_value());
  EXPECT_NE(first.error().message().find("NXDOMAIN"), std::string::npos) << first.error().message();
  const int queries = server.queries();
  ASSERT_GE(queries, 1);

  // The failure is remembered: no further query, same-shaped error.
  const auto second = resolver.Resolve("missing.example.test");
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(server.queries(), queries);

  // Once the negative entry expires the resolver tries again.
  *clock += std::chrono::seconds(11);
  EXPECT_FALSE(resolver.Resolve("missing.example.test").has_value());
  EXPECT_GT(server.queries(), queries);
}

TEST(DnsResolverTest, MismatchedResponseIdsAreIgnored)
{
  TestDnsServer server([](std::string_view query, std::string* response) {
    *response = MakeResponse(query, 1, std::string(kIp1, 4), 60);
    // Corrupt the id so the client must not accept the answer.
    (*response)[0] = static_cast<char>(0x7f);
    (*response)[1] = static_cast<char>(0x7e);
    return true;
  });
  ASSERT_TRUE(server.valid());
  DnsResolver resolver(OptionsFor(server));

  const auto resolved = resolver.Resolve("spoofed.example.test");
  ASSERT_FALSE(resolved.has_value());
  EXPECT_NE(resolved.error().message().find("id mismatch"), std::string::npos)
      << resolved.error().message();
}

TEST(DnsResolverTest, MalformedResponsesAreRejected)
{
  TestDnsServer server([](std::string_view query, std::string* response) {
    *response = MakeResponse(query, 1, std::string(kIp1, 4), 60);
    response->resize(20); // truncated answer
    return true;
  });
  ASSERT_TRUE(server.valid());
  DnsResolver resolver(OptionsFor(server));
  const auto resolved = resolver.Resolve("broken.example.test");
  ASSERT_FALSE(resolved.has_value());
  EXPECT_NE(resolved.error().message().find("malformed"), std::string::npos)
      << resolved.error().message();
}

TEST(DnsResolverTest, HostsFileWinsAndNumericHostsSkipTheNetwork)
{
  const std::string hosts = TempFile(
      "hosts", "127.0.0.1 localhost hosts.test\n# comment\n10.9.8.7\tother.test alias.test\n");
  DnsResolver::Options options;
  options.servers = {"127.0.0.1"};
  options.port = 1; // nothing listens there: a query would fail
  options.timeout_ms = 100;
  options.attempts = 1;
  options.hosts_path = hosts;
  DnsResolver resolver(options);

  const auto from_hosts = resolver.Resolve("hosts.test");
  ASSERT_TRUE(from_hosts.has_value()) << from_hosts.error().message();
  ASSERT_EQ(from_hosts.value().size(), 1u);
  EXPECT_EQ(from_hosts.value()[0], "127.0.0.1");

  // An alias on a later hosts line resolves too.
  const auto alias = resolver.Resolve("alias.test");
  ASSERT_TRUE(alias.has_value());
  EXPECT_EQ(alias.value()[0], "10.9.8.7");

  // Numeric addresses bypass DNS entirely.
  const auto numeric = resolver.Resolve("192.0.2.10");
  ASSERT_TRUE(numeric.has_value());
  EXPECT_EQ(numeric.value()[0], "192.0.2.10");
}

TEST(DnsResolverTest, EmptyHostIsAnInvalidArgument)
{
  DnsResolver resolver;
  const auto resolved = resolver.Resolve("");
  ASSERT_FALSE(resolved.has_value());
  EXPECT_NE(resolved.error().message().find("empty host"), std::string::npos)
      << resolved.error().message();
}
#endif // !_WIN32

// ---------------------------------------------------------------------------
// Socket::Connect integration (the resolver is the first stage there)
// ---------------------------------------------------------------------------

#ifndef _WIN32
TEST(DnsSocketIntegration, ConnectsThroughTheResolverPath)
{
  const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(listen_fd, 0);
  struct sockaddr_in address
  {
  };
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  ASSERT_EQ(::listen(listen_fd, 4), 0);
  socklen_t length = sizeof(address);
  ASSERT_EQ(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&address), &length), 0);
  const uint16_t port = ntohs(address.sin_port);
  std::thread acceptor([listen_fd] {
    const int client = ::accept(listen_fd, nullptr, nullptr);
    if (client >= 0) {
      ::close(client);
    }
  });

  // A numeric host skips DNS; a hosts-file name ("localhost") goes through the
  // resolver's hosts stage.  Both must reach the local server.
  const auto numeric = Socket::Connect("127.0.0.1", port, 2000);
  EXPECT_TRUE(numeric.has_value()) << numeric.error().message();
  const auto by_name = Socket::Connect("localhost", port, 2000);
  EXPECT_TRUE(by_name.has_value()) << by_name.error().message();

  ::close(listen_fd);
  acceptor.join();
}
#endif // !_WIN32

} // namespace
} // namespace neko::network
