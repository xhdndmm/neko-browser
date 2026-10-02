#include "neko/browser/network_protocol.h"

#include <algorithm>
#include <cstddef>

namespace neko::browser {
namespace {

// A bounds-checked cursor over a wire payload.  Every read validates its
// range; malformed input surfaces as InvalidArgument instead of UB.
class Reader
{
public:
  explicit Reader(std::string_view payload) : data_(payload.data()), size_(payload.size()) {}

  bool Ok() const
  {
    return ok_;
  }
  std::size_t Remaining() const
  {
    return pos_ <= size_ ? size_ - pos_ : 0;
  }

  base::Error Take(std::size_t n, const std::uint8_t** out)
  {
    if (!ok_ || pos_ > size_ || size_ - pos_ < n) {
      ok_ = false;
      return base::Error::InvalidArgument("truncated payload");
    }
    *out = reinterpret_cast<const std::uint8_t*>(data_ + pos_);
    pos_ += n;
    return base::Error();
  }

  base::Error U8(std::uint8_t* out)
  {
    const std::uint8_t* p = nullptr;
    if (auto e = Take(1, &p); !e.ok()) {
      return e;
    }
    *out = p[0];
    return base::Error();
  }

  base::Error U32(std::uint32_t* out)
  {
    const std::uint8_t* p = nullptr;
    if (auto e = Take(4, &p); !e.ok()) {
      return e;
    }
    *out = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    return base::Error();
  }

  base::Error U64(std::uint64_t* out)
  {
    const std::uint8_t* p = nullptr;
    if (auto e = Take(8, &p); !e.ok()) {
      return e;
    }
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
      value = (value << 8) | p[i];
    }
    *out = value;
    return base::Error();
  }

  base::Error String(std::uint32_t max_len, std::string* out)
  {
    std::uint32_t len = 0;
    if (auto e = U32(&len); !e.ok()) {
      return e;
    }
    if (len > max_len) {
      ok_ = false;
      return base::Error::InvalidArgument("field exceeds its size cap");
    }
    const std::uint8_t* p = nullptr;
    if (auto e = Take(len, &p); !e.ok()) {
      return e;
    }
    out->assign(reinterpret_cast<const char*>(p), len);
    return base::Error();
  }

private:
  const char* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

// Field size caps (independent of the 64 MiB frame cap; keep a hostile peer
// from asking for absurd allocations inside a well-formed frame).
inline constexpr std::uint32_t kMaxUrlBytes = 8u * 1024u * 1024u;
inline constexpr std::uint32_t kMaxCookieBytes = 1024u * 1024u;
inline constexpr std::uint32_t kMaxHeaderFieldBytes = 1024u * 1024u;
inline constexpr std::uint32_t kMaxErrorBytes = 1024u * 1024u;
inline constexpr std::uint32_t kMaxHeaderCount = 4096;

void PutU8(std::string& out, std::uint8_t v)
{
  out.push_back(static_cast<char>(v));
}

void PutU32(std::string& out, std::uint32_t v)
{
  out.push_back(static_cast<char>(v & 0xff));
  out.push_back(static_cast<char>((v >> 8) & 0xff));
  out.push_back(static_cast<char>((v >> 16) & 0xff));
  out.push_back(static_cast<char>((v >> 24) & 0xff));
}

void PutU64(std::string& out, std::uint64_t v)
{
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
  }
}

void PutString(std::string& out, std::string_view text)
{
  PutU32(out, static_cast<std::uint32_t>(text.size()));
  out.append(text.data(), text.size());
}

bool Fits(std::string_view text, std::uint32_t cap)
{
  return text.size() <= cap;
}

} // namespace

