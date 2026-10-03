#include "neko/network/http.h"

#include "neko/base/logging.h"
#include "neko/base/string_util.h"
#include "neko/base/version.h"
#include "neko/network/compression.h"
#include "neko/network/http_client.h"
#include "neko/network/socket.h"
#include "neko/network/tls_socket.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neko::network {
namespace {

// Upper bound on a single response body, so a malicious Content-Length or
// chunk size cannot trigger an unbounded allocation.  512 MiB is well above
// any legitimate page body.
constexpr std::size_t kMaxBodySize = 512u * 1024u * 1024u;

// Decodes standard base64 (RFC 4648 §4); ASCII whitespace is ignored and
// missing padding is accepted (both are common in the wild).  Returns
// InvalidArgument for characters outside the alphabet, non-zero tail bits or
// data after the padding.
base::Result<std::string> Base64Decode(std::string_view input)
{
  auto value_of = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') {
      return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
      return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
      return c - '0' + 52;
    }
    if (c == '+') {
      return 62;
    }
    if (c == '/') {
      return 63;
    }
    return -1;
  };

  std::string out;
  out.reserve(input.size() / 4 * 3 + 3);
  std::uint32_t accumulator = 0;
  int bits = 0;
  bool padded = false;
  for (const char c : input) {
    if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
      continue;
    }
    if (c == '=') {
      padded = true;
      continue;
    }
    if (padded) {
      return base::Err(base::Error::InvalidArgument("base64 data after padding"));
    }
    const int value = value_of(c);
    if (value < 0) {
      return base::Err(base::Error::InvalidArgument("invalid base64 character"));
    }
    accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((accumulator >> bits) & 0xffu));
    }
  }
  if (bits > 0 && (accumulator & ((1u << bits) - 1u)) != 0) {
    return base::Err(base::Error::InvalidArgument("base64 tail bits are not zero"));
  }
  return out;
}

bool NoProxyMatches(std::string_view host, uint16_t port)
{
  const char* value = std::getenv("no_proxy");
  if (value == nullptr || *value == '\0') {
    value = std::getenv("NO_PROXY");
  }
  if (value == nullptr || *value == '\0') {
    return false;
  }
  const std::string lower_host = base::ToLower(host);
  std::string_view entries(value);
  while (!entries.empty()) {
    const std::size_t comma = entries.find(',');
    std::string entry(base::Trim(entries.substr(0, comma)));
    entries = comma == std::string_view::npos ? std::string_view{} : entries.substr(comma + 1);
    if (entry == "*") {
      return true;
    }
    const std::size_t colon = entry.rfind(':');
    if (colon != std::string::npos && entry.find(':') == colon) {
      const std::string port_text = entry.substr(colon + 1);
      if (!port_text.empty() && port_text != std::to_string(port)) {
        continue;
      }
      entry.resize(colon);
    }
    entry = base::ToLower(entry);
    if (entry.empty()) {
      continue;
    }
    if (entry.front() == '.') {
      entry.erase(entry.begin());
    }
    if (lower_host == entry ||
        (lower_host.size() > entry.size() &&
         lower_host.compare(lower_host.size() - entry.size(), entry.size(), entry) == 0 &&
         lower_host[lower_host.size() - entry.size() - 1] == '.')) {
      return true;
    }
  }
  return false;
}

base::Result<TlsOptions>
TlsOptionsWithEnvironmentProxy(std::string_view host, uint16_t port, const TlsOptions& options)
{
  if (!options.proxy_host.empty() || NoProxyMatches(host, port)) {
    return options;
  }
  const char* proxy = std::getenv("https_proxy");
  if (proxy == nullptr || *proxy == '\0') {
    proxy = std::getenv("HTTPS_PROXY");
  }
  if (proxy == nullptr || *proxy == '\0') {
    return options;
  }
  const base::Result<url::Url> parsed = url::Url::Parse(proxy);
  if (!parsed || parsed.value().scheme() != "http" || parsed.value().host().empty()) {
    return base::Err(base::Error::Network("HTTPS proxy must be an http:// URL with a host"));
  }
  TlsOptions configured = options;
  configured.proxy_host = parsed.value().host();
  configured.proxy_port = parsed.value().effective_port();
  return configured;
}

// Parses a decimal byte count, rejecting values that overflow size_t.
// Returns nullopt for empty or non-numeric input.
std::optional<std::size_t> ParseByteCount(std::string_view text)
{
  if (text.empty()) {
    return std::nullopt;
  }
  std::size_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    if (value > (SIZE_MAX - static_cast<std::size_t>(c - '0')) / 10) {
      return std::nullopt;
    }
    value = value * 10 + static_cast<std::size_t>(c - '0');
  }
  return value;
}

// Returns the values of every header named |name| (case-insensitive).
std::vector<std::string_view> GetAllHeaderValues(const HttpResponse& response,
                                                 std::string_view name)
{
  std::vector<std::string_view> values;
  for (const HttpHeader& header : response.headers) {
    if (base::AsciiEqualsIgnoreCase(header.name, name)) {
      values.push_back(header.value);
    }
  }
  return values;
}

