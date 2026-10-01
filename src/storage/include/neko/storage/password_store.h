#pragma once

#include "neko/base/status.h"

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace neko::storage {

// One saved login: an origin-scoped username/password pair.
struct Credential
{
  std::string origin; // serialized security origin (scheme://host[:port])
  std::string username;
  std::string password;
};

// Persistent, encrypted password storage.
//
// Records are keyed by (origin, username); saving the same key again replaces
// the password.  The store lives in <profile>/logins.dat as a small binary
// container: an 8-byte magic, a random 12-byte AES-GCM nonce, the 16-byte GCM
// tag and the encrypted payload (the plaintext is the usual line-oriented,
// percent-encoded record format).  The payload never touches disk in the
// clear.  The 256-bit key is a random machine-local key in
// <profile>/login_key.bin, created 0600 on POSIX.
//
// Threat model (mirrored in docs/security/security-model.md): encryption here
// protects the saved logins from being lifted by copying logins.dat alone
// (backups, synced profile folders, another local account that cannot read
// the key file).  It does NOT protect against local malware running as the
// user -- the key sits next to the data -- and this milestone deliberately
// ships no master password.  Record the limitation honestly; a future
// milestone can move the key into an OS keyring.
//
// Corruption policy: a missing file loads as empty; if the file exists but
// cannot be decrypted (wrong/rotated key, tampering) Load() reports an error
// and the instance refuses to Save(), so a failed load can never overwrite
// recoverable data.
//
// Threading: internally synchronized -- every public method guards its
// mutation/read with |mutex_|.
class PasswordStore
{
public:
  explicit PasswordStore(std::string profile_dir);
  ~PasswordStore() = default;

  PasswordStore(const PasswordStore&) = delete;
  PasswordStore& operator=(const PasswordStore&) = delete;

  base::Result<void> Load();
  // Encrypts and atomically writes the current records.  Fails when a previous
  // Load() could not decrypt the existing file (refusing to destroy data).
  base::Result<void> Save() const;

  // Inserts or replaces the credential for (|origin|, |username|).
  void Add(std::string origin, std::string username, std::string password);

  // All records, sorted by (origin, username).  Copies under the lock.
  std::vector<Credential> All() const;

  // Records for one origin (exact match on the serialized origin).
  std::vector<Credential> ForOrigin(std::string_view origin) const;

  // Removes one record.  Returns true when a record was removed.
  bool Remove(std::string_view origin, std::string_view username);

  void Clear();

  size_t size() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
  }

  const std::string& profile_dir() const
  {
    return profile_dir_;
  }

private:
  // Plaintext serialization of |entries_| (line-oriented, percent-encoded).
  std::string SerializeLocked() const;
  // Parses a decrypted payload into entries_; malformed records are skipped.
  void ParseLocked(std::string_view payload);

  // Reads (or creates, when |create|) the key file.  Caller holds mutex_.
  base::Result<std::string> LoadKeyLocked(bool create) const;

  // |save_blocked_|: a Load() found an existing file it could not decrypt;
  // saving would destroy data the user may still recover with the old key.
  bool save_blocked_ = false;

  mutable std::mutex mutex_;
  std::string profile_dir_;
  std::string data_path_;
  std::string key_path_;
  std::vector<Credential> entries_;
};

} // namespace neko::storage
