#include "neko/storage/password_store.h"

#include "neko/storage/field_codec.h"
#include "neko/storage/file_util.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <string>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace neko::storage {
namespace {

// Container layout: magic | 12-byte nonce | 16-byte tag | ciphertext.
constexpr std::string_view kMagic = "NEKOPW1\n";
constexpr std::size_t kNonceBytes = 12;
constexpr std::size_t kTagBytes = 16;
constexpr std::size_t kKeyBytes = 32;

// AES-256-GCM encrypts |plaintext| into a self-contained container (magic +
// nonce + tag + ciphertext).  Returns an empty string on failure (RNG or
// cipher error).
std::string EncryptContainer(std::string_view plaintext, std::string_view key)
{
  std::array<unsigned char, kNonceBytes> nonce{};
  if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
    return {};
  }

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (ctx == nullptr) {
    return {};
  }
  std::string output(
      plaintext.size() + static_cast<std::size_t>(EVP_CIPHER_block_size(EVP_aes_256_gcm())), '\0');
  std::array<unsigned char, kTagBytes> tag{};
  int len = 0;
  int total = 0;
  bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(
                ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) == 1 &&
            EVP_EncryptInit_ex(ctx,
                               nullptr,
                               nullptr,
                               reinterpret_cast<const unsigned char*>(key.data()),
                               nonce.data()) == 1 &&
            EVP_EncryptUpdate(ctx,
                              reinterpret_cast<unsigned char*>(output.data()),
                              &len,
                              reinterpret_cast<const unsigned char*>(plaintext.data()),
                              static_cast<int>(plaintext.size())) == 1;
  if (ok) {
    total = len;
    ok = EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(output.data()) + total, &len) ==
         1;
    if (ok) {
      total += len;
    }
  }
  if (ok) {
    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()), tag.data()) ==
         1;
  }
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) {
    return {};
  }

  output.resize(static_cast<std::size_t>(total));
  std::string container(kMagic);
  container.append(reinterpret_cast<const char*>(nonce.data()), nonce.size());
  container.append(reinterpret_cast<const char*>(tag.data()), tag.size());
  container.append(output);
  return container;
}

// Decrypts a container produced by EncryptContainer().  GCM verifies the tag,
// so a wrong key or tampered bytes fail here instead of yielding garbage.
base::Result<std::string> DecryptContainer(std::string_view container, std::string_view key)
{
  if (container.size() < kMagic.size() + kNonceBytes + kTagBytes) {
    return base::Error::Parse("password store container is truncated");
  }
  if (container.substr(0, kMagic.size()) != kMagic) {
    return base::Error::Parse("password store container has a bad magic");
  }
  const auto* nonce = reinterpret_cast<const unsigned char*>(
      container.data() + static_cast<std::ptrdiff_t>(kMagic.size()));
  const auto* tag = reinterpret_cast<const unsigned char*>(
      container.data() + static_cast<std::ptrdiff_t>(kMagic.size() + kNonceBytes));
  const std::string_view ciphertext = container.substr(kMagic.size() + kNonceBytes + kTagBytes);

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (ctx == nullptr) {
    return base::Error::Unknown("failed to allocate a cipher context");
  }
  std::string output(
      ciphertext.size() + static_cast<std::size_t>(EVP_CIPHER_block_size(EVP_aes_256_gcm())), '\0');
  int len = 0;
  int total = 0;
  bool ok =
      EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceBytes), nullptr) ==
          1 &&
      EVP_DecryptInit_ex(
          ctx, nullptr, nullptr, reinterpret_cast<const unsigned char*>(key.data()), nonce) == 1 &&
      EVP_DecryptUpdate(ctx,
                        reinterpret_cast<unsigned char*>(output.data()),
                        &len,
                        reinterpret_cast<const unsigned char*>(ciphertext.data()),
                        static_cast<int>(ciphertext.size())) == 1;
  if (ok) {
    total = len;
    // Set the expected tag just before finalizing: DecryptFinal_ex verifies it.
    ok = EVP_CIPHER_CTX_ctrl(ctx,
                             EVP_CTRL_GCM_SET_TAG,
                             static_cast<int>(kTagBytes),
                             const_cast<unsigned char*>(tag)) == 1 &&
         EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(output.data()) + total, &len) ==
             1;
    if (ok) {
      total += len;
    }
  }
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) {
    return base::Error::Parse("password store could not be decrypted (wrong key or corruption)");
  }
  output.resize(static_cast<std::size_t>(total));
  return output;
}

} // namespace

PasswordStore::PasswordStore(std::string profile_dir)
    : profile_dir_(std::move(profile_dir)), data_path_(profile_dir_ + "/logins.dat"),
      key_path_(profile_dir_ + "/login_key.bin")
{}