// RFC 9110 section 5.1 field-name = token.  CTLs and separators are rejected
// so a header name can never terminate the field and start another line.
bool IsValidHeaderFieldName(std::string_view name)
{
  if (name.empty()) {
    return false;
  }
  for (const char ch : name) {
    const auto c = static_cast<unsigned char>(ch);
    if (c <= 0x20 || c >= 0x7F) {
      return false; // CTLs, SP and DEL
    }
    switch (c) {
    case '(':
    case ')':
    case '<':
    case '>':
    case '@':
    case ',':
    case ';':
    case ':':
    case '\\':
    case '"':
    case '/':
    case '[':
    case ']':
    case '?':
    case '=':
    case '{':
    case '}': // separators
      return false;
    default:
      break;
    }
  }
  return true;
}

// RFC 9110 section 5.5 field-value = *( field-vchar [ 1*( SP / HTAB ) field-vchar ] ).
// field-vchar = VCHAR / obs-text, so any CTL -- and therefore CR and LF -- is
// forbidden.  HTAB is tolerated because header folding was historically common.
bool IsValidHeaderFieldValue(std::string_view value)
{
  for (const char ch : value) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == '\t') {
      continue;
    }
    if (c < 0x20 || c == 0x7F) {
      return false; // CTL or DEL
    }
  }
  return true;
}

// Percent-encodes a URL path/query for use in an HTTP request target
// (RFC 3986 3.3 / RFC 7230 5.3).  Only characters that are valid in a path
// or query are left raw; anything else (spaces, control characters, quote
// and angle brackets, non-ASCII bytes) is encoded so that untrusted URL
// components cannot inject bytes into the request line.
bool IsRequestTargetChar(unsigned char c)
{
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
    return true;
  }
  switch (c) {
  case '-':
  case '.':
  case '_':
  case '~': // unreserved
  case '!':
  case '$':
  case '&':
  case '\'':
  case '(':
  case ')': // sub-delims
  case '*':
  case '+':
  case ',':
  case ';':
  case '=':
  case ':':
  case '@':
  case '/':
  case '?':
  case '%':
    return true;
  default:
    return false;
  }
}

std::string EncodeRequestTarget(std::string_view text)
{
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (IsRequestTargetChar(c)) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

std::string_view TrimView(std::string_view text)
{
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

// Parses one chunk-size line ("1A", "1A;ext=...").  Returns the byte count,
// or nullopt for an empty/invalid value or one that overflows size_t (a
// crafted size could otherwise wrap and silently truncate the body).
std::optional<std::size_t> ParseChunkSizeLine(std::string_view line)
{
  const std::size_t semi = line.find(';');
  const std::string_view hex = line.substr(0, semi);
  std::size_t chunk_size = 0;
  bool valid = !hex.empty();
  for (const char c : hex) {
    int digit = -1;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      valid = false;
      break;
    }
    if (chunk_size > (SIZE_MAX - static_cast<std::size_t>(digit)) / 16) {
      return std::nullopt;
    }
    chunk_size = chunk_size * 16 + static_cast<std::size_t>(digit);
  }
  if (!valid) {
    return std::nullopt;
  }
  return chunk_size;
}

// Decodes a chunked transfer body into |out.body|.
base::Result<void> DecodeChunked(std::string_view data, HttpResponse& out)
{
  std::string body;
  std::size_t pos = 0;
  for (;;) {
    const std::size_t line_end = data.find("\r\n", pos);
    if (line_end == std::string_view::npos) {
      return base::Err(base::Error::Parse("truncated chunk size line"));
    }
    const std::string_view size_line = data.substr(pos, line_end - pos);
    const std::optional<std::size_t> parsed = ParseChunkSizeLine(size_line);
    if (!parsed.has_value()) {
      return base::Err(base::Error::Parse("invalid chunk size"));
    }
    const std::size_t chunk_size = parsed.value();
    pos = line_end + 2;
    if (chunk_size == 0) {
      out.body = std::move(body);
      return base::Ok();
    }
    // |pos + chunk_size| could itself wrap; compare against the remaining
    // byte count instead of adding first.
    if (chunk_size > data.size() - pos) {
      return base::Err(base::Error::Parse("truncated chunk data"));
    }
    body.append(data.substr(pos, chunk_size));
    pos += chunk_size;
    if (pos + 2 <= data.size() && data.substr(pos, 2) == "\r\n") {
      pos += 2;
    }
  }
}

} // namespace

std::string HttpResponse::GetHeader(std::string_view name) const
{
  for (const HttpHeader& header : headers) {
    if (base::AsciiEqualsIgnoreCase(header.name, name)) {
      return header.value;
    }
  }
  return {};
}

