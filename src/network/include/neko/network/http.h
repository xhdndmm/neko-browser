#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "neko/base/status.h"
#include "neko/url/url.h"

namespace neko::network {

struct HttpHeader {
  std::string name;  // lowercased
  std::string value;
};

// A parsed HTTP/1.1 response.
struct HttpResponse {
  int status_code = 0;
  std::string reason;
  std::vector<HttpHeader> headers;
  std::string body;

  // First header value matching |name| (case-insensitive), or empty.
  std::string GetHeader(std::string_view name) const;
};

// Parses a raw HTTP/1.1 response (status line, headers, body).  Handles
// Content-Length and chunked Transfer-Encoding.
base::Result<HttpResponse> ParseHttpResponse(std::string_view raw);

// Sends a GET request and returns the final response, following redirects
// (301/302/303/307/308) up to |redirect_limit| times.  Only http:// is
// supported; https:// returns a NOT IMPLEMENTED error (TLS is future work).
base::Result<HttpResponse> HttpGet(const url::Url& url, int redirect_limit = 5);

}  // namespace neko::network
