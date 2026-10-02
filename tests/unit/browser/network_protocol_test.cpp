// Unit tests for the network-process wire protocol (ADR 0016 M3a): round trips
// for every message kind and rejection of malformed/hostile payloads.  The
// child process end-to-end behavior lives in network_session_test.cpp.

#include "neko/browser/browser_options.h"
#include "neko/browser/network_protocol.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <vector>

namespace neko::browser {
namespace {

std::string Byte(std::uint8_t value)
{
  return std::string(1, static_cast<char>(value));
}

std::string U32(std::uint32_t value)
{
  std::string out;
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
  }
  return out;
}

TEST(NetworkProtocolTest, FetchRequestRoundTrips)
{
  NetworkRequest request;
  request.op = NetworkOp::kFetch;
  request.url = "https://example.com/a?b=c";
  request.cookie_header = "sid=secret; theme=dark";
  const auto encoded = EncodeNetworkRequest(request);
  ASSERT_TRUE(encoded.has_value());

  const auto decoded = DecodeNetworkRequest(encoded.value());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value().op, NetworkOp::kFetch);
  EXPECT_EQ(decoded.value().url, request.url);
  EXPECT_EQ(decoded.value().cookie_header, request.cookie_header);
}

TEST(NetworkProtocolTest, ShutdownRequestRoundTrips)
{
  NetworkRequest request;
  request.op = NetworkOp::kShutdown;
  const auto encoded = EncodeNetworkRequest(request);
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeNetworkRequest(encoded.value());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value().op, NetworkOp::kShutdown);
}

TEST(NetworkProtocolTest, ResponseReplyRoundTrips)
{
  NetworkReply reply;
  reply.kind = NetworkReply::Kind::kResponse;
  reply.status_code = 200;
  reply.reason = "OK";
  reply.final_url = "https://example.com/final";
  reply.headers = {{"content-type", "text/html"}, {"set-cookie", "a=b"}};
  reply.body = "<html>hello</html>";
  const auto encoded = EncodeNetworkReply(reply);
  ASSERT_TRUE(encoded.has_value());

  const auto decoded = DecodeNetworkReply(encoded.value());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value().kind, NetworkReply::Kind::kResponse);
  EXPECT_EQ(decoded.value().status_code, 200);
  EXPECT_EQ(decoded.value().reason, "OK");
  EXPECT_EQ(decoded.value().final_url, reply.final_url);
  ASSERT_EQ(decoded.value().headers.size(), 2u);
  EXPECT_EQ(decoded.value().headers[0].name, "content-type");
  EXPECT_EQ(decoded.value().headers[0].value, "text/html");
  EXPECT_EQ(decoded.value().headers[1].name, "set-cookie");
  EXPECT_EQ(decoded.value().headers[1].value, "a=b");
  EXPECT_EQ(decoded.value().body, reply.body);
}

TEST(NetworkProtocolTest, ErrorReplyRoundTrips)
{
  NetworkReply reply;
  reply.kind = NetworkReply::Kind::kError;
  reply.error = "TLS handshake failed";
  const auto encoded = EncodeNetworkReply(reply);
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeNetworkReply(encoded.value());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value().kind, NetworkReply::Kind::kError);
  EXPECT_EQ(decoded.value().error, reply.error);
}

TEST(NetworkProtocolTest, CookieLookupAndReplyRoundTrip)
{
  NetworkReply lookup;
  lookup.kind = NetworkReply::Kind::kCookieLookup;
  lookup.lookup_url = "https://cdn.example.net/a.js";
  const auto encoded_lookup = EncodeNetworkReply(lookup);
  ASSERT_TRUE(encoded_lookup.has_value());
  const auto decoded_lookup = DecodeNetworkReply(encoded_lookup.value());
  ASSERT_TRUE(decoded_lookup.has_value());
  EXPECT_EQ(decoded_lookup.value().kind, NetworkReply::Kind::kCookieLookup);
  EXPECT_EQ(decoded_lookup.value().lookup_url, lookup.lookup_url);

  const auto encoded_cookie = EncodeCookieReply("sid=abc");
  ASSERT_TRUE(encoded_cookie.has_value());
  const auto cookie = DecodeCookieReply(encoded_cookie.value());
  ASSERT_TRUE(cookie.has_value());
  EXPECT_EQ(cookie.value(), "sid=abc");

  // An empty cookie is a valid answer (no cookies for that hop).
  const auto encoded_empty = EncodeCookieReply("");
  ASSERT_TRUE(encoded_empty.has_value());
  const auto empty = DecodeCookieReply(encoded_empty.value());
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty.value().empty());
}