// Parses the status line and header section of a response.  |header_block|
// must include the terminating CRLFCRLF.  Only fills status_code / reason /
// headers; body framing is handled by the caller.
base::Result<void> ParseResponseHeaders(std::string_view header_block, HttpResponse& response)
{
  const std::size_t line_end = header_block.find("\r\n");
  if (line_end == std::string_view::npos) {
    return base::Err(base::Error::Parse("missing status line"));
  }
  const std::string_view status_line = header_block.substr(0, line_end);
  const std::size_t sp1 = status_line.find(' ');
  if (sp1 == std::string_view::npos) {
    return base::Err(base::Error::Parse("malformed status line"));
  }
  const std::size_t sp2 = status_line.find(' ', sp1 + 1);
  const std::string_view code_str = status_line.substr(
      sp1 + 1, sp2 == std::string_view::npos ? std::string_view::npos : sp2 - sp1 - 1);
  if (code_str.empty()) {
    return base::Err(base::Error::Parse("missing status code"));
  }
  int code = 0;
  // Status codes are three digits (RFC 7230 5.3.2); reject anything longer
  // so the accumulation below cannot overflow.
  if (code_str.size() > 5) {
    return base::Err(base::Error::Parse("status code too long"));
  }
  for (const char c : code_str) {
    if (c < '0' || c > '9') {
      return base::Err(base::Error::Parse("invalid status code"));
    }
    code = code * 10 + (c - '0');
  }
  response.status_code = code;
  if (sp2 != std::string_view::npos) {
    response.reason = std::string(status_line.substr(sp2 + 1));
  }

  std::size_t pos = line_end + 2;
  for (;;) {
    const std::size_t end = header_block.find("\r\n", pos);
    if (end == std::string_view::npos) {
      return base::Err(base::Error::Parse("truncated header section"));
    }
    if (end == pos) {
      break; // CRLFCRLF terminator
    }
    const std::string_view line = header_block.substr(pos, end - pos);
    const std::size_t colon = line.find(':');
    if (colon != std::string_view::npos) {
      HttpHeader header;
      header.name = base::ToLower(line.substr(0, colon));
      header.value = std::string(TrimView(line.substr(colon + 1)));
      response.headers.push_back(std::move(header));
    }
    pos = end + 2;
  }
  return base::Ok();
}

// Determines how the response body is framed and returns the decoded body,
// or an error.  |raw_body| is the full body bytes already on hand (used by
// the legacy ParseHttpResponse path); a ResponseReader reads incrementally.
struct BodyFraming
{
  enum class Kind
  {
    kNone, // no body (e.g. 204/304, HEAD)
    kChunked,
    kContentLength,
    kUntilClose,
  };
  Kind kind = Kind::kNone;
  std::size_t length = 0; // kContentLength
};

base::Result<BodyFraming> ClassifyBody(const HttpResponse& response)
{
  const std::vector<std::string_view> transfer_encodings =
      GetAllHeaderValues(response, "transfer-encoding");
  const std::vector<std::string_view> content_lengths =
      GetAllHeaderValues(response, "content-length");

  // Transfer-Encoding takes precedence over Content-Length; having both is
  // ambiguous and is a request-smuggling / response-splitting vector, so
  // reject it (RFC 7230 3.3.3).
  if (!transfer_encodings.empty() && !content_lengths.empty()) {
    return base::Err(base::Error::Parse("conflicting transfer-encoding and content-length"));
  }

  if (!transfer_encodings.empty()) {
    // Only a final "chunked" encoding is understood; anything else is not
    // implemented and must not be treated as a plain body.
    const std::string_view last_te = transfer_encodings.back();
    const std::size_t comma = last_te.rfind(',');
    const std::string_view final_te =
        comma == std::string_view::npos ? last_te : last_te.substr(comma + 1);
    if (!base::AsciiEqualsIgnoreCase(TrimView(final_te), "chunked")) {
      return base::Err(base::Error::Parse("unsupported transfer-encoding"));
    }
    return BodyFraming{BodyFraming::Kind::kChunked, 0};
  }

  if (!content_lengths.empty()) {
    // All Content-Length values must agree; inconsistent values mean the
    // body boundary is ambiguous and must be rejected (RFC 7230 3.3.3).
    std::optional<std::size_t> length;
    for (const std::string_view value : content_lengths) {
      const std::optional<std::size_t> parsed = ParseByteCount(TrimView(value));
      if (!parsed.has_value()) {
        return base::Err(base::Error::Parse("invalid content-length"));
      }
      if (length.has_value() && length.value() != parsed.value()) {
        return base::Err(base::Error::Parse("conflicting content-length"));
      }
      length = parsed;
    }
    // Responses that are defined to never carry a body ignore Content-Length
    // (RFC 7230 3.3.3 / 3.3.2): a "0" is not an error here, and a nonzero
    // value on a bodyless response is garbage that must not be read as body.
    if (response.status_code == 204 || response.status_code == 304 ||
        (response.status_code >= 100 && response.status_code < 200)) {
      return BodyFraming{BodyFraming::Kind::kNone, 0};
    }
    if (length.value() > kMaxBodySize) {
      return base::Err(base::Error::Parse("content-length too large"));
    }
    return BodyFraming{BodyFraming::Kind::kContentLength, length.value()};
  }

  // No framing headers: the body runs until the connection closes.
  return BodyFraming{BodyFraming::Kind::kUntilClose, 0};
}

base::Result<HttpResponse> ParseHttpResponse(std::string_view raw)
{
  HttpResponse response;

  const std::size_t header_end = raw.find("\r\n\r\n");
  if (header_end == std::string_view::npos) {
    return base::Err(base::Error::Parse("missing header terminator"));
  }
  const base::Result<void> headers = ParseResponseHeaders(raw.substr(0, header_end + 4), response);
  if (!headers) {
    return base::Err(headers.error());
  }
  const std::string_view body = raw.substr(header_end + 4);

  const base::Result<BodyFraming> framing = ClassifyBody(response);
  if (!framing) {
    return base::Err(framing.error());
  }
  switch (framing.value().kind) {
  case BodyFraming::Kind::kChunked: {
    const base::Result<void> decoded = DecodeChunked(body, response);
    if (!decoded) {
      return base::Err(decoded.error());
    }
    return response;
  }
  case BodyFraming::Kind::kContentLength: {
    // A body shorter than Content-Length is a truncated response and must be
    // rejected rather than silently accepted (the TLS layer treats a
    // missing close_notify as EOF, so this check is the integrity backstop).
    if (body.size() < framing.value().length) {
      return base::Err(base::Error::Parse("truncated body"));
    }
    response.body = std::string(body.substr(0, framing.value().length));
    return response;
  }
  case BodyFraming::Kind::kNone:
    response.body.clear();
    return response;
  case BodyFraming::Kind::kUntilClose:
    response.body = std::string(body);
    return response;
  }
  return response;
}

