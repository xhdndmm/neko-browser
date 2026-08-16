#include "neko/storage/cookie_store.h"

#include "neko/base/logging.h"
#include "neko/storage/field_codec.h"
#include "neko/storage/file_util.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <string>
#include <vector>

namespace neko::storage {
namespace {

// ---------------------------------------------------------------------------
// Date parsing (RFC 6265 section 5.1.1): accepts the three allowed formats
//   Sun, 06 Nov 1994 08:49:37 GMT   (RFC 1123)
//   Sunday, 06-Nov-94 08:49:37 GMT  (RFC 850)
//   Sun Nov  6 08:49:37 1994        (asctime)
// Returns unix seconds, or -1 when the date is malformed.
// ---------------------------------------------------------------------------

constexpr const char* kMonths[] = {
    "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};

int MonthIndex(std::string_view s)
{
  for (int i = 0; i < 12; ++i) {
    if (s == kMonths[i])
      return i + 1;
  }
  return 0;
}

bool AllDigits(std::string_view s)
{
  if (s.empty())
    return false;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c)))
      return false;
  }
  return true;
}

// Parses a decimal integer, guarding against signed overflow (callers feed
// untrusted token text such as a Set-Cookie Expires date).
int ParseInt(std::string_view s)
{
  int v = 0;
  for (char c : s) {
    if (v > (INT_MAX - (c - '0')) / 10) {
      return INT_MAX; // saturate; callers validate the value afterwards
    }
    v = v * 10 + (c - '0');
  }
  return v;
}

