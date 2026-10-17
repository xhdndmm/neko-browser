#pragma once

// The "site" of a resource: the scheme + registrable domain pair that the
// SameSite cookie rule and same-site checks are defined on.  An origin is a
// (scheme, host, port) triple; a site is coarser -- every subdomain of the
// same registrable domain shares it.
//
// RFC 6265bis §5.2 defines the "site for cookies" specifically:
//   * the scheme is "https" for any potentially trustworthy origin (so
//     http://localhost counts as https) and "http" otherwise;
//   * the domain is the registrable domain (Public Suffix List), or the host
//     itself when the host is an IP address or has no registrable domain.

#include "neko/url/url.h"

#include <string>

namespace neko::security {

// The site of |url| for cookie purposes ("scheme://registrable-domain").
// Empty for schemes the model does not cover (opaque origins, data:, ...).
std::string SiteForCookies(const url::Url& url);

// True when |a| and |b| have the same site for cookie purposes.  Two empty
// sites are NOT the same site (an unknown initiator never matches).
bool IsSameSiteForCookies(const url::Url& a, const url::Url& b);

} // namespace neko::security