// Incremental reader over a transport: reads the header block, then exactly
// the body bytes the response framing requires.  A peer that closes early is
// reported as an error by the framing-aware readers (never silently accepted
// as a complete body).
template <typename Transport> struct ResponseReader
{
  explicit ResponseReader(Transport& t) : transport(t) {}

  Transport& transport;
  std::string buffer; // bytes read but not yet consumed
  // Bytes received from the transport while serving this response.  Zero when
  // the connection failed before producing any response byte, which is what
  // tells the pool that a failed *reused* connection may be retried safely.
  std::size_t bytes_received = 0;

  static constexpr std::size_t kChunkBytes = 16384;
  static constexpr std::size_t kMaxHeaderBytes = 256 * 1024;

  // Appends the next chunk from the transport.  Returns false when the peer
  // closed (or the timeout elapsed) with no data.  Reads once rather than
  // filling kChunkBytes: on a persistent connection there are no bytes left to
  // wait for once the current message has been consumed.
  base::Result<bool> Fill()
  {
    const base::Result<std::string> chunk = transport.ReadSome(kChunkBytes);
    if (!chunk) {
      return base::Err(chunk.error());
    }
    const bool got = !chunk.value().empty();
    if (got) {
      bytes_received += chunk.value().size();
    }
    buffer += chunk.value();
    return got;
  }

  // The header block including the trailing CRLFCRLF.
  base::Result<std::string> ReadHeaders()
  {
    for (;;) {
      const std::size_t sep = buffer.find("\r\n\r\n");
      if (sep != std::string::npos) {
        std::string out = buffer.substr(0, sep + 4);
        buffer.erase(0, sep + 4);
        return out;
      }
      if (buffer.size() > kMaxHeaderBytes) {
        return base::Err(base::Error::Parse("response headers too large"));
      }
      const base::Result<bool> got = Fill();
      if (!got) {
        return base::Err(got.error());
      }
      if (!got.value()) {
        return base::Err(base::Error::Parse("truncated response headers"));
      }
    }
  }

  // One line, not including the CRLF terminator.
  base::Result<std::string> ReadLine()
  {
    for (;;) {
      const std::size_t nl = buffer.find("\r\n");
      if (nl != std::string::npos) {
        std::string out = buffer.substr(0, nl);
        buffer.erase(0, nl + 2);
        return out;
      }
      const base::Result<bool> got = Fill();
      if (!got) {
        return base::Err(got.error());
      }
      if (!got.value()) {
        return base::Err(base::Error::Parse("truncated line"));
      }
    }
  }

  // Exactly |count| bytes.
  base::Result<std::string> ReadExactly(std::size_t count)
  {
    if (count > kMaxBodySize) {
      return base::Err(base::Error::Parse("response body too large"));
    }
    std::string out;
    out.reserve(count);
    while (out.size() < count) {
      const std::size_t take = std::min(count - out.size(), buffer.size());
      out += buffer.substr(0, take);
      buffer.erase(0, take);
      if (out.size() == count) {
        return out;
      }
      const base::Result<bool> got = Fill();
      if (!got) {
        return base::Err(got.error());
      }
      if (!got.value()) {
        return base::Err(base::Error::Parse("connection closed mid-body"));
      }
    }
    return out;
  }
};

// Reads a chunked body from |reader| and returns the decoded body.
template <typename Transport>
base::Result<std::string> ReadChunkedBody(ResponseReader<Transport>& reader)
{
  std::string body;
  for (;;) {
    const base::Result<std::string> size_line = reader.ReadLine();
    if (!size_line) {
      return base::Err(size_line.error());
    }
    const std::optional<std::size_t> parsed = ParseChunkSizeLine(size_line.value());
    if (!parsed.has_value()) {
      return base::Err(base::Error::Parse("invalid chunk size"));
    }
    const std::size_t chunk_size = parsed.value();
    if (chunk_size == 0) {
      // Trailer section: read until the empty line.
      for (;;) {
        const base::Result<std::string> trailer = reader.ReadLine();
        if (!trailer) {
          return base::Err(trailer.error());
        }
        if (trailer.value().empty()) {
          return body;
        }
      }
    }
    const base::Result<std::string> data = reader.ReadExactly(chunk_size);
    if (!data) {
      return base::Err(data.error());
    }
    if (data.value().size() > kMaxBodySize - body.size()) {
      return base::Err(base::Error::Parse("response body too large"));
    }
    body += data.value();
    const base::Result<std::string> terminator = reader.ReadLine();
    if (!terminator) {
      return base::Err(terminator.error());
    }
    if (!terminator.value().empty()) {
      return base::Err(base::Error::Parse("malformed chunk terminator"));
    }
  }
}

// One live HTTP/1.1 connection to an origin: a plain TCP socket or a TLS
// socket, plus whatever bytes were read past the previous response.  Owned by
// the client's pool and leased to exactly one request at a time, which is what
// keeps the single-thread-per-connection rule of Socket / TlsSocket intact.
class PooledConnection
{
public:
  PooledConnection(std::unique_ptr<Socket> plain, std::unique_ptr<TlsSocket> tls, int timeout_ms)
      : plain_(std::move(plain)), tls_(std::move(tls)), timeout_ms_(timeout_ms)
  {}