TEST(NetworkProtocolTest, VersionMismatchRejected)
{
  NetworkRequest request;
  request.op = NetworkOp::kFetch;
  request.url = "http://example.com/";
  auto encoded = EncodeNetworkRequest(request).value();
  encoded[0] = static_cast<char>(99);
  EXPECT_FALSE(DecodeNetworkRequest(encoded).has_value());
}

TEST(NetworkProtocolTest, UnknownOpRejected)
{
  const std::string payload = Byte(kNetworkProtocolVersion) + Byte(99);
  EXPECT_FALSE(DecodeNetworkRequest(payload).has_value());
}

TEST(NetworkProtocolTest, UnknownReplyKindRejected)
{
  const std::string payload = Byte(kNetworkProtocolVersion) + Byte(99);
  EXPECT_FALSE(DecodeNetworkReply(payload).has_value());
}

TEST(NetworkProtocolTest, TruncatedPayloadsRejected)
{
  NetworkRequest request;
  request.op = NetworkOp::kFetch;
  request.url = "http://example.com/";
  request.cookie_header = "a=b";
  const std::string encoded_request = EncodeNetworkRequest(request).value();
  EXPECT_FALSE(
      DecodeNetworkRequest(encoded_request.substr(0, encoded_request.size() - 1)).has_value());

  NetworkReply reply;
  reply.kind = NetworkReply::Kind::kResponse;
  reply.status_code = 200;
  reply.reason = "OK";
  reply.final_url = "http://example.com/";
  reply.headers = {{"content-type", "text/plain"}};
  reply.body = "body";
  const std::string encoded_reply = EncodeNetworkReply(reply).value();
  EXPECT_FALSE(DecodeNetworkReply(encoded_reply.substr(0, encoded_reply.size() - 1)).has_value());

  const std::string encoded_cookie = EncodeCookieReply("a=b").value();
  EXPECT_FALSE(DecodeCookieReply(encoded_cookie.substr(0, encoded_cookie.size() - 1)).has_value());
}

TEST(NetworkProtocolTest, TrailingBytesRejected)
{
  NetworkRequest request;
  request.op = NetworkOp::kShutdown;
  std::string encoded = EncodeNetworkRequest(request).value();
  encoded.push_back('\0');
  EXPECT_FALSE(DecodeNetworkRequest(encoded).has_value());

  const std::string reply = EncodeNetworkReply(NetworkReply{}).value() + std::string(1, '\0');
  EXPECT_FALSE(DecodeNetworkReply(reply).has_value());
}

TEST(NetworkProtocolTest, TooManyHeadersRejectedWithoutAllocation)
{
  // A response that claims ~4 billion headers must fail the count check
  // before any per-header allocation happens.
  std::string payload =
      Byte(kNetworkProtocolVersion) + Byte(0) + U32(200) + U32(0) + U32(0) + U32(0xFFFFFFFFu);
  const auto decoded = DecodeNetworkReply(payload);
  EXPECT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST(NetworkProtocolTest, OversizedFieldsRejectedOnEncode)
{
  NetworkRequest request;
  request.op = NetworkOp::kFetch;
  request.url = "http://example.com/";
  // A cookie beyond the field cap must be rejected before it becomes a u32
  // length in the payload.
  request.cookie_header.assign(2u * 1024u * 1024u, 'x');
  EXPECT_FALSE(EncodeNetworkRequest(request).has_value());
}

TEST(NetworkProtocolTest, RandomGarbageNeverCrashesOrAllocatesWildly)
{
  std::mt19937 rng(2026);
  std::uniform_int_distribution<int> length_dist(0, 64);
  std::uniform_int_distribution<int> byte_dist(0, 255);
  for (int iteration = 0; iteration < 300; ++iteration) {
    std::string payload;
    const int length = length_dist(rng);
    payload.reserve(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i) {
      payload.push_back(static_cast<char>(byte_dist(rng)));
    }
    (void)DecodeNetworkRequest(payload);
    (void)DecodeNetworkReply(payload);
    (void)DecodeCookieReply(payload);
  }
}

TEST(NetworkOptionsTest, CommandLineRecognizesNetworkFlags)
{
  char* argv[] = {const_cast<char*>("neko-browser"),
                  const_cast<char*>("--network-process"),
                  const_cast<char*>("--network-child"),
                  nullptr};
  const ParseResult parsed = ParseCommandLine(3, argv);
  EXPECT_EQ(parsed.action, ParseResult::Action::kRun);
  EXPECT_TRUE(parsed.options.network_process);
  EXPECT_TRUE(parsed.options.network_child);
  EXPECT_NE(UsageText().find("--network-child"), std::string::npos);
}

} // namespace
} // namespace neko::browser
