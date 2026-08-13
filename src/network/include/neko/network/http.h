#pragma once

#include <functional>
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

// Provides per-request extra headers (e.g. a Cookie header computed by the
// browser layer).  Re-invoked for every redirect hop so cookies stay scoped
// to the actual request host.
using HeaderProvider = std::function<std::vector<HttpHeader>(const url::Url&)>;

// Sends a GET request and returns the final response, following redirects
// (301/302/303/307/308) up to |redirect_limit| times.  |extra_headers| is
// consulted for each hop.  Only http:// is supported; https:// returns a
// NOT IMPLEMENTED error (TLS is future work).
base::Result<HttpResponse> HttpGet(const url::Url& url, int redirect_limit = 5,
                                   const HeaderProvider& extra_headers = {});

}  // namespace neko::network