  base::Result<std::size_t> Send(std::string_view data)
  {
    return tls_ != nullptr ? tls_->Send(data) : plain_->Send(data);
  }

  // One read: returns as soon as bytes are available (see ResponseReader).
  base::Result<std::string> ReadSome(std::size_t max_bytes)
  {
    return tls_ != nullptr ? tls_->ReadSome(max_bytes, timeout_ms_)
                           : plain_->ReadSome(max_bytes, timeout_ms_);
  }

  // True when the peer closed this connection (or sent unsolicited bytes)
  // while it was idle in the pool.  A healthy idle keep-alive connection
  // reports false.  Probe errors count as "do not reuse".
  bool IsReusable()
  {
    const base::Result<bool> pending =
        tls_ != nullptr ? tls_->HasPendingInput() : plain_->HasPendingInput();
    return pending.has_value() && !pending.value();
  }

  // Bytes already read past the previous response.  Must be empty for the
  // connection to be pooled again: this client never pipelines, so trailing
  // bytes mean the peer is not speaking the framing we expect.
  std::string leftover;

private:
  std::unique_ptr<Socket> plain_;
  std::unique_ptr<TlsSocket> tls_;
  int timeout_ms_ = 10000;
};

// Dials a fresh connection for |url| (plain TCP, or TCP + TLS handshake).
base::Result<std::unique_ptr<PooledConnection>> DialConnection(const url::Url& url,
                                                               const TlsOptions& options)
{
  if (url.scheme() == "https") {
    const base::Result<TlsOptions> configured =
        TlsOptionsWithEnvironmentProxy(url.host(), url.effective_port(), options);
    if (!configured) {
      return base::Err(configured.error());
    }
    base::Result<TlsSocket> tls =
        TlsSocket::Connect(url.host(), url.effective_port(), configured.value());
    if (!tls) {
      return base::Err(tls.error());
    }
    return base::Ok(std::make_unique<PooledConnection>(
        nullptr, std::make_unique<TlsSocket>(std::move(tls.value())), options.timeout_ms));
  }
  base::Result<Socket> socket = Socket::Connect(url.host(), url.effective_port());
  if (!socket) {
    return base::Err(socket.error());
  }
  return base::Ok(std::make_unique<PooledConnection>(
      std::make_unique<Socket>(std::move(socket.value())), nullptr, options.timeout_ms));
}

// The result of one request/response exchange on one connection.
struct AttemptResult
{
  // base::Result has no default constructor: start from an explicit "not
  // attempted" error that every path below overwrites.
  base::Result<HttpResponse> response = base::Err(base::Error::Network("request not attempted"));
  // At least one byte of the response arrived.  A failure with no byte at all
  // on a *reused* connection means the pooled connection was dead, so the
  // request can be repeated on a fresh one without double-applying anything.
  bool response_started = false;
  // The connection is still in a known-good state and may be pooled again.
  bool reusable = false;
};

// True when |value| contains |token| as a comma-separated header field value
// element (RFC 9110 5.6.1), case-insensitive.
bool HeaderValueHasToken(std::string_view value, std::string_view token)
{
  std::size_t pos = 0;
  while (pos <= value.size()) {
    const std::size_t comma = value.find(',', pos);
    const std::string_view element = TrimView(
        value.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos));
    if (base::AsciiEqualsIgnoreCase(element, token)) {
      return true;
    }
    if (comma == std::string_view::npos) {
      return false;
    }
    pos = comma + 1;
  }
  return false;
}

// Whether the response leaves the connection usable for a further request.
// A `Connection: close` (however it is spelled) always wins; otherwise an
// HTTP/1.1 response is persistent.  An HTTP/1.0 peer that answers with a full
// Content-Length body and no keep-alive header is optimistically pooled: if it
// then closed the socket, the pending-input probe or the retry-once path drops
// it, so the optimistic case costs an optimisation, never correctness.
bool ResponseAllowsReuse(const HttpResponse& response)
{
  for (const HttpHeader& header : response.headers) {
    if (header.name == "connection") {
      if (HeaderValueHasToken(header.value, "close")) {
        return false;
      }
      if (HeaderValueHasToken(header.value, "keep-alive")) {
        return true;
      }
    }
  }
  return true;
}

