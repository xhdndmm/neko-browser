#pragma once

#include "neko/base/status.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace neko::browser {

// Versioned binary wire format between the browser process and a network
// child (ADR 0016 M3a).  Mirrors renderer_protocol.h: the IPC layer guarantees
// whole framed payloads up to 64 MiB; these helpers encode/decode the payload
// itself, and every decode is bounds-checked — the other end of the pipe is
// part of the threat model.
//
// Messages (little-endian):
//
//   NetworkRequest (browser -> child):
//     u8 version(1) | u8 op(1 = fetch, 2 = shutdown)
//     fetch: u32 url_len | url | u32 cookie_len | cookie
//
//   NetworkReply (child -> browser):
//     u8 version(1) | u8 kind
//     kind 0 (response): u32 status_code | u32 reason_len | reason |
//                        u32 final_url_len | final_url |
//                        u32 header_count | (u32 name_len | name |
//                                              u32 value_len | value)* |
//                        u64 body_len | body
//     kind 1 (error):    u32 message_len | message
//     kind 2 (cookie lookup): u32 url_len | url
//
//   CookieReply (browser -> child, answering kind 2):
//     u8 version(1) | u32 cookie_len | cookie
//
// The cookie lookup makes redirect handling correct without moving the cookie
// jar out of the browser: HttpGet re-invokes its HeaderProvider on every hop,
// the child forwards each hop URL to the browser, and the browser answers with
// the cookies for that exact host (HttpOnly cookies therefore never need to
// exist in the child).
inline constexpr std::uint8_t kNetworkProtocolVersion = 1;

// Response bodies travel inside one IPC frame (64 MiB cap).  Keep a safety
// margin for the protocol overhead and reject larger bodies explicitly instead
// of letting the frame layer fail: chunked/streamed delivery is M3b work.
inline constexpr std::uint32_t kMaxNetworkBodyBytes = 48u * 1024u * 1024u;

enum class NetworkOp : std::uint8_t
{
  kFetch = 1,    // GET |url| with |cookie_header| for the first hop
  kShutdown = 2, // browser is done with the child
};

struct NetworkRequest
{
  NetworkOp op = NetworkOp::kFetch;
  std::string url;
  // Cookie header ("name=value; name2=value2", no "Cookie:" prefix) for the
  // first hop.  Redirect hops are resolved through the cookie-lookup channel.
  std::string cookie_header;
};

struct NetworkReply
{
  enum class Kind : std::uint8_t
  {
    kResponse = 0,
    kError = 1,
    kCookieLookup = 2,
  };

  struct Header
  {
    std::string name;
    std::string value;
  };

  Kind kind = Kind::kResponse;

  // kResponse
  int status_code = 0;
  std::string reason;
  std::string final_url;
  std::vector<Header> headers;
  std::string body;

  // kError
  std::string error;

  // kCookieLookup
  std::string lookup_url;
};

base::Result<std::string> EncodeNetworkRequest(const NetworkRequest& request);
base::Result<NetworkRequest> DecodeNetworkRequest(std::string_view payload);

base::Result<std::string> EncodeNetworkReply(const NetworkReply& reply);
base::Result<NetworkReply> DecodeNetworkReply(std::string_view payload);

base::Result<std::string> EncodeCookieReply(std::string_view cookie);
base::Result<std::string> DecodeCookieReply(std::string_view payload);

} // namespace neko::browser
