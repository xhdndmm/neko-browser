#pragma once

#include "neko/url/url.h"

#include <cstdint>
#include <string>

namespace neko::security {

// The origin of a resource (scheme + host + effective port), following the
// HTML standard's origin definition.  Two origins are the same origin when
// their scheme, host and (effective) port all match; this is the basis for
// the Same-Origin Policy.
//
// Phase 10 milestone 1: the origin model itself.  SOP enforcement on network
// reads (fetch/XHR), CORS, CSP and the cookie/secure-context rules are
// documented as future work — see docs/security/security-model.md.
struct Origin
{
  std::string scheme;
  std::string host;
  // Effective port: the explicit port, else the scheme default (80 for
  // http, 443 for https, ...).  0 for non-special schemes.
  std::uint16_t port = 0;
  // Opaque origins (data:, blob:, ...) are never same-origin with anything,
  // including themselves.
  bool opaque = false;

  // Derives the origin of |url|.  Special schemes (http/https/ws/wss/ftp)
  // yield their tuple; everything else (including file:, which the URL
  // parser does not support yet) yields an opaque origin.
  static Origin FromUrl(const url::Url& url);

  // An opaque origin (no tuple).
  static Origin Opaque();

  [[nodiscard]] bool IsOpaque() const
  {
    return opaque;
  }

  // The same-origin test (HTML standard, "same origin").
  [[nodiscard]] bool IsSameOrigin(const Origin& other) const;

  // Serialized origin, e.g. "https://example.com" or "https://example.com:8443".
  // Opaque origins serialize as "null" (matching the HTML standard).
  [[nodiscard]] std::string Serialize() const;

  bool operator==(const Origin& other) const;
  bool operator!=(const Origin& other) const
  {
    return !(*this == other);
  }
};

} // namespace neko::security