// Sends |request| over |transport| and parses the raw response.  The connection
// is left byte-clean (no unconsumed bytes) exactly when the result is reusable.
AttemptResult PerformRequest(PooledConnection& transport, const std::string& request)
{
  AttemptResult attempt;
  const base::Result<std::size_t> sent = transport.Send(request);
  if (!sent) {
    attempt.response = base::Err(sent.error());
    return attempt;
  }

  ResponseReader<PooledConnection> reader(transport);
  // A pooled connection normally has nothing buffered; carry anything over so
  // the byte stream stays in order even if a peer sent extra data.
  reader.buffer = std::move(transport.leftover);

  // A failure once the response has started is never retried: the peer may have
  // acted on the request, and a half-read body is not a repeatable exchange.
  const auto fail = [&attempt, &reader](const base::Error& error) {
    attempt.response_started = reader.bytes_received > 0;
    attempt.response = base::Err(error);
    return std::move(attempt);
  };

  const base::Result<std::string> header_block = reader.ReadHeaders();
  if (!header_block) {
    return fail(header_block.error());
  }
  HttpResponse response;
  const base::Result<void> headers = ParseResponseHeaders(header_block.value(), response);
  if (!headers) {
    return fail(headers.error());
  }
  const base::Result<BodyFraming> framing = ClassifyBody(response);
  if (!framing) {
    return fail(framing.error());
  }

  // A delimited response is poolable when nothing of it is left unread and the
  // response itself did not ask for the connection to close.  An until-close
  // body is by definition not poolable (the connection is spent).
  const auto finish = [&attempt, &reader, &response](bool delimited) {
    const bool clean = delimited && reader.buffer.empty();
    attempt.response_started = true;
    attempt.reusable = clean && ResponseAllowsReuse(response);
    attempt.response = base::Ok(std::move(response));
    return std::move(attempt);
  };

  switch (framing.value().kind) {
  case BodyFraming::Kind::kChunked: {
    const base::Result<std::string> body = ReadChunkedBody(reader);
    if (!body) {
      return fail(body.error());
    }
    response.body = std::move(body.value());
    return finish(/*delimited=*/true);
  }
  case BodyFraming::Kind::kContentLength: {
    const base::Result<std::string> body = reader.ReadExactly(framing.value().length);
    if (!body) {
      return fail(body.error());
    }
    response.body = std::move(body.value());
    return finish(/*delimited=*/true);
  }
  case BodyFraming::Kind::kNone:
    response.body.clear();
    return finish(/*delimited=*/true);
  case BodyFraming::Kind::kUntilClose:
    // No framing: read until the connection closes (or the timeout).  The
    // TLS layer treats a missing close_notify as EOF, so this terminates on
    // real-world CDN servers too.
    for (;;) {
      const base::Result<std::string> chunk =
          transport.ReadSome(ResponseReader<PooledConnection>::kChunkBytes);
      if (!chunk) {
        return fail(chunk.error());
      }
      if (chunk.value().empty()) {
        return finish(/*delimited=*/false);
      }
      if (chunk.value().size() > kMaxBodySize - response.body.size()) {
        return fail(base::Error::Parse("response body too large"));
      }
      response.body += chunk.value();
    }
  }
  return attempt;
}

base::Result<HttpResponse> DecodeDataUrl(std::string_view url)
{
  // data:[<mediatype>][;base64],<data>   (RFC 2397)
  constexpr std::string_view kPrefix = "data:";
  if (url.substr(0, kPrefix.size()) != kPrefix) {
    return base::Err(base::Error::InvalidArgument("not a data: URL"));
  }
  if (url.size() > kMaxBodySize) {
    return base::Err(base::Error::Parse("data: URL is too large"));
  }
  const std::size_t comma = url.find(',');
  if (comma == std::string_view::npos) {
    return base::Err(base::Error::InvalidArgument("data: URL has no data"));
  }
  std::string_view metadata = url.substr(kPrefix.size(), comma - kPrefix.size());
  const std::string_view encoded = url.substr(comma + 1);

  // The last parameter may be ";base64" (browsers accept any case).
  bool is_base64 = false;
  const std::size_t last_semicolon = metadata.rfind(';');
  if (last_semicolon != std::string_view::npos &&
      base::ToLower(metadata.substr(last_semicolon + 1)) == "base64") {
    is_base64 = true;
    metadata = metadata.substr(0, last_semicolon);
  }

  // Percent-decoding applies to the data part before any base64 decoding, so
  // "%2B" is a literal '+' in the payload.
  const std::string decoded = url::PercentDecode(encoded);
  std::string body;
  if (is_base64) {
    auto bytes = Base64Decode(decoded);
    if (!bytes.has_value()) {
      return base::Err(bytes.error());
    }
    body = std::move(bytes.value());
  } else {
    body = decoded;
  }
  if (body.size() > kMaxBodySize) {
    return base::Err(base::Error::Parse("data: payload is too large"));
  }

  HttpResponse response;
  response.status_code = 200;
  response.reason = "OK";
  // RFC 2397's default media type when the URL carries no metadata.
  response.headers.push_back(
      {"content-type", std::string(metadata.empty() ? "text/plain;charset=US-ASCII" : metadata)});
  response.body = std::move(body);
  response.final_url = std::string(url);
  return response;
}