base::Result<std::string> EncodeNetworkRequest(const NetworkRequest& request)
{
  if (!Fits(request.url, kMaxUrlBytes) || !Fits(request.cookie_header, kMaxCookieBytes)) {
    return base::Err(base::Error::InvalidArgument("request field exceeds its size cap"));
  }
  std::string out;
  out.reserve(request.url.size() + request.cookie_header.size() + 12);
  PutU8(out, kNetworkProtocolVersion);
  PutU8(out, static_cast<std::uint8_t>(request.op));
  if (request.op == NetworkOp::kFetch) {
    PutString(out, request.url);
    PutString(out, request.cookie_header);
  }
  return out;
}

base::Result<NetworkRequest> DecodeNetworkRequest(std::string_view payload)
{
  Reader reader(payload);
  NetworkRequest request;
  std::uint8_t version = 0;
  std::uint8_t op = 0;
  if (auto e = reader.U8(&version); !e.ok()) {
    return base::Err(e);
  }
  if (version != kNetworkProtocolVersion) {
    return base::Err(base::Error::InvalidArgument("unsupported network protocol version"));
  }
  if (auto e = reader.U8(&op); !e.ok()) {
    return base::Err(e);
  }
  if (op != static_cast<std::uint8_t>(NetworkOp::kFetch) &&
      op != static_cast<std::uint8_t>(NetworkOp::kShutdown)) {
    return base::Err(base::Error::InvalidArgument("unknown network operation"));
  }
  request.op = static_cast<NetworkOp>(op);
  if (request.op == NetworkOp::kFetch) {
    if (auto e = reader.String(kMaxUrlBytes, &request.url); !e.ok()) {
      return base::Err(e);
    }
    if (auto e = reader.String(kMaxCookieBytes, &request.cookie_header); !e.ok()) {
      return base::Err(e);
    }
  }
  if (reader.Remaining() != 0) {
    return base::Err(base::Error::InvalidArgument("trailing bytes in network request"));
  }
  return request;
}

base::Result<std::string> EncodeNetworkReply(const NetworkReply& reply)
{
  if (!Fits(reply.error, kMaxErrorBytes) || !Fits(reply.lookup_url, kMaxUrlBytes) ||
      !Fits(reply.reason, kMaxHeaderFieldBytes) || !Fits(reply.final_url, kMaxUrlBytes) ||
      reply.body.size() > kMaxNetworkBodyBytes || reply.headers.size() > kMaxHeaderCount) {
    return base::Err(base::Error::InvalidArgument("network reply field exceeds its size cap"));
  }
  for (const NetworkReply::Header& header : reply.headers) {
    if (!Fits(header.name, kMaxHeaderFieldBytes) || !Fits(header.value, kMaxHeaderFieldBytes)) {
      return base::Err(base::Error::InvalidArgument("network header field exceeds its size cap"));
    }
  }

  std::string out;
  out.reserve(reply.body.size() + 64 + reply.headers.size() * 32);
  PutU8(out, kNetworkProtocolVersion);
  PutU8(out, static_cast<std::uint8_t>(reply.kind));
  switch (reply.kind) {
  case NetworkReply::Kind::kResponse: {
    PutU32(out, static_cast<std::uint32_t>(std::max(0, reply.status_code)));
    PutString(out, reply.reason);
    PutString(out, reply.final_url);
    PutU32(out, static_cast<std::uint32_t>(reply.headers.size()));
    for (const NetworkReply::Header& header : reply.headers) {
      PutString(out, header.name);
      PutString(out, header.value);
    }
    PutU64(out, static_cast<std::uint64_t>(reply.body.size()));
    out.append(reply.body);
    break;
  }
  case NetworkReply::Kind::kError:
    PutString(out, reply.error);
    break;
  case NetworkReply::Kind::kCookieLookup:
    PutString(out, reply.lookup_url);
    break;
  }
  return out;
}

