#include "neko/network/http.h"

#include "neko/base/string_util.h"
#include "neko/network/compression.h"
#include "neko/network/socket.h"
#include "neko/network/tls_socket.h"

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace neko::network {
namespace {

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
    const std::size_t hex_end = size_line.find(';');
    const std::string_view hex = size_line.substr(0, hex_end);
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
      // Guard against size_t wraparound: chunk_size * 16 + digit must not
      // overflow, otherwise a crafted chunk size could wrap to 0 or a small
      // value and silently truncate / misparse the response body.
      if (chunk_size > (SIZE_MAX - static_cast<std::size_t>(digit)) / 16) {
        return base::Err(base::Error::Parse("chunk size too large"));
      }
      chunk_size = chunk_size * 16 + static_cast<std::size_t>(digit);
    }
    if (!valid) {
      return base::Err(base::Error::Parse("invalid chunk size"));
    }
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

base::Result<HttpResponse> ParseHttpResponse(std::string_view raw)
{
  HttpResponse response;

  const std::size_t line_end = raw.find("\r\n");
  if (line_end == std::string_view::npos) {
    return base::Err(base::Error::Parse("missing status line"));
  }
  const std::string_view status_line = raw.substr(0, line_end);
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
    const std::size_t end = raw.find("\r\n", pos);
    if (end == std::string_view::npos) {
      return base::Err(base::Error::Parse("truncated header section"));
    }
    if (end == pos) {
      pos += 2;
      break;
    }
    const std::string_view line = raw.substr(pos, end - pos);
    const std::size_t colon = line.find(':');
    if (colon != std::string_view::npos) {
      HttpHeader header;
      header.name = base::ToLower(line.substr(0, colon));
      header.value = std::string(TrimView(line.substr(colon + 1)));
      response.headers.push_back(std::move(header));
    }
    pos = end + 2;
  }

  const std::string_view body = raw.substr(pos);
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
    const base::Result<void> decoded = DecodeChunked(body, response);
    if (!decoded) {
      return base::Err(decoded.error());
    }
    return response;
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
    response.body = std::string(body.substr(0, length.value()));
  } else {
    response.body = std::string(body);
  }
  return response;
}

// Sends |request| over a transport (Socket or TlsSocket) and parses the raw
// response.  The two transports expose the same Send/ReceiveAll interface.
template <typename Transport>
base::Result<HttpResponse> PerformRequest(Transport& transport, const std::string& request)
{
  const base::Result<std::size_t> sent = transport.Send(request);
  if (!sent) {
    return base::Err(sent.error());
  }
  const base::Result<std::string> raw = transport.ReceiveAll();
  if (!raw) {
    return base::Err(raw.error());
  }
  return ParseHttpResponse(raw.value());
}

base::Result<HttpResponse> HttpGet(const url::Url& url,
                                   int redirect_limit,
                                   const HeaderProvider& extra_headers,
                                   const TlsOptions& tls_options)
{
  if (url.scheme() != "http" && url.scheme() != "https") {
    return base::Err(base::Error::NotImplemented("unsupported URL scheme '" + url.scheme() +
                                                 "' (only http/https)"));
  }

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
  request += "Connection: close\r\n";
  request += "User-Agent: neko-browser/0.1.0\r\n";
  request += "Accept: text/html,application/xhtml+xml\r\n";
  request += "Accept-Encoding: gzip, deflate\r\n";
  if (extra_headers) {
    for (const HttpHeader& header : extra_headers(url)) {
      request += header.name;
      request += ": ";
      request += header.value;
      request += "\r\n";
    }
  }
  request += "\r\n";

  // Fetch over the scheme-appropriate transport (plain TCP for http, TLS
  // for https) and parse the response.
  base::Result<HttpResponse> response = [&]() -> base::Result<HttpResponse> {
    if (url.scheme() == "https") {
      base::Result<TlsSocket> tls =
          TlsSocket::Connect(url.host(), url.effective_port(), tls_options);
      if (!tls) {
        return base::Err(tls.error());
      }
      return PerformRequest(tls.value(), request);
    }
    base::Result<Socket> socket = Socket::Connect(url.host(), url.effective_port());
    if (!socket) {
      return base::Err(socket.error());
    }
    return PerformRequest(socket.value(), request);
  }();
  if (!response) {
    return base::Err(response.error());
  }

  // Follow redirects.
  const int status = response.value().status_code;
  if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
      redirect_limit > 0) {
    const std::string location = response.value().GetHeader("location");
    if (!location.empty()) {
      const base::Result<url::Url> next = url::Url::Parse(location, url);
      if (next) {
        return HttpGet(next.value(), redirect_limit - 1, extra_headers, tls_options);
      }
    }
  }

  // Record the URL this response came from.  Redirects return their own
  // (already final) response above, so reaching here means |url| is final.
  response.value().final_url = url.Serialize();

  // Decode the body per Content-Encoding (RFC 7231 §3.1.2).  An unsupported
  // coding is an explicit error rather than returning corrupted content.
  const std::string content_encoding = response.value().GetHeader("content-encoding");
  if (!content_encoding.empty() && !base::AsciiEqualsIgnoreCase(content_encoding, "identity")) {
    const base::Result<std::string> decoded =
        DecodeContentEncodings(content_encoding, response.value().body);
    if (!decoded) {
      return base::Err(decoded.error());
    }
    response.value().body = std::move(decoded.value());
  }
  return response;
}

} // namespace neko::network