namespace {

// Builds the HTTP/1.1 GET request line and headers for |url|.  The request is
// persistent (no `Connection: close`): the connection goes back to the pool
// unless the response says otherwise.
std::string BuildGetRequest(const url::Url& url, const HeaderProvider& extra_headers)
{
  std::string request = "GET ";
  // The request target must be percent-encoded (RFC 3986): raw path/query
  // bytes must not reach the request line (defense in depth against CRLF
  // and header injection even if a caller constructs a malformed Url).
  request += EncodeRequestTarget(url.path());
  if (url.has_query()) {
    request.push_back('?');
    request += EncodeRequestTarget(url.query());
  }
  request += " HTTP/1.1\r\n";
  request += "Host: ";
  request += url.host();
  if (url.port().has_value()) {
    request.push_back(':');
    request += std::to_string(url.port().value());
  }
  request += "\r\n";
  request += "User-Agent: ";
  request += base::GetUserAgent();
  request += "\r\n";
  request += "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n";
  request += "Accept-Language: en-US,en;q=0.9\r\n";
  request += "Accept-Encoding: gzip, deflate\r\n";
  if (extra_headers) {
    for (const HttpHeader& header : extra_headers(url)) {
      // Last line of defense against request splitting: the request target is
      // percent-encoded above, but caller-supplied header names/values (e.g.
      // the Cookie header built from the cookie jar) used to be concatenated
      // into the request verbatim.  A value carrying CR or LF -- reachable
      // from document.cookie before the cookie jar validated cookie-octets --
      // would have injected arbitrary headers or a second request.  Refuse to
      // emit any header that is not a plain RFC 9110 field.
      if (!IsValidHeaderFieldName(header.name) || !IsValidHeaderFieldValue(header.value)) {
        continue;
      }
      request += header.name;
      request += ": ";
      request += header.value;
      request += "\r\n";
    }
  }
  request += "\r\n";
  return request;
}

// Identity of a pool lane: connections are only interchangeable when they lead
// to the same origin *and* were dialed with the same trust/proxy/timeout
// settings (a test's extra CA must never be silently reused for a request that
// did not ask for it, and a connection's read timeout is fixed at dial time).
struct ConnectionKey
{
  std::string scheme;
  std::string host;
  std::uint16_t port = 0;
  int timeout_ms = 10000;
  std::size_t proxy_hash = 0;
  std::size_t trust_anchor_hash = 0;

  bool operator==(const ConnectionKey& other) const
  {
    return scheme == other.scheme && host == other.host && port == other.port &&
           timeout_ms == other.timeout_ms && proxy_hash == other.proxy_hash &&
           trust_anchor_hash == other.trust_anchor_hash;
  }
};

struct ConnectionKeyHash
{
  std::size_t operator()(const ConnectionKey& key) const noexcept
  {
    std::size_t hash = std::hash<std::string>{}(key.scheme);
    const auto mix = [&hash](std::size_t value) {
      // 64-bit golden-ratio mixing keeps multi-field keys from colliding in
      // the low bits (the port and the two hashes are small integers).
      hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    };
    mix(std::hash<std::string>{}(key.host));
    mix(key.port);
    mix(static_cast<std::size_t>(key.timeout_ms));
    mix(key.proxy_hash);
    mix(key.trust_anchor_hash);
    return hash;
  }
};

} // namespace

struct HttpClient::Impl
{
  struct Idle
  {
    std::unique_ptr<PooledConnection> connection;
    std::chrono::steady_clock::time_point idle_since;
  };

  static ConnectionKey KeyFor(const url::Url& url, const TlsOptions& options)
  {
    ConnectionKey key;
    key.scheme = url.scheme();
    key.host = url.host();
    key.port = url.effective_port();
    key.timeout_ms = options.timeout_ms;
    // The *route* to an origin is part of a connection's identity, not just the
    // origin: with HTTPS_PROXY set (and the host not excluded by NO_PROXY) the
    // connection is dialed through the proxy, so it must not be interchangeable
    // with one dialed directly.  Resolving the proxy exactly as the dial does
    // keeps a direct connection from serving a proxied request (and the
    // reverse) when the environment changes -- which is what tests do.
    if (url.scheme() == "https") {
      const base::Result<TlsOptions> effective =
          TlsOptionsWithEnvironmentProxy(url.host(), url.effective_port(), options);
      if (!effective) {
        return key; // the dial reports the same configuration error
      }
      if (!effective.value().proxy_host.empty()) {
        key.proxy_hash = std::hash<std::string>{}(effective.value().proxy_host + ":" +
                                                  std::to_string(effective.value().proxy_port));
      }
    } else if (!options.proxy_host.empty()) {
      key.proxy_hash =
          std::hash<std::string>{}(options.proxy_host + ":" + std::to_string(options.proxy_port));
    }
    if (!options.extra_ca_cert_pem.empty()) {
      key.trust_anchor_hash = std::hash<std::string>{}(options.extra_ca_cert_pem);
    }
    return key;
  }

  // Leases a connection to |key|: the most recently returned healthy idle one,
  // or a fresh dial.  Idle connections the peer closed while they sat in the
  // pool are dropped here rather than costing a request.
  base::Result<std::unique_ptr<PooledConnection>>
  Acquire(const ConnectionKey& key, const url::Url& url, const TlsOptions& options, bool* reused)
  {
    *reused = false;
    for (;;) {
      std::unique_ptr<PooledConnection> candidate;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto lane = idle_.find(key);
        if (lane != idle_.end()) {
          const auto now = std::chrono::steady_clock::now();
          while (!lane->second.empty()) {
            Idle entry = std::move(lane->second.back());
            lane->second.pop_back();
            if (now - entry.idle_since > std::chrono::milliseconds(HttpClient::kIdleTimeoutMs)) {
              ++stats_.connections_discarded;
              continue; // expired: closed by ~unique_ptr, try the next one
            }
            candidate = std::move(entry.connection);
            break;
          }
          if (lane->second.empty()) {
            idle_.erase(lane);
          }
        }
      }
      if (candidate == nullptr) {
        break;
      }
      if (candidate->IsReusable()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.connections_reused;
        *reused = true;
        return base::Ok(std::move(candidate));
      }
      // The peer closed it (or left unexpected bytes): drop it and try the
      // next candidate.
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.connections_discarded;
    }

