// The site model (RFC 6265bis §5.2 "site for cookies"): scheme + registrable
// domain, with the scheme collapsed to https for potentially trustworthy
// origins and http otherwise.

#include "neko/security/site.h"

#include "neko/security/public_suffix.h"

namespace neko::security {
namespace {

// Potentially trustworthy (Secure Contexts §3.1, the subset that matters for
// cookie sites): https/wss, and the localhost family over http/ws.
bool IsPotentiallyTrustworthy(const url::Url& url)
{
  const std::string& scheme = url.scheme();
  if (scheme == "https" || scheme == "wss") {
    return true;
  }
  if (scheme != "http" && scheme != "ws") {
    return false;
  }
  const std::string& host = url.host();
  if (host == "localhost" || host == "127.0.0.1" || host == "::1" || host == "[::1]") {
    return true;
  }
  return host.size() > 10 && host.compare(host.size() - 10, 10, ".localhost") == 0;
}

} // namespace

std::string SiteForCookies(const url::Url& url)
{
  const std::string& scheme = url.scheme();
  if (scheme != "http" && scheme != "https" && scheme != "ws" && scheme != "wss") {
    return ""; // opaque or unsupported scheme: no site
  }
  const std::string& host = url.host();
  if (host.empty()) {
    return "";
  }
  std::string domain = RegistrableDomain(host);
  if (domain.empty()) {
    // An IP literal or a bare public suffix is its own site.
    domain = host;
  }
  return (IsPotentiallyTrustworthy(url) ? std::string("https") : std::string("http")) + "://" +
         domain;
}

bool IsSameSiteForCookies(const url::Url& a, const url::Url& b)
{
  const std::string site_a = SiteForCookies(a);
  if (site_a.empty()) {
    return false;
  }
  return site_a == SiteForCookies(b);
}

} // namespace neko::security
