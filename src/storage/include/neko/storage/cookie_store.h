#pragma once

#include "neko/base/status.h"
#include "neko/url/url.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace neko::storage {

// A single cookie (RFC 6265).
struct Cookie
{
  std::string domain;     // e.g. "example.com" (leading dot stripped)
  bool host_only = false; // true when no Domain attribute was given
  std::string path;
  std::string name;
  std::string value;
  int64_t expiry = 0;  // unix seconds; 0 = session cookie (never persists)
  int64_t created = 0; // unix seconds when the cookie was stored
  bool secure = false;
  bool http_only = false;
  std::string same_site; // "", "None", "Lax" or "Strict"
};

// A persistent cookie jar.
//
// Responsibilities (RFC 6265 subset):
//   * parse Set-Cookie response headers against the request origin,
//   * store / replace / delete cookies,
//   * select cookies to send for a request URL (domain + path matching,
//     expiry and Secure filtering),
//   * serialize to and from a line-oriented text file.
//
// Known limitations (documented in docs/security/security-model.md):
//   * no Public Suffix List (a host may set a cookie for a registrable suffix
//     it does not own, e.g. "evil.example" setting domain "example"),
//   * SameSite is parsed and stored but not enforced: the current HTTP
//     client has no "site for cookies" / request-context concept.  Real
//     enforcement requires navigation metadata (initiator site).
//
// Threading: internally synchronized — every public method guards its
// mutation/read with |mutex_|, so the store can be used from the worker
// thread while the GUI thread reads snapshots (size/All).  Locked sections
// are short; persistence (Save) is synchronous.
class CookieStore
{
public:
  explicit CookieStore(std::string profile_dir);
  ~CookieStore() = default;

  CookieStore(const CookieStore&) = delete;
  CookieStore& operator=(const CookieStore&) = delete;

  // Loads the cookie file if present.  Missing file == empty store (no error).
  base::Result<void> Load();

  // Persists all non-session cookies atomically.
  base::Result<void> Save() const;

  // Parses one Set-Cookie header value against the request |origin| and
  // stores it (RFC 6265 sections 5.2 / 5.3).  Returns false when the header
  // is rejected (malformed or security rule violation); no error is
  // reported for a rejected header.  |now| is the current unix time and is
  // injectable for tests.
  bool SetCookieFromHeader(const url::Url& origin, std::string_view header, int64_t now);

  // RFC 6265 section 5.4: cookies to include in a request to |url| at |now|.
  std::vector<const Cookie*> CookiesFor(const url::Url& url, int64_t now) const;

  // "name=value; name2=value2" request header value for |url|, or empty.
  std::string CookieHeaderFor(const url::Url& url, int64_t now) const;

  // Removes expired cookies.
  void PurgeExpired(int64_t now);

  // Deletes one cookie (exact name/domain/path match).  Returns true if any
  // cookie was removed.
  bool DeleteCookie(const std::string& name, const std::string& domain, const std::string& path);

  void Clear();

  size_t size() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return cookies_.size();
  }
  bool empty() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return cookies_.empty();
  }

  // All cookies, copied under the lock (safe from any thread).
  std::vector<Cookie> All() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return cookies_;
  }

  const std::string& profile_dir() const
  {
    return profile_dir_;
  }

private:
  // Bodies of DeleteCookie/CookiesFor with |mutex_| already held (public
  // methods lock and delegate; SetCookieFromHeader needs the DeleteCookie
  // logic without re-entering the non-recursive lock).
  bool
  DeleteCookieLocked(const std::string& name, const std::string& domain, const std::string& path);
  std::vector<const Cookie*> CookiesForLocked(const url::Url& url, int64_t now) const;

  mutable std::mutex mutex_;
  std::string profile_dir_;
  std::string file_path_;
  std::vector<Cookie> cookies_;
};

} // namespace neko::storage
