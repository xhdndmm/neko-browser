#pragma once

#include "neko/base/status.h"

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace neko::storage {

// A persistent web-local storage (WHATWG HTML localStorage), as a key-value
// store partitioned by origin.
//
// Entries are triples (origin, key, value) persisted as percent-encoded,
// tab-separated fields in local_storage.txt with atomic writes (same scheme
// as the cookie/history/bookmark stores).  Values are stored verbatim
// (including empty strings); only the presence of a key is meaningful.
//
// Limitations (documented):
//   * No quota (a localStorage origin in browsers is capped at ~5 MiB).
//   * No storage event notification.
//   * Not yet reachable from JavaScript (Phase 8 milestone 2 Web IDL).
//
// Threading: internally synchronized — every public method guards its
// mutation/read with |mutex_|, matching the other stores.
class LocalStorage
{
public:
  explicit LocalStorage(std::string profile_dir);
  ~LocalStorage() = default;

  LocalStorage(const LocalStorage&) = delete;
  LocalStorage& operator=(const LocalStorage&) = delete;

  // Loads the store file if present.  Missing file == empty store (no error).
  base::Result<void> Load();

  // Persists all entries atomically.
  base::Result<void> Save() const;

  // Sets |key| to |value| for |origin| (inserting or replacing the entry).
  void SetItem(std::string_view origin, std::string_view key, std::string_view value);

  // Value for |key| at |origin|, or nullopt when the key is absent.
  std::optional<std::string> GetItem(std::string_view origin, std::string_view key) const;

  // Removes |key| at |origin|.  Returns true when an entry was removed.
  bool RemoveItem(std::string_view origin, std::string_view key);

  // Removes every key stored under |origin|.
  void Clear(std::string_view origin);

  // Removes every entry (all origins).
  void ClearAll();

  // All (key, value) pairs for |origin|, in insertion order.  Copy under lock.
  std::vector<std::pair<std::string, std::string>> All(std::string_view origin) const;

  // Total number of stored entries across all origins.
  std::size_t size() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
  }
  bool empty() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.empty();
  }

  const std::string& profile_dir() const
  {
    return profile_dir_;
  }

private:
  // Origin, key, value (insertion-ordered).
  std::vector<std::tuple<std::string, std::string, std::string>> entries_;

  mutable std::mutex mutex_;
  std::string profile_dir_;
  std::string file_path_;
};

} // namespace neko::storage
