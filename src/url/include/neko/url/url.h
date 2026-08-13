#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "neko/base/status.h"

namespace neko::url {

// A parsed absolute URL.
//
// Modeled on WHATWG URL / RFC 3986.  Components are stored normalized:
// the scheme and host are lowercased, the path has dot segments removed,
// and special schemes (http/https/ws/wss/ftp) always carry an authority.
//
// Limitations (Phase 1): IDNA for non-ASCII hosts, opaque paths for
// non-special schemes and `file:` URLs are not supported yet.
class Url {
 public:
  // Parses an absolute URL (a scheme is required).
  static base::Result<Url> Parse(std::string_view input);

  // Parses |input|, resolving relative references against |base|.
  static base::Result<Url> Parse(std::string_view input, const Url& base);

  const std::string& scheme() const { return scheme_; }
  const std::string& host() const { return host_; }
  const std::string& username() const { return username_; }
  const std::string& password() const { return password_; }

  // Explicit port; nullopt when not specified.
  const std::optional<uint16_t>& port() const { return port_; }
  // Port to use for connections (scheme default when unspecified).
  uint16_t effective_port() const;

  const std::string& path() const { return path_; }
  const std::string& query() const { return query_; }
  const std::string& fragment() const { return fragment_; }

  bool has_authority() const { return has_authority_; }
  bool has_query() const { return has_query_; }
  bool has_fragment() const { return has_fragment_; }

  // Serialized form.  The fragment is omitted unless |include_fragment|.
  std::string Serialize(bool include_fragment = false) const;

  // Origin, e.g. "https://example.com" or "https://example.com:8443".
  // Default ports are omitted, matching browser origin serialization.
  std::string Origin() const;

 private:
  Url() = default;

  // Parser helpers (defined in url.cpp) need access to the component fields.
  friend bool ParseAuthority(std::string_view authority, Url& url);
  friend void SplitPathQueryFragment(std::string_view rest, Url& url);
  friend base::Result<Url> ParseAbsoluteInternal(std::string_view input);
  friend base::Result<Url> ResolveNetworkPath(std::string_view input, const Url& base);
  friend base::Result<Url> ResolveRelative(std::string_view input, const Url& base);

  std::string scheme_;
  bool has_authority_ = false;
  std::string username_;
  std::string password_;
  std::string host_;
  std::optional<uint16_t> port_;
  std::string path_;
  bool has_query_ = false;
  std::string query_;
  bool has_fragment_ = false;
  std::string fragment_;
};

// Percent-encoding helpers (also used by the network layer).
std::string PercentEncode(std::string_view input);
std::string PercentDecode(std::string_view input);

// Default port for a special scheme, or 0 for unknown schemes.
uint16_t DefaultPortForScheme(std::string_view scheme);

// True for schemes that always use an authority (http/https/ws/wss/ftp).
bool IsSpecialScheme(std::string_view scheme);

}  // namespace neko::url
