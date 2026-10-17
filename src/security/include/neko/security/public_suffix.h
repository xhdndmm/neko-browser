#pragma once

// The Public Suffix List (https://publicsuffix.org/) matcher: which domains
// are "public suffixes" (com, co.uk, blogspot.com) and therefore may never
// own cookies or share a document.domain.
//
//   * cookie scoping: RFC 6265 §5.3 step 5 rejects Domain= cookies whose
//     domain is a public suffix;
//   * document.domain (HTML §7.7.1): the target must share the registrable
//     domain with the origin.
//
// The rule data is generated from the PSL by tools/gen_psl.py (do not edit
// public_suffix_data.inc by hand); see that script for regeneration.

#include <string>
#include <string_view>

namespace neko::security {

// The prevailing public suffix of |host| per the PSL matching algorithm
// (normal, wildcard `*.` and exception `!` rules; a host with no matching
// rule has its rightmost label as the suffix).  Returns "" for empty input.
std::string PublicSuffix(std::string_view host);

// The registrable domain (eTLD+1): the public suffix plus one more label.
// Empty when |host| is itself a public suffix (or a bare label).  An IP
// literal is its own registrable domain.
std::string RegistrableDomain(std::string_view host);

// True when |host| is exactly a public suffix (e.g. "com", "co.uk",
// "blogspot.com").  IP literals are never public suffixes.
bool IsPublicSuffix(std::string_view host);

} // namespace neko::security
