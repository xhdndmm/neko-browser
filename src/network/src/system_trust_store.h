#pragma once

// Internal discovery of the certificate trust store of the *host* machine.
//
// A packaged build bundles libcrypto from the release runner, and OpenSSL
// resolves its default trust store (SSL_CTX_set_default_verify_paths) from the
// paths compiled into that library -- Debian/Ubuntu's /usr/lib/ssl, or the
// vcpkg prefix on Windows.  On a host with a different layout (Arch's
// /etc/ssl/certs, Fedora/RHEL's /etc/pki/..., macOS's /etc/ssl/cert.pem) that
// lookup finds zero anchors *without reporting an error*, so every https://
// handshake fails with "certificate verify failed" (2026-09 rc2 Linux
// release: worked on the Ubuntu runner, broke on Arch and every other
// non-Debian host).
//
// This module locates the trust store of the machine the process actually runs
// on.  Discovery mirrors OpenSSL's own rules -- SSL_CERT_FILE / SSL_CERT_DIR
// replace the defaults when set -- and falls back to the well-known locations
// of the platform, so verification works no matter where libcrypto was built.
//
// Parsing a distribution CA bundle (121 certificates) costs ~2.6 ms, which
// would be paid per connection by every freshly created SSL_CTX, so the parsed
// anchors are memoized per resolved candidate list (thread-safe; a cache entry
// is never invalidated, i.e. trust store updates are picked up on restart --
// the same trade-off browsers make for their root store).
//
// This header is internal to the module; it is not installed and must not be
// included from public headers.

#include "neko/base/status.h"

#include <cstddef>
#include <string>
#include <vector>

// Only forward-declared: OpenSSL types stay out of this header, and callers
// that do not own a store never need to include OpenSSL headers.
typedef struct x509_store_st X509_STORE;

namespace neko::network {

// The kind of location a candidate trust store is.
enum class TrustStoreKind
{
  kPemFile,         // one PEM bundle containing one or more CA certificates
  kHashedDirectory, // an OpenSSL hashed directory (c_rehash layout)
  kPlatformStore,   // a native store addressed by an OpenSSL store URI
};

struct TrustStoreCandidate
{
  TrustStoreKind kind = TrustStoreKind::kPemFile;
  // A filesystem path for the first two kinds, the loader URI (e.g.
  // "org.openssl.winstore://") for kPlatformStore.
  std::string location;

  friend bool operator==(const TrustStoreCandidate&, const TrustStoreCandidate&) = default;
};

// The candidate locations for this machine, highest priority first: the
// well-known locations of the platform, unless SSL_CERT_FILE / SSL_CERT_DIR
// replace them (OpenSSL's rule for those variables).  Pure (it only reads the
// environment), so tests can pin the list; existence is checked by
// ExistingTrustStoreCandidates.
std::vector<TrustStoreCandidate> SystemTrustStoreCandidates();

// |candidates| reduced to the ones usable on this machine: regular files,
// directories that look like an OpenSSL hashed directory, and platform stores
// (which are taken on faith -- the platform always has one).
std::vector<TrustStoreCandidate>
ExistingTrustStoreCandidates(const std::vector<TrustStoreCandidate>& candidates);

// What a load attempt put into the store.
struct TrustStoreLoad
{
  std::size_t certificates = 0; // anchors parsed into the store
  std::size_t directories = 0;  // hashed dirs registered as lookup sources
  std::size_t stores = 0;       // native stores registered as lookup sources

  bool empty() const
  {
    return certificates == 0 && directories == 0 && stores == 0;
  }
};

// Loads |candidates| into |store|.  Fails when a candidate exists but cannot be
// loaded; an empty candidate list is not an error here (see
// LoadSystemTrustStore).
base::Result<TrustStoreLoad>
LoadTrustStoreCandidates(X509_STORE* store, const std::vector<TrustStoreCandidate>& candidates);

// Discovers, filters and loads the host trust store.  Fails when the host has
// no usable trust store, so certificate verification can never silently run
// with zero anchors.
base::Result<TrustStoreLoad> LoadSystemTrustStore(X509_STORE* store);

// The candidates that were considered, as "path" / "path (hashed directory)"
// / "uri", joined with ", ": used in errors so the report names what was
// actually looked for.
std::string DescribeTrustStoreCandidates(const std::vector<TrustStoreCandidate>& candidates);

} // namespace neko::network
