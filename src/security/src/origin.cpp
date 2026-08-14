// neko::security::Origin — the origin model (scheme + host + effective port)
// behind the Same-Origin Policy.  Phase 10 milestone 1; see origin.h and
// docs/security/security-model.md for the scope.

#include "neko/security/origin.h"

namespace neko::security {

Origin Origin::FromUrl(const url::Url& url)
{
  if (!url::IsSpecialScheme(url.scheme())) {
    return Opaque();
  }
  Origin origin;
  origin.scheme = url.scheme();
  origin.host = url.host();
  origin.port = url.effective_port();
  return origin;
}

Origin Origin::Opaque()
{
  Origin origin;
  origin.opaque = true;
  return origin;
}

bool Origin::IsSameOrigin(const Origin& other) const
{
  if (opaque || other.opaque) {
    return false;
  }
  return scheme == other.scheme && host == other.host && port == other.port;
}

std::string Origin::Serialize() const
{
  if (opaque) {
    return "null";
  }
  std::string out = scheme + "://" + host;
  const std::uint16_t default_port = url::DefaultPortForScheme(scheme);
  if (port != 0 && port != default_port) {
    out += ":" + std::to_string(port);
  }
  return out;
}

bool Origin::operator==(const Origin& other) const
{
  return opaque == other.opaque && scheme == other.scheme && host == other.host &&
         port == other.port;
}

} // namespace neko::security
