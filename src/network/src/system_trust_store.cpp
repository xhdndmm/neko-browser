#include "system_trust_store.h"

// OpenSSL's error queue text (SslErrorString) is shared with tls_socket.cpp.
#include "openssl_error.h"

#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neko::network {
namespace {

using StorePtr = std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)>;
using AnchorPtr = std::shared_ptr<X509>;
using AnchorPtrs = std::vector<AnchorPtr>;

std::string EnvironmentValue(const char* name)
{
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

// The well-known trust store locations of this platform, highest priority
// first.  The MIME/libcryptostore layouts below are the ones actually shipped
// by the distributions and operating systems we support (see
// docs/networking/README.md).
std::vector<TrustStoreCandidate> PlatformCandidates()
{
#if defined(_WIN32)
  // Windows has no conventional PEM bundle: the OS root store is the source of
  // truth and is reachable through OpenSSL's "winstore" loader.
  return {{TrustStoreKind::kPlatformStore, "org.openssl.winstore://"}};
#elif defined(__APPLE__)
  // macOS ships its root bundle here; it is updated with the OS.
  return {{TrustStoreKind::kPemFile, "/etc/ssl/cert.pem"}};
#else
  return {
      // Debian, Ubuntu, Arch, Gentoo, Alpine, Void.
      {TrustStoreKind::kPemFile, "/etc/ssl/certs/ca-certificates.crt"},
      // Fedora, RHEL, CentOS, Amazon Linux.
      {TrustStoreKind::kPemFile, "/etc/pki/tls/certs/ca-bundle.crt"},
      // Fedora/RHEL ca-trust extracted bundle.
      {TrustStoreKind::kPemFile, "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"},
      // Alpine, FreeBSD, older macOS.
      {TrustStoreKind::kPemFile, "/etc/ssl/cert.pem"},
      // openSUSE.
      {TrustStoreKind::kPemFile, "/etc/ssl/ca-bundle.pem"},
      // NetBSD.
      {TrustStoreKind::kPemFile, "/etc/ssl/cacert.pem"},
      // FreeBSD ports.
      {TrustStoreKind::kPemFile, "/usr/local/share/certs/ca-root-nss.crt"},
      // Hashed directories, for hosts that only ship the c_rehash layout.
      {TrustStoreKind::kHashedDirectory, "/etc/ssl/certs"},
      {TrustStoreKind::kHashedDirectory, "/etc/pki/tls/certs"},
  };
#endif
}

bool IsHexDigit(char c)
{
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool IsDigit(char c)
{
  return c >= '0' && c <= '9';
}

// True for the names c_rehash / update-ca-certificates create for a
// certificate: eight hex digits of the subject hash, a dot, then the index.
bool IsHashedCertificateName(std::string_view name)
{
  if (name.size() < 10 || name[8] != '.') {
    return false;
  }
  for (std::size_t i = 0; i < 8; ++i) {
    if (!IsHexDigit(name[i])) {
      return false;
    }
  }
  for (std::size_t i = 9; i < name.size(); ++i) {
    if (!IsDigit(name[i])) {
      return false;
    }
  }
  return true;
}

// A directory only counts as a trust store when it actually holds hashed
// certificates: OpenSSL registers the directory as a *lazy* lookup source
// (X509_STORE_load_path reports success for an empty one), so this is the only
// way to tell a usable store from an empty leftover directory.
//
// Only called for the POSIX directory candidates, so the byte-oriented
// filename conversion cannot throw (the Windows list has no directories).
bool HoldsHashedCertificates(const std::string& directory)
{
  std::error_code error;
  std::filesystem::directory_iterator it{std::filesystem::path(directory), error};
  const std::filesystem::directory_iterator end;
  while (!error && it != end) {
    if (IsHashedCertificateName(it->path().filename().string())) {
      return true;
    }
    it.increment(error);
  }
  return false;
}

bool CandidateExists(const TrustStoreCandidate& candidate)
{
  std::error_code error;
  switch (candidate.kind) {
  case TrustStoreKind::kPemFile:
    return std::filesystem::is_regular_file(candidate.location, error) && !error;
  case TrustStoreKind::kHashedDirectory:
    return std::filesystem::is_directory(candidate.location, error) && !error &&
           HoldsHashedCertificates(candidate.location);
  case TrustStoreKind::kPlatformStore:
    return true; // the platform always has a store of this kind
  }
  return false;
}

// Parsed anchors, keyed by the resolved candidate list.  Never invalidated: a
// trust store updated on disk is picked up on restart, which is the trade-off
// browsers make for their root store.
struct AnchorCache
{
  std::mutex mutex;
  std::unordered_map<std::string, AnchorPtrs> entries;
};

AnchorCache& Cache()
{
  // Deliberately never destroyed: a function-local static's destructor would
  // free the X509 objects at exit while another thread may still be verifying
  // certificates.
  static AnchorCache* cache = new AnchorCache();
  return *cache;
}

std::string CacheKey(const std::vector<TrustStoreCandidate>& candidates)
{
  std::string key;
  for (const TrustStoreCandidate& candidate : candidates) {
    key += std::to_string(static_cast<int>(candidate.kind));
    key += ':';
    key += candidate.location;
    key += '\n';
  }
  return key;
}

// Parses one PEM bundle into a throwaway store and harvests its certificates;
// reading the bundle costs ~2.6 ms, which is why the result is memoized.
base::Result<AnchorPtrs> ParsePemBundle(const std::string& path)
{
  StorePtr store(X509_STORE_new(), X509_STORE_free);
  if (!store) {
    return base::Err(base::Error::Network("TLS: cannot create a certificate store"));
  }
  if (X509_STORE_load_file(store.get(), path.c_str()) != 1) {
    return base::Err(
        base::Error::Network("TLS: cannot load trust store " + path + ": " + SslErrorString()));
  }
  AnchorPtrs anchors;
  STACK_OF(X509_OBJECT)* objects = X509_STORE_get0_objects(store.get());
  const int count = objects == nullptr ? 0 : sk_X509_OBJECT_num(objects);
  for (int i = 0; i < count; ++i) {
    X509* certificate = X509_OBJECT_get0_X509(sk_X509_OBJECT_value(objects, i));
    if (certificate == nullptr) {
      continue; // CRL entry
    }
    if (X509_up_ref(certificate) != 1) {
      return base::Err(base::Error::Network("TLS: cannot reference a trust anchor"));
    }
    anchors.emplace_back(certificate, X509_free);
  }
  if (anchors.empty()) {
    return base::Err(
        base::Error::Network("TLS: trust store " + path + " contains no certificates"));
  }
  return anchors;
}

base::Result<AnchorPtrs> ParsedAnchors(const std::vector<TrustStoreCandidate>& files)
{
  const std::string key = CacheKey(files);
  AnchorCache& cache = Cache();
  {
    std::lock_guard<std::mutex> lock(cache.mutex);
    const auto found = cache.entries.find(key);
    if (found != cache.entries.end()) {
      return found->second;
    }
  }

  AnchorPtrs parsed;
  for (const TrustStoreCandidate& file : files) {
    base::Result<AnchorPtrs> anchors = ParsePemBundle(file.location);
    if (!anchors) {
      return base::Err(anchors.error());
    }
    parsed.insert(parsed.end(),
                  std::make_move_iterator(anchors.value().begin()),
                  std::make_move_iterator(anchors.value().end()));
  }

  std::lock_guard<std::mutex> lock(cache.mutex);
  // Concurrent loads of the same list are harmless: they parse the same
  // certificates and the first one to arrive wins.
  const auto entry = cache.entries.try_emplace(key, std::move(parsed)).first;
  return entry->second;
}

} // namespace

std::vector<TrustStoreCandidate> SystemTrustStoreCandidates()
{
  const std::string cert_file = EnvironmentValue("SSL_CERT_FILE");
  const std::string cert_dir = EnvironmentValue("SSL_CERT_DIR");
  if (cert_file.empty() && cert_dir.empty()) {
    return PlatformCandidates();
  }

  // SSL_CERT_FILE / SSL_CERT_DIR *replace* the platform defaults when set
  // (OpenSSL's rule), so an explicit override is exactly what gets used.
  std::vector<TrustStoreCandidate> candidates;
  if (!cert_file.empty()) {
    candidates.push_back({TrustStoreKind::kPemFile, cert_file});
  }
  if (!cert_dir.empty()) {
    candidates.push_back({TrustStoreKind::kHashedDirectory, cert_dir});
  }
  return candidates;
}

std::vector<TrustStoreCandidate>
ExistingTrustStoreCandidates(const std::vector<TrustStoreCandidate>& candidates)
{
  std::vector<TrustStoreCandidate> existing;
  for (const TrustStoreCandidate& candidate : candidates) {
    if (CandidateExists(candidate)) {
      existing.push_back(candidate);
    }
  }
  return existing;
}

base::Result<TrustStoreLoad>
LoadTrustStoreCandidates(X509_STORE* store, const std::vector<TrustStoreCandidate>& candidates)
{
  if (store == nullptr) {
    return base::Err(base::Error::Network("TLS: cannot access the certificate store"));
  }

  TrustStoreLoad loaded;
  std::vector<TrustStoreCandidate> files;
  for (const TrustStoreCandidate& candidate : candidates) {
    switch (candidate.kind) {
    case TrustStoreKind::kPemFile:
      files.push_back(candidate); // parsed below, once per candidate list
      break;
    case TrustStoreKind::kHashedDirectory:
      if (X509_STORE_load_path(store, candidate.location.c_str()) != 1) {
        return base::Err(base::Error::Network("TLS: cannot load trust store directory " +
                                              candidate.location + ": " + SslErrorString()));
      }
      ++loaded.directories;
      break;
    case TrustStoreKind::kPlatformStore:
      if (X509_STORE_load_store(store, candidate.location.c_str()) != 1) {
        return base::Err(base::Error::Network("TLS: cannot load platform trust store " +
                                              candidate.location + ": " + SslErrorString()));
      }
      ++loaded.stores;
      break;
    }
  }
  if (files.empty()) {
    return loaded;
  }

  const base::Result<AnchorPtrs> anchors = ParsedAnchors(files);
  if (!anchors) {
    return base::Err(anchors.error());
  }
  for (const AnchorPtr& anchor : anchors.value()) {
    if (X509_STORE_add_cert(store, anchor.get()) != 1) {
      return base::Err(base::Error::Network("TLS: cannot add a trust anchor: " + SslErrorString()));
    }
  }
  loaded.certificates = anchors.value().size();
  return loaded;
}

base::Result<TrustStoreLoad> LoadSystemTrustStore(X509_STORE* store)
{
  const std::vector<TrustStoreCandidate> candidates = SystemTrustStoreCandidates();
  base::Result<TrustStoreLoad> loaded =
      LoadTrustStoreCandidates(store, ExistingTrustStoreCandidates(candidates));
  if (!loaded) {
    return loaded;
  }
  if (loaded.value().empty()) {
    // Failing closed with an actionable message beats verifying against zero
    // anchors, which is what SSL_CTX_set_default_verify_paths does when the
    // paths compiled into libcrypto do not exist on this host.
    return base::Err(base::Error::Network(
        "TLS: no system trust store found (looked for " + DescribeTrustStoreCandidates(candidates) +
        "); install the CA certificates package of your distribution, or set SSL_CERT_FILE to a "
        "PEM bundle"));
  }
  return loaded;
}

std::string DescribeTrustStoreCandidates(const std::vector<TrustStoreCandidate>& candidates)
{
  std::string description;
  for (const TrustStoreCandidate& candidate : candidates) {
    if (!description.empty()) {
      description += ", ";
    }
    description += candidate.location;
    if (candidate.kind == TrustStoreKind::kHashedDirectory) {
      description += " (hashed directory)";
    }
  }
  return description;
}

} // namespace neko::network
