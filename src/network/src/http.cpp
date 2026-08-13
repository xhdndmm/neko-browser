#include "neko/network/http.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

#include "neko/base/string_util.h"
#include "neko/network/socket.h"

namespace neko::network {
namespace {

std::string_view TrimView(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

// Decodes a chunked transfer body into |out.body|.
base::Result<void> DecodeChunked(std::string_view data, HttpResponse& out) {
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

}  // namespace

std::string HttpResponse::GetHeader(std::string_view name) const {
  for (const HttpHeader& header : headers) {
    if (base::AsciiEqualsIgnoreCase(header.name, name)) {
      return header.value;
    }
  }
  return {};
}

base::Result<HttpResponse> ParseHttpResponse(std::string_view raw) {
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
  const std::string_view code_str =
      status_line.substr(sp1 + 1, sp2 == std::string_view::npos ? std::string_view::npos
                                                                : sp2 - sp1 - 1);
  if (code_str.empty()) {
    return base::Err(base::Error::Parse("missing status code"));
  }
  int code = 0;
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
  const std::string transfer_encoding = response.GetHeader("transfer-encoding");
  if (base::AsciiEqualsIgnoreCase(transfer_encoding, "chunked")) {
    const base::Result<void> decoded = DecodeChunked(body, response);
    if (!decoded) {
      return base::Err(decoded.error());
    }
    return response;
  }
  const std::string content_length = response.GetHeader("content-length");
  if (!content_length.empty()) {
    std::size_t length = 0;
    bool valid = true;
    for (const char c : content_length) {
      if (c < '0' || c > '9') {
        valid = false;
        break;
      }
      length = length * 10 + static_cast<std::size_t>(c - '0');
    }
    if (!valid) {
      return base::Err(base::Error::Parse("invalid content-length"));
    }
    response.body = std::string(body.substr(0, length));
  } else {
    response.body = std::string(body);
  }
  return response;
}

base::Result<HttpResponse> HttpGet(const url::Url& url, int redirect_limit,
                                  const HeaderProvider& extra_headers) {
  if (url.scheme() != "http") {
    return base::Err(base::Error::NotImplemented(
        "only http:// is supported; https:// requires TLS (planned)"));
  }

  std::string request = "GET ";
  request += url.path();
  if (url.has_query()) {
    request.push_back('?');
    request += url.query();
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
  if (extra_headers) {
    for (const HttpHeader& header : extra_headers(url)) {
      request += header.name;
      request += ": ";
      request += header.value;
      request += "\r\n";
    }
  }
  request += "\r\n";

  base::Result<Socket> socket = Socket::Connect(url.host(), url.effective_port());
  if (!socket) {
    return base::Err(socket.error());
  }
  const base::Result<std::size_t> sent = socket.value().Send(request);
  if (!sent) {
    return base::Err(sent.error());
  }
  const base::Result<std::string> raw = socket.value().ReceiveAll();
  if (!raw) {
    return base::Err(raw.error());
  }
  base::Result<HttpResponse> response = ParseHttpResponse(raw.value());
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
        return HttpGet(next.value(), redirect_limit - 1, extra_headers);
      }
    }
  }

  // Compression is not supported yet; be explicit rather than returning
  // corrupted content.
  const std::string content_encoding = response.value().GetHeader("content-encoding");
  if (!content_encoding.empty() && !base::AsciiEqualsIgnoreCase(content_encoding, "identity")) {
    return base::Err(base::Error::NotImplemented(
        "content-encoding '" + content_encoding + "' is not supported yet"));
  }
  return response;
}

}  // namespace neko::network