    base::Result<std::unique_ptr<PooledConnection>> dialed = DialConnection(url, options);
    if (!dialed) {
      return dialed; // dialed.error() carries the TLS/connect failure
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.connections_opened;
    }
    return dialed;
  }

  void Release(const ConnectionKey& key, std::unique_ptr<PooledConnection> connection)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Idle>& lane = idle_[key];
    if (lane.size() >= HttpClient::kMaxIdlePerOrigin) {
      ++stats_.connections_discarded; // destroyed here: the socket closes
      return;
    }
    lane.push_back(Idle{std::move(connection), std::chrono::steady_clock::now()});
  }

  void Discard(std::unique_ptr<PooledConnection> connection)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.connections_discarded;
    connection.reset();
  }

  void CountRequest()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.requests;
  }

  std::mutex mutex_;
  std::unordered_map<ConnectionKey, std::vector<Idle>, ConnectionKeyHash> idle_;
  Stats stats_;
};

HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {}

HttpClient::~HttpClient() = default;

HttpClient::Stats HttpClient::stats() const
{
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  return impl_->stats_;
}

void HttpClient::CloseIdleConnections()
{
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  impl_->idle_.clear();
}

// One request/response exchange, no redirect handling.
base::Result<HttpResponse> HttpClient::GetOnce(const url::Url& url,
                                               const HeaderProvider& extra_headers,
                                               const TlsOptions& tls_options)
{
  if (url.scheme() != "http" && url.scheme() != "https") {
    return base::Err(base::Error::NotImplemented("unsupported URL scheme '" + url.scheme() +
                                                 "' (only http/https)"));
  }
  const std::string request = BuildGetRequest(url, extra_headers);
  const ConnectionKey key = Impl::KeyFor(url, tls_options);
  impl_->CountRequest();

  base::Result<HttpResponse> last = base::Err(base::Error::Network("request was not attempted"));
  for (int attempt = 0; attempt < 2; ++attempt) {
    bool reused = false;
    base::Result<std::unique_ptr<PooledConnection>> lease =
        impl_->Acquire(key, url, tls_options, &reused);
    if (!lease) {
      return base::Err(lease.error());
    }
    AttemptResult result = PerformRequest(*lease.value(), request);
    // Retry exactly once, and only when a *pooled* connection failed before
    // producing a single byte: that is the "the peer closed it while it was
    // idle" case, where repeating an idempotent GET is safe.  A failure with
    // response bytes on the wire, or on a freshly dialed connection, is real.
    const bool retryable =
        !result.response.has_value() && reused && !result.response_started && attempt == 0;
    if (result.reusable) {
      impl_->Release(key, std::move(lease.value()));
    } else {
      impl_->Discard(std::move(lease.value()));
    }
    last = std::move(result.response);
    if (!retryable) {
      return last;
    }
  }
  return last;
}

base::Result<HttpResponse> HttpClient::Get(const url::Url& url,
                                           int redirect_limit,
                                           const HeaderProvider& extra_headers,
                                           const TlsOptions& tls_options)
{
  if (url.scheme() == "data") {
    // A data: URL never touches the network, but every resource loader in the
    // engine (stylesheets, scripts, @font-face, images, fetch()) funnels
    // through HttpGet — decoding it here gives them all support at once.
    // The fragment is not part of the resource, hence Serialize(false).
    return DecodeDataUrl(url.Serialize(/*include_fragment=*/false));
  }

  url::Url current = url;
  for (;;) {
    base::Result<HttpResponse> response = GetOnce(current, extra_headers, tls_options);
    if (!response) {
      return response;
    }
    const int status = response.value().status_code;
    const bool is_redirect =
        status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    if (is_redirect && redirect_limit > 0) {
      const std::string location = response.value().GetHeader("location");
      if (!location.empty()) {
        const base::Result<url::Url> next = url::Url::Parse(location, current);
        if (next) {
          // Refuse to downgrade an https request to plaintext http: silently
          // following such a redirect is SSL stripping.  The caller can retry
          // the Location explicitly if it wants to.
          if (current.scheme() == "https" && next.value().scheme() == "http") {
            return base::Err(
                base::Error::NotImplemented("refusing https->http redirect downgrade"));
          }
          current = next.value();
          --redirect_limit;
          continue;
        }
      }
    }
    // The response is final (or a redirect we are not following): record the
    // URL it came from, then decode the body per Content-Encoding (RFC 7231
    // §3.1.2).  An unsupported coding is an explicit error rather than
    // returning corrupted content.
    HttpResponse out = std::move(response.value());
    out.final_url = current.Serialize();
    const std::string content_encoding = out.GetHeader("content-encoding");
    if (!content_encoding.empty() && !base::AsciiEqualsIgnoreCase(content_encoding, "identity")) {
      const base::Result<std::string> decoded = DecodeContentEncodings(content_encoding, out.body);
      if (!decoded) {
        return base::Err(decoded.error());
      }
      out.body = std::move(decoded.value());
    }
    return base::Ok(std::move(out));
  }
}

HttpClient& SharedHttpClient()
{
  // One pool for the whole process: every existing HttpGet caller (stylesheet
  // / script / image / font / fetch loaders) gets connection reuse without
  // threading a client through the engine.  Function-local static so the
  // process-wide pool is initialized on first use (no static init order
  // dependency); it is destroyed at exit, after the engine's threads are gone.
  static HttpClient shared;
  return shared;
}

base::Result<HttpResponse> HttpGet(const url::Url& url,
                                   int redirect_limit,
                                   const HeaderProvider& extra_headers,
                                   const TlsOptions& tls_options)
{
  return SharedHttpClient().Get(url, redirect_limit, extra_headers, tls_options);
}

} // namespace neko::network
