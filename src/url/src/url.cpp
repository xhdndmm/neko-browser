#include "neko/url/url.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace neko::url {
namespace {

constexpr std::string_view kHexDigits = "0123456789ABCDEF";

bool IsAsciiAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }

char ToLowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

std::string ToLowerAscii(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    out.push_back(ToLowerAscii(c));
  }
  return out;
}

int HexValue(char c) {
  if (IsAsciiDigit(c)) {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

bool IsUnreserved(char c) {
  return IsAsciiAlpha(c) || IsAsciiDigit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

// Characters allowed in a (non-bracketed) host.  Forbids whitespace, control
// characters and path delimiters; '%' is allowed for percent-encoded hosts.
bool IsValidHostChar(char c) {
  return IsUnreserved(c) || c == '!' || c == '$' || c == '&' || c == '\'' || c == '(' ||
         c == ')' || c == '*' || c == '+' || c == ',' || c == ';' || c == '=' || c == '%';
}

// True when the host is an IPv6 literal (contains ':'), which must be
// bracketed when serialized.
bool IsBracketedHost(std::string_view host) {
  return host.find(':') != std::string_view::npos;
}

bool IsSchemeChar(char c, bool first) {
  if (first) {
    return IsAsciiAlpha(c);
  }
  return IsAsciiAlpha(c) || IsAsciiDigit(c) || c == '+' || c == '-' || c == '.';
}

// Parses a decimal port; returns false for empty or out-of-range input.
bool ParsePort(std::string_view text, std::optional<uint16_t>& out) {
  if (text.empty()) {
    return false;
  }
  uint32_t value = 0;
  for (const char c : text) {
    if (!IsAsciiDigit(c)) {
      return false;
    }
    value = value * 10 + static_cast<uint32_t>(c - '0');
    if (value > 65535) {
      return false;
    }
  }
  out = static_cast<uint16_t>(value);
  return true;
}

void RemoveLastSegment(std::string& output) {
  const size_t slash = output.rfind('/');
  if (slash == std::string::npos) {
    output.clear();
  } else {
    output.erase(slash);
  }
}

// RFC 3986 5.2.4 remove_dot_segments.
std::string RemoveDotSegments(std::string_view path) {
  std::string input(path);
  std::string output;
  while (!input.empty()) {
    if (input.rfind("../", 0) == 0) {
      input.erase(0, 3);
    } else if (input.rfind("./", 0) == 0) {
      input.erase(0, 2);
    } else if (input.rfind("/./", 0) == 0) {
      input.erase(0, 2);  // "/./" -> "/"
    } else if (input == "/.") {
      input = "/";
    } else if (input.rfind("/../", 0) == 0) {
      input.erase(0, 3);  // "/../" -> "/"
      RemoveLastSegment(output);
    } else if (input == "/..") {
      input = "/";
      RemoveLastSegment(output);
    } else if (input == "." || input == "..") {
      input.clear();
    } else {
      // Move the first path segment (including a leading '/') to output.
      const size_t seg_end = input.find('/', 1);
      if (seg_end == std::string::npos) {
        output += input;
        input.clear();
      } else {
        output += input.substr(0, seg_end);
        input.erase(0, seg_end);
      }
    }
  }
  return output;
}

std::string SerializeHost(std::string_view host) {
  if (IsBracketedHost(host)) {
    std::string out;
    out.push_back('[');
    out += host;
    out.push_back(']');
    return out;
  }
  return std::string(host);
}

}  // namespace

// Parses the authority (userinfo@host:port) into |url|.
bool ParseAuthority(std::string_view authority, Url& url) {
  std::string_view hostport = authority;

  const size_t at = authority.rfind('@');
  if (at != std::string_view::npos) {
    const std::string_view userinfo = authority.substr(0, at);
    hostport = authority.substr(at + 1);
    const size_t colon = userinfo.find(':');
    if (colon != std::string_view::npos) {
      url.username_ = std::string(userinfo.substr(0, colon));
      url.password_ = std::string(userinfo.substr(colon + 1));
    } else {
      url.username_ = std::string(userinfo);
    }
  }

  if (hostport.empty()) {
    return true;  // Caller rejects empty hosts for special schemes.
  }

  if (hostport[0] == '[') {
    // IPv6 literal (kept verbatim; full validation is future work).
    const size_t close = hostport.find(']');
    if (close == std::string_view::npos) {
      return false;
    }
    url.host_ = std::string(hostport.substr(1, close - 1));
    const std::string_view rest = hostport.substr(close + 1);
    if (rest.empty()) {
      return true;
    }
    if (rest[0] != ':') {
      return false;
    }
    return ParsePort(rest.substr(1), url.port_);
  }

  const size_t colon = hostport.rfind(':');
  if (colon == std::string_view::npos) {
    url.host_ = ToLowerAscii(hostport);
  } else {
    url.host_ = ToLowerAscii(hostport.substr(0, colon));
    if (!ParsePort(hostport.substr(colon + 1), url.port_)) {
      return false;
    }
  }
  for (const char c : url.host_) {
    if (!IsValidHostChar(c)) {
      return false;
    }
  }
  return true;
}

// True when |c| is an ASCII control character (0x00-0x1F or 0x7F).  These
// must never appear raw in a URL's path/query: they are not valid in a
// request target and would let untrusted input inject bytes into the HTTP
// request line (CRLF injection).
bool IsControlChar(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return u < 0x20 || u == 0x7F;
}

// Splits the remainder of a URL (after scheme or authority) into
// path / query / fragment.  Rejects control characters anywhere in the
// path/query portion.
base::Result<void> SplitPathQueryFragment(std::string_view rest, Url& url) {
  for (const char c : rest) {
    if (IsControlChar(c)) {
      return base::Err(base::Error::Parse("URL contains a control character"));
    }
  }

  const size_t question = rest.find('?');
  const size_t hash = rest.find('#');
  const size_t path_end = std::min(question == std::string_view::npos ? rest.size() : question,
                                   hash == std::string_view::npos ? rest.size() : hash);

  url.path_ = RemoveDotSegments(rest.substr(0, path_end));
  if (url.has_authority_ && url.path_.empty()) {
    url.path_ = "/";
  }

  if (question != std::string_view::npos) {
    url.has_query_ = true;
    const size_t query_end = hash == std::string_view::npos ? rest.size() : hash;
    url.query_ = std::string(rest.substr(question + 1, query_end - question - 1));
  }
  if (hash != std::string_view::npos) {
    url.has_fragment_ = true;
    url.fragment_ = std::string(rest.substr(hash + 1));
  }
  return base::Ok();
}

base::Result<Url> ParseAbsoluteInternal(std::string_view input) {
  const size_t colon = input.find(':');
  if (colon == std::string_view::npos) {
    return base::Err(base::Error::Parse("URL is missing a scheme"));
  }
  const std::string_view scheme = input.substr(0, colon);
  if (scheme.empty() || !IsSchemeChar(scheme[0], /*first=*/true)) {
    return base::Err(base::Error::Parse("URL scheme must start with a letter"));
  }
  for (const char c : scheme) {
    if (!IsSchemeChar(c, /*first=*/false)) {
      return base::Err(base::Error::Parse("URL scheme contains an invalid character"));
    }
  }

  Url url;
  url.scheme_ = ToLowerAscii(scheme);
  std::string_view rest = input.substr(colon + 1);

  if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/') {
    url.has_authority_ = true;
    rest = rest.substr(2);
    const size_t authority_end = rest.find_first_of("/?#");
    const std::string_view authority = rest.substr(0, authority_end);
    if (!ParseAuthority(authority, url)) {
      return base::Err(base::Error::Parse("invalid URL authority"));
    }
    if (IsSpecialScheme(url.scheme_) && url.host_.empty()) {
      return base::Err(base::Error::Parse("URL has an empty host"));
    }
    rest = (authority_end == std::string_view::npos) ? std::string_view{}
                                                     : rest.substr(authority_end);
  } else if (IsSpecialScheme(url.scheme_)) {
    // Special schemes always use "//".
    return base::Err(base::Error::Parse("special scheme requires '//'"));
  }

  const base::Result<void> split = SplitPathQueryFragment(rest, url);
  if (!split) {
    return base::Err(split.error());
  }
  return url;
}

// Resolves an input that starts with "//" (network-path reference).
base::Result<Url> ResolveNetworkPath(std::string_view input, const Url& base) {
  Url url;
  url.scheme_ = base.scheme();
  url.has_authority_ = true;
  std::string_view rest = input.substr(2);
  const size_t authority_end = rest.find_first_of("/?#");
  const std::string_view authority = rest.substr(0, authority_end);
  if (!ParseAuthority(authority, url)) {
    return base::Err(base::Error::Parse("invalid URL authority"));
  }
  if (IsSpecialScheme(url.scheme_) && url.host_.empty()) {
    return base::Err(base::Error::Parse("URL has an empty host"));
  }
  rest = (authority_end == std::string_view::npos) ? std::string_view{}
                                                    : rest.substr(authority_end);
  const base::Result<void> split = SplitPathQueryFragment(rest, url);
  if (!split) {
    return base::Err(split.error());
  }
  return url;
}

std::string MergePaths(const Url& base, std::string_view relative) {
  if (base.has_authority() && base.path().empty()) {
    return std::string("/") + std::string(relative);
  }
  const std::string& base_path = base.path();
  const size_t last_slash = base_path.rfind('/');
  if (last_slash == std::string::npos) {
    return std::string(relative);
  }
  return base_path.substr(0, last_slash + 1) + std::string(relative);
}

// RFC 3986 5.2.2 relative reference resolution.
base::Result<Url> ResolveRelative(std::string_view input, const Url& base) {
  if (input.empty()) {
    return base;
  }
  for (const char c : input) {
    if (IsControlChar(c)) {
      return base::Err(base::Error::Parse("URL contains a control character"));
    }
  }
  if (input[0] == '?') {
    Url t = base;
    t.query_ = std::string(input.substr(1));
    t.has_query_ = true;
    t.has_fragment_ = false;
    t.fragment_.clear();
    return t;
  }
  if (input[0] == '#') {
    Url t = base;
    t.fragment_ = std::string(input.substr(1));
    t.has_fragment_ = true;
    return t;
  }

  Url t;
  t.scheme_ = base.scheme();
  t.has_authority_ = base.has_authority();
  t.username_ = base.username();
  t.password_ = base.password();
  t.host_ = base.host();
  t.port_ = base.port();

  const size_t question = input.find('?');
  const size_t hash = input.find('#');
  const size_t path_end = std::min(question == std::string_view::npos ? input.size() : question,
                                   hash == std::string_view::npos ? input.size() : hash);
  const std::string_view path = input.substr(0, path_end);

  if (path.empty()) {
    t.path_ = base.path();
  } else if (path[0] == '/') {
    t.path_ = std::string(path);
  } else {
    t.path_ = MergePaths(base, path);
  }
  t.path_ = RemoveDotSegments(t.path_);
  if (t.has_authority_ && t.path_.empty()) {
    t.path_ = "/";
  }

  if (question != std::string_view::npos) {
    t.has_query_ = true;
    const size_t query_end = hash == std::string_view::npos ? input.size() : hash;
    t.query_ = std::string(input.substr(question + 1, query_end - question - 1));
  }
  if (hash != std::string_view::npos) {
    t.has_fragment_ = true;
    t.fragment_ = std::string(input.substr(hash + 1));
  }
  return t;
}

base::Result<Url> Url::Parse(std::string_view input) {
  return ParseAbsoluteInternal(input);
}

base::Result<Url> Url::Parse(std::string_view input, const Url& base) {
  // Absolute references: a scheme appears before any of '/', '?', '#'.
  const size_t colon = input.find(':');
  const size_t delim = input.find_first_of("/?#");
  if (colon != std::string_view::npos && colon > 0 && (delim == std::string_view::npos || colon < delim)) {
    bool scheme_ok = IsAsciiAlpha(input[0]);
    for (size_t i = 1; i < colon && scheme_ok; ++i) {
      scheme_ok = IsSchemeChar(input[i], /*first=*/false);
    }
    if (scheme_ok) {
      return ParseAbsoluteInternal(input);
    }
    // Otherwise the ':' is part of a relative path segment.
  }

  if (input.size() >= 2 && input[0] == '/' && input[1] == '/') {
    return ResolveNetworkPath(input, base);
  }
  return ResolveRelative(input, base);
}

uint16_t Url::effective_port() const {
  if (port_.has_value()) {
    return port_.value();
  }
  return DefaultPortForScheme(scheme_);
}

std::string Url::Serialize(bool include_fragment) const {
  std::string out = scheme_;
  out.push_back(':');
  if (has_authority_) {
    out += "//";
    if (!username_.empty() || !password_.empty()) {
      out += username_;
      if (!password_.empty()) {
        out.push_back(':');
        out += password_;
      }
      out.push_back('@');
    }
    out += SerializeHost(host_);
    if (port_.has_value() && port_.value() != DefaultPortForScheme(scheme_)) {
      out.push_back(':');
      out += std::to_string(port_.value());
    }
  }
  out += path_;
  if (has_query_) {
    out.push_back('?');
    out += query_;
  }
  if (include_fragment && has_fragment_) {
    out.push_back('#');
    out += fragment_;
  }
  return out;
}

std::string Url::Origin() const {
  std::string out = scheme_;
  out += "://";
  out += SerializeHost(host_);
  const uint16_t default_port = DefaultPortForScheme(scheme_);
  if (port_.has_value() && port_.value() != default_port) {
    out.push_back(':');
    out += std::to_string(port_.value());
  }
  return out;
}

std::string PercentEncode(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (const char ch : input) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (IsUnreserved(static_cast<char>(c))) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHexDigits[c >> 4]);
      out.push_back(kHexDigits[c & 0x0F]);
    }
  }
  return out;
}

std::string PercentDecode(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%' && i + 2 < input.size()) {
      const int hi = HexValue(input[i + 1]);
      const int lo = HexValue(input[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(input[i]);
  }
  return out;
}

uint16_t DefaultPortForScheme(std::string_view scheme) {
  if (scheme == "http" || scheme == "ws") {
    return 80;
  }
  if (scheme == "https" || scheme == "wss") {
    return 443;
  }
  if (scheme == "ftp") {
    return 21;
  }
  return 0;
}

bool IsSpecialScheme(std::string_view scheme) {
  return scheme == "http" || scheme == "https" || scheme == "ws" || scheme == "wss" ||
         scheme == "ftp";
}

}  // namespace neko::url