base::Result<void> PasswordStore::Load()
{
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
  save_blocked_ = false;

  auto bytes = ReadFile(data_path_);
  if (!bytes.has_value()) {
    if (!std::filesystem::exists(data_path_)) {
      // No store yet: an empty one is the correct starting state.
      return base::Ok();
    }
    // The file exists but cannot be read (permissions, I/O): keep it safe.
    save_blocked_ = true;
    return base::Err(std::move(bytes).error());
  }
  if (bytes.value().empty()) {
    return base::Ok();
  }

  auto key = LoadKeyLocked(/*create=*/false);
  if (!key.has_value()) {
    save_blocked_ = true;
    return base::Err(std::move(key).error());
  }
  auto plaintext = DecryptContainer(bytes.value(), key.value());
  if (!plaintext.has_value()) {
    save_blocked_ = true;
    return base::Err(std::move(plaintext).error());
  }
  ParseLocked(plaintext.value());
  return base::Ok();
}

base::Result<void> PasswordStore::Save() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (save_blocked_) {
    return base::Error::Io(
        "password store refuses to overwrite data a previous Load() could not decrypt");
  }
  auto key = LoadKeyLocked(/*create=*/true);
  if (!key.has_value()) {
    return base::Err(std::move(key).error());
  }
  const std::string container = EncryptContainer(SerializeLocked(), key.value());
  if (container.empty()) {
    return base::Error::Unknown("failed to encrypt the password store");
  }
  return WriteFileAtomic(data_path_, container);
}

void PasswordStore::Add(std::string origin, std::string username, std::string password)
{
  std::lock_guard<std::mutex> lock(mutex_);
  for (Credential& entry : entries_) {
    if (entry.origin == origin && entry.username == username) {
      entry.password = std::move(password);
      return;
    }
  }
  entries_.push_back(Credential{std::move(origin), std::move(username), std::move(password)});
}

std::vector<Credential> PasswordStore::All() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Credential> copy = entries_;
  std::sort(copy.begin(), copy.end(), [](const Credential& lhs, const Credential& rhs) {
    if (lhs.origin != rhs.origin) {
      return lhs.origin < rhs.origin;
    }
    return lhs.username < rhs.username;
  });
  return copy;
}

std::vector<Credential> PasswordStore::ForOrigin(std::string_view origin) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Credential> matches;
  for (const Credential& entry : entries_) {
    if (entry.origin == origin) {
      matches.push_back(entry);
    }
  }
  return matches;
}

bool PasswordStore::Remove(std::string_view origin, std::string_view username)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Credential& entry) {
    return entry.origin == origin && entry.username == username;
  });
  if (it == entries_.end()) {
    return false;
  }
  entries_.erase(it);
  return true;
}

void PasswordStore::Clear()
{
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
}

std::string PasswordStore::SerializeLocked() const
{
  std::string out;
  for (const Credential& entry : entries_) {
    out += EncodeField(entry.origin);
    out += '\t';
    out += EncodeField(entry.username);
    out += '\t';
    out += EncodeField(entry.password);
    out += '\n';
  }
  return out;
}

void PasswordStore::ParseLocked(std::string_view payload)
{
  std::size_t line_start = 0;
  while (line_start < payload.size()) {
    std::size_t line_end = payload.find('\n', line_start);
    if (line_end == std::string_view::npos) {
      line_end = payload.size();
    }
    const std::string_view line = payload.substr(line_start, line_end - line_start);
    line_start = line_end + 1;
    if (line.empty()) {
      continue;
    }
    const std::vector<std::string_view> fields = SplitTabFields(line);
    if (fields.size() != 3) {
      continue; // Malformed record: skip, keep the rest loadable.
    }
    auto origin = DecodeField(fields[0]);
    auto username = DecodeField(fields[1]);
    auto password = DecodeField(fields[2]);
    if (!origin.has_value() || !username.has_value() || !password.has_value()) {
      continue;
    }
    entries_.push_back(Credential{
        std::move(origin).value(), std::move(username).value(), std::move(password).value()});
  }
}

base::Result<std::string> PasswordStore::LoadKeyLocked(bool create) const
{
  auto existing = ReadFile(key_path_);
  if (existing.has_value()) {
    if (existing.value().size() == kKeyBytes) {
      return existing.value();
    }
    if (!existing.value().empty()) {
      return base::Error::Parse("login key file has the wrong size");
    }
    // An empty file is treated like a missing one below.
  } else if (std::filesystem::exists(key_path_)) {
    return base::Err(std::move(existing).error());
  }
  if (!create) {
    return base::Error::Io("no login key file");
  }

  std::string key(kKeyBytes, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char*>(key.data()), static_cast<int>(kKeyBytes)) != 1) {
    return base::Error::Unknown("failed to generate a login key");
  }
  auto written = WriteFileAtomic(key_path_, key);
  if (!written.has_value()) {
    return base::Err(std::move(written).error());
  }
#ifndef _WIN32
  // Owner read/write only: the key is the whole security boundary of the
  // store, so it must not be world-readable like a regular profile file.
  ::chmod(key_path_.c_str(), S_IRUSR | S_IWUSR);
#endif
  return key;
}

} // namespace neko::storage
