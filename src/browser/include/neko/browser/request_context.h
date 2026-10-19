#pragma once

// The per-request context the transport needs beyond the URL itself.

#include <string>
#include <utility>
#include <vector>

namespace neko::browser {

// Everything a fetch caller knows about a request that the URL cannot express:
// headers it wants verbatim and the initiator provenance the same-site cookie
// filter is defined on (RFC 6265bis §5.4.2).  Subresource and script fetches
// pass the containing page's site; a navigation passes the PREVIOUS document's
// site and marks itself top-level.
struct RequestContext
{
  // Headers to send verbatim (the CORS Origin among them).
  std::vector<std::pair<std::string, std::string>> extra_headers;
  // The initiator's site (security::SiteForCookies).  Empty means same-site or
  // unknown, which imposes no SameSite restriction.
  std::string initiator_site;
  // True for top-level navigations (cross-site Lax cookies may ride them).
  bool top_level_navigation = false;
  // The HTTP method and (for methods that carry one) the request body.
  // Resource fetches are "GET" with an empty body; a form submitted with
  // method="post" is a top-level navigation whose body is the
  // application/x-www-form-urlencoded entry list (HTML §4.10.21.5).
  std::string method = "GET";
  std::string body;
  std::string content_type;
};

} // namespace neko::browser