// Howard Hinnant's days-from-civil algorithm (proleptic Gregorian).
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
{
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const int doy =
      (153 * (static_cast<int>(m) + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<int>(d) - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + static_cast<unsigned>(doy);
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

int64_t CivilToUnix(int y, int mo, int d, int hh, int mm, int ss)
{
  if (y < 1970)
    return -1;
  const int64_t days = DaysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
  return days * 86400 + hh * 3600 + mm * 60 + ss;
}

int64_t ParseCookieDate(std::string_view input)
{
  std::vector<std::string> tokens;
  std::string cur;
  const auto flush = [&] {
    if (!cur.empty()) {
      tokens.push_back(cur);
      cur.clear();
    }
  };
  for (const char raw : input) {
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
    if (c == ' ' || c == ',' || c == '-') {
      flush();
    } else {
      cur.push_back(c);
    }
  }
  flush();

  int hh = 0, mm = 0, ss = 0;
  bool have_time = false;
  std::vector<int> numerics;
  int month = 0;
  for (const auto& t : tokens) {
    if (t.size() >= 8 && t[2] == ':' && t[5] == ':') {
      const std::string_view h = std::string_view(t).substr(0, 2);
      const std::string_view m = std::string_view(t).substr(3, 2);
      const std::string_view s = std::string_view(t).substr(6, 2);
      if (AllDigits(h) && AllDigits(m) && AllDigits(s)) {
        hh = ParseInt(h);
        mm = ParseInt(m);
        ss = ParseInt(s);
        have_time = true;
      }
      continue;
    }
    if (const int mi = MonthIndex(t); mi != 0) {
      month = mi;
      continue;
    }
    if (AllDigits(t)) {
      numerics.push_back(ParseInt(t));
    }
  }

  // Every supported format carries exactly two numbers: day and year.
  if (!have_time || month == 0 || numerics.size() != 2)
    return -1;
  const int day = numerics[0];
  int year = numerics[1];
  if (day < 1 || day > 31)
    return -1;
  if (year < 100)
    year += (year >= 50) ? 1900 : 2000;
  // RFC 6265 5.1.5.1: years must be within 1601..9999; anything else is a
  // parse failure (also bounds the value after saturation).
  if (year < 1601 || year > 9999)
    return -1;
  return CivilToUnix(year, month, day, hh, mm, ss);
}

// ---------------------------------------------------------------------------
// Matching (RFC 6265 section 5.1.3 / 5.1.4)
// ---------------------------------------------------------------------------

bool DomainMatch(std::string_view host, const Cookie& c)
{
  if (c.host_only)
    return host == c.domain;
  if (host == c.domain)
    return true;
  const size_t dlen = c.domain.size();
  if (host.size() <= dlen + 1)
    return false;
  const size_t start = host.size() - dlen - 1;
  return host[start] == '.' && host.compare(start + 1, dlen, c.domain) == 0;
}

// Suffix-only domain match used by Set-Cookie acceptance (RFC 6265 5.3).
bool HostMatchesDomain(std::string_view host, std::string_view domain)
{
  if (host == domain)
    return true;
  if (host.size() <= domain.size() + 1)
    return false;
  const size_t start = host.size() - domain.size() - 1;
  return host[start] == '.' && host.compare(start + 1, domain.size(), domain) == 0;
}

bool PathMatch(std::string_view request_path, const Cookie& c)
{
  if (request_path == c.path)
    return true;
  if (request_path.size() <= c.path.size())
    return false;
  if (!request_path.starts_with(c.path))
    return false;
  return c.path.back() == '/' || request_path[c.path.size()] == '/';
}

// Default cookie path (RFC 6265 section 5.1.4).
std::string DefaultPath(const url::Url& url)
{
  const std::string& path = url.path();
  if (path.empty() || path.front() != '/')
    return "/";
  const size_t slash = path.find_last_of('/');
  if (slash == 0)
    return "/";
  return path.substr(0, slash);
}

// ---------------------------------------------------------------------------
// Set-Cookie parsing helpers (RFC 6265 section 5.2)
// ---------------------------------------------------------------------------

std::vector<std::string_view> SplitOnSemicolon(std::string_view s)
{
  std::vector<std::string_view> parts;
  size_t start = 0;
  while (true) {
    const size_t semi = s.find(';', start);
    if (semi == std::string_view::npos) {
      parts.push_back(s.substr(start));
      break;
    }
    parts.push_back(s.substr(start, semi - start));
    start = semi + 1;
  }
  return parts;
}

std::string_view Trim(std::string_view s)
{
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
    s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    s.remove_suffix(1);
  return s;
}

std::string ToLowerAscii(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

// Cookie name separators (RFC 6265 section 4.1.1).
bool NameHasSeparator(std::string_view name)
{
  constexpr std::string_view kSeps = "()<>@,;:\\\"/[]?={} \t";
  for (char c : name) {
    if (kSeps.find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 0x20 ||
        static_cast<unsigned char>(c) > 0x7E) {
      return true;
    }
  }
  return false;
}

// Splits "name=value" at the first '='.  Trailing/leading whitespace is
// trimmed from both sides.
void SplitNameValue(std::string_view pair, std::string_view& name, std::string_view& value)
{
  const size_t eq = pair.find('=');
  if (eq == std::string_view::npos) {
    name = Trim(pair);
    value = "";
  } else {
    name = Trim(pair.substr(0, eq));
    value = Trim(pair.substr(eq + 1));
  }
}

// Split one attribute "Name[=Value]".  Outputs std::string because the name
// is lowercased into a new buffer (a string_view would dangle).
void SplitAttribute(std::string_view attr, std::string& name, std::string& value)
{
  const size_t eq = attr.find('=');
  if (eq == std::string_view::npos) {
    name = ToLowerAscii(Trim(attr));
    value.clear();
  } else {
    name = ToLowerAscii(Trim(attr.substr(0, eq)));
    value = std::string(Trim(attr.substr(eq + 1)));
  }
}

bool ParseInt64(std::string_view s, int64_t* out)
{
  if (s.empty())
    return false;
  int64_t v = 0;
  const bool neg = s.front() == '-';
  const size_t start = neg ? 1 : 0;
  if (start >= s.size())
    return false;
  for (size_t i = start; i < s.size(); ++i) {
    const char c = s[i];
    if (!std::isdigit(static_cast<unsigned char>(c)))
      return false;
    v = v * 10 + (c - '0');
    if (v < 0)
      return false; // overflow
  }
  *out = neg ? -v : v;
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// CookieStore
// ---------------------------------------------------------------------------

CookieStore::CookieStore(std::string profile_dir)
    : profile_dir_(std::move(profile_dir)), file_path_(profile_dir_ + "/cookies.txt")
{}

base::Result<void> CookieStore::Load()
{
  std::lock_guard<std::mutex> lock(mutex_);
  cookies_.clear();
  auto maybe_data = ReadFile(file_path_);
  if (!maybe_data) {
    if (maybe_data.error().category() == base::ErrorCategory::kIo) {
      // Missing cookie file is a fresh profile, not an error.
      return base::Error();
    }
    return maybe_data.error();
  }
  const std::string& data = maybe_data.value();
  size_t pos = 0;
  int line_no = 0;
  while (pos < data.size()) {
    const size_t nl = data.find('\n', pos);
    const size_t end = (nl == std::string::npos) ? data.size() : nl;
    std::string_view line(data.data() + pos, end - pos);
    pos = (nl == std::string::npos) ? data.size() : nl + 1;
    ++line_no;
    if (line.empty() || line.front() == '#')
      continue;

    std::vector<std::string_view> fields;
    size_t fstart = 0;
    while (true) {
      const size_t tab = line.find('\t', fstart);
      if (tab == std::string_view::npos) {
        fields.push_back(line.substr(fstart));
        break;
      }
      fields.push_back(line.substr(fstart, tab - fstart));
      fstart = tab + 1;
    }
    if (fields.size() != 10) {
      NEKO_LOG_WARNING_F("cookie store: skipping malformed line {}", line_no);
      continue;
    }
    Cookie c;
    auto r_domain = DecodeField(fields[0]);
    auto r_path = DecodeField(fields[2]);
    auto r_name = DecodeField(fields[3]);
    auto r_value = DecodeField(fields[4]);
    auto r_same = DecodeField(fields[9]);
    if (!r_domain || !r_path || !r_name || !r_value || !r_same) {
      NEKO_LOG_WARNING_F("cookie store: skipping undecodable line {}", line_no);
      continue;
    }
    c.domain = std::move(r_domain.value());
    c.host_only = fields[1] == "1";
    c.path = std::move(r_path.value());
    c.name = std::move(r_name.value());
    c.value = std::move(r_value.value());
    if (!ParseInt64(fields[5], &c.expiry) || !ParseInt64(fields[6], &c.created)) {
      NEKO_LOG_WARNING_F("cookie store: skipping line with bad numbers {}", line_no);
      continue;
    }
    c.secure = fields[7] == "1";
    c.http_only = fields[8] == "1";
    c.same_site = std::move(r_same.value());
    cookies_.push_back(std::move(c));
  }
  return base::Error();
}

base::Result<void> CookieStore::Save() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::string out = "# neko-cookie v1\n";
  for (const auto& c : cookies_) {
    if (c.expiry == 0)
      continue; // session cookies never persist
    out += EncodeField(c.domain);
    out += '\t';
    out += c.host_only ? '1' : '0';
    out += '\t';
    out += EncodeField(c.path);
    out += '\t';
    out += EncodeField(c.name);
    out += '\t';
    out += EncodeField(c.value);
    out += '\t';
    out += std::to_string(c.expiry);
    out += '\t';
    out += std::to_string(c.created);
    out += '\t';
    out += c.secure ? '1' : '0';
    out += '\t';
    out += c.http_only ? '1' : '0';
    out += '\t';
    out += EncodeField(c.same_site);
    out += '\n';
  }
  return WriteFileAtomic(file_path_, out);
}

bool CookieStore::SetCookieFromHeader(const url::Url& origin, std::string_view header, int64_t now)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const std::vector<std::string_view> parts = SplitOnSemicolon(header);
  if (parts.empty())
    return false;

  // Cookie-pair (section 5.2 step 1).
  std::string_view raw_name, raw_value;
  SplitNameValue(parts[0], raw_name, raw_value);
  if (raw_name.empty() || NameHasSeparator(raw_name))
    return false;
  const std::string name(raw_name);
  const std::string value(raw_value);

  // Attributes.
  std::string domain;
  bool has_domain = false;
  std::string path;
  bool has_path = false;
  int64_t max_age = -1;
  bool has_max_age = false;
  bool secure = false;
  bool http_only = false;
  std::string same_site;
  int64_t expires = -1;
  bool has_expires = false;

  for (size_t i = 1; i < parts.size(); ++i) {
    std::string attr_name, attr_value;
    SplitAttribute(parts[i], attr_name, attr_value);
    if (attr_name == "expires") {
      const int64_t t = ParseCookieDate(attr_value);
      if (t >= 0) {
        expires = t;
        has_expires = true;
      }
    } else if (attr_name == "max-age") {
      int64_t v = 0;
      if (ParseInt64(attr_value, &v)) {
        max_age = v;
        has_max_age = true;
      }
    } else if (attr_name == "domain") {
      if (!attr_value.empty()) {
        // Ignore a leading dot; strip trailing dots (RFC 6265 5.2.3).
        std::string d(attr_value);
        if (!d.empty() && d.front() == '.')
          d.erase(d.begin());
        while (!d.empty() && d.back() == '.')
          d.pop_back();
        domain = ToLowerAscii(d);
        has_domain = !domain.empty();
      }
    } else if (attr_name == "path") {
      if (!attr_value.empty() && attr_value.front() == '/') {
        path = std::string(attr_value);
        has_path = true;
      }
    } else if (attr_name == "secure") {
      secure = true;
    } else if (attr_name == "httponly") {
      http_only = true;
    } else if (attr_name == "samesite") {
      const std::string v = ToLowerAscii(attr_value);
      if (v == "none" || v == "lax" || v == "strict") {
        same_site = v;
      }
    }
  }

  // Section 5.3 step 5: reject when the cookie domain is not a domain-match
  // of the origin host.
  const std::string& origin_host = origin.host();
  if (has_domain && !HostMatchesDomain(origin_host, domain)) {
    return false;
  }

  const std::string cookie_domain = has_domain ? domain : origin_host;
  const bool host_only = !has_domain;
  const std::string cookie_path = has_path ? path : DefaultPath(origin);

  // Expiry (section 5.2.2 / 5.3).
  int64_t expiry = 0; // session by default
  if (has_max_age) {
    if (max_age <= 0) {
      // Delete the cookie: Max-Age=0 (or negative) means removal.
      DeleteCookieLocked(name, cookie_domain, cookie_path);
      return true;
    }
    expiry = now + max_age;
  } else if (has_expires) {
    expiry = expires;
  }
  if (expiry > 0 && expiry <= now) {
    // Already expired: remove any existing cookie and do not store.
    DeleteCookieLocked(name, cookie_domain, cookie_path);
    return true;
  }

  // Replace any existing cookie with the same name/domain/path.
  int64_t created = now;
  for (auto& c : cookies_) {
    if (c.name == name && c.domain == cookie_domain && c.path == cookie_path) {
      created = c.created; // preserve creation time across replacement
      c.domain = cookie_domain;
      c.host_only = host_only;
      c.path = cookie_path;
      c.value = value;
      c.expiry = expiry;
      c.secure = secure;
      c.http_only = http_only;
      c.same_site = same_site;
      return true;
    }
  }

  cookies_.push_back(Cookie{cookie_domain,
                            host_only,
                            cookie_path,
                            name,
                            value,
                            expiry,
                            created,
                            secure,
                            http_only,
                            same_site});
  return true;
}

std::vector<const Cookie*> CookieStore::CookiesFor(const url::Url& url, int64_t now) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return CookiesForLocked(url, now);
}

std::vector<const Cookie*> CookieStore::CookiesForLocked(const url::Url& url, int64_t now) const
{
  const std::string& host = url.host();
  const std::string& path = url.path();
  const bool is_secure = url.scheme() == "https";
  std::vector<const Cookie*> out;
  for (const auto& c : cookies_) {
    if (c.expiry > 0 && now > c.expiry)
      continue;
    if (c.secure && !is_secure)
      continue;
    if (!DomainMatch(host, c))
      continue;
    if (!PathMatch(path, c))
      continue;
    out.push_back(&c);
  }
  // RFC 6265 section 5.4 step 2: longer paths first, then earlier creation.
  std::stable_sort(out.begin(), out.end(), [](const Cookie* a, const Cookie* b) {
    if (a->path.size() != b->path.size())
      return a->path.size() > b->path.size();
    return a->created < b->created;
  });
  return out;
}

std::string CookieStore::CookieHeaderFor(const url::Url& url, int64_t now) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto cookies = CookiesForLocked(url, now);
  std::string header;
  for (const Cookie* c : cookies) {
    if (!header.empty())
      header += "; ";
    header += c->name;
    header += '=';
    header += c->value;
  }
  return header;
}

void CookieStore::PurgeExpired(int64_t now)
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::erase_if(cookies_, [now](const Cookie& c) { return c.expiry > 0 && now > c.expiry; });
}

bool CookieStore::DeleteCookie(const std::string& name,
                               const std::string& domain,
                               const std::string& path)
{
  std::lock_guard<std::mutex> lock(mutex_);
  return DeleteCookieLocked(name, domain, path);
}

bool CookieStore::DeleteCookieLocked(const std::string& name,
                                     const std::string& domain,
                                     const std::string& path)
{
  const size_t before = cookies_.size();
  std::erase_if(cookies_, [&](const Cookie& c) {
    return c.name == name && c.domain == domain && c.path == path;
  });
  return cookies_.size() != before;
}

void CookieStore::Clear()
{
  std::lock_guard<std::mutex> lock(mutex_);
  cookies_.clear();
}

} // namespace neko::storage