base::Result<NetworkReply> DecodeNetworkReply(std::string_view payload)
{
  Reader reader(payload);
  NetworkReply reply;
  std::uint8_t version = 0;
  std::uint8_t kind = 0;
  if (auto e = reader.U8(&version); !e.ok()) {
    return base::Err(e);
  }
  if (version != kNetworkProtocolVersion) {
    return base::Err(base::Error::InvalidArgument("unsupported network protocol version"));
  }
  if (auto e = reader.U8(&kind); !e.ok()) {
    return base::Err(e);
  }
  switch (static_cast<NetworkReply::Kind>(kind)) {
  case NetworkReply::Kind::kResponse: {
    reply.kind = NetworkReply::Kind::kResponse;
    std::uint32_t status = 0;
    if (auto e = reader.U32(&status); !e.ok()) {
      return base::Err(e);
    }
    if (auto e = reader.String(kMaxHeaderFieldBytes, &reply.reason); !e.ok()) {
      return base::Err(e);
    }
    if (auto e = reader.String(kMaxUrlBytes, &reply.final_url); !e.ok()) {
      return base::Err(e);
    }
    std::uint32_t header_count = 0;
    if (auto e = reader.U32(&header_count); !e.ok()) {
      return base::Err(e);
    }
    if (header_count > kMaxHeaderCount) {
      return base::Err(base::Error::InvalidArgument("too many headers"));
    }
    reply.headers.reserve(header_count);
    for (std::uint32_t i = 0; i < header_count; ++i) {
      NetworkReply::Header header;
      if (auto e = reader.String(kMaxHeaderFieldBytes, &header.name); !e.ok()) {
        return base::Err(e);
      }
      if (auto e = reader.String(kMaxHeaderFieldBytes, &header.value); !e.ok()) {
        return base::Err(e);
      }
      reply.headers.push_back(std::move(header));
    }
    std::uint64_t body_len = 0;
    if (auto e = reader.U64(&body_len); !e.ok()) {
      return base::Err(e);
    }
    if (body_len > kMaxNetworkBodyBytes) {
      return base::Err(base::Error::InvalidArgument("response body exceeds the cap"));
    }
    const std::uint8_t* body = nullptr;
    if (auto e = reader.Take(static_cast<std::size_t>(body_len), &body); !e.ok()) {
      return base::Err(e);
    }
    reply.body.assign(reinterpret_cast<const char*>(body), body_len);
    reply.status_code = static_cast<int>(status);
    break;
  }
  case NetworkReply::Kind::kError:
    reply.kind = NetworkReply::Kind::kError;
    if (auto e = reader.String(kMaxErrorBytes, &reply.error); !e.ok()) {
      return base::Err(e);
    }
    break;
  case NetworkReply::Kind::kCookieLookup:
    reply.kind = NetworkReply::Kind::kCookieLookup;
    if (auto e = reader.String(kMaxUrlBytes, &reply.lookup_url); !e.ok()) {
      return base::Err(e);
    }
    break;
  default:
    return base::Err(base::Error::InvalidArgument("unknown network reply kind"));
  }
  if (reader.Remaining() != 0) {
    return base::Err(base::Error::InvalidArgument("trailing bytes in network reply"));
  }
  return reply;
}

base::Result<std::string> EncodeCookieReply(std::string_view cookie)
{
  if (!Fits(cookie, kMaxCookieBytes)) {
    return base::Err(base::Error::InvalidArgument("cookie exceeds its size cap"));
  }
  std::string out;
  out.reserve(cookie.size() + 8);
  PutU8(out, kNetworkProtocolVersion);
  PutString(out, cookie);
  return out;
}

base::Result<std::string> DecodeCookieReply(std::string_view payload)
{
  Reader reader(payload);
  std::uint8_t version = 0;
  if (auto e = reader.U8(&version); !e.ok()) {
    return base::Err(e);
  }
  if (version != kNetworkProtocolVersion) {
    return base::Err(base::Error::InvalidArgument("unsupported network protocol version"));
  }
  std::string cookie;
  if (auto e = reader.String(kMaxCookieBytes, &cookie); !e.ok()) {
    return base::Err(e);
  }
  if (reader.Remaining() != 0) {
    return base::Err(base::Error::InvalidArgument("trailing bytes in cookie reply"));
  }
  return cookie;
}

} // namespace neko::browser
