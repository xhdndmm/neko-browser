#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace neko::storage {

// window.sessionStorage backing store: in-memory, partitioned by origin, with
// a lifetime of one top-level browsing context (tab/session) instead of the
// document (WHATWG HTML 7.1: "session storage" is scoped to a tab and survives
// navigations within it, but not a new tab and not a browser restart).
//
// The class intentionally mirrors LocalStorage's accessor surface so the
// browser layer can wire both stores the same way; the only difference is
// that nothing is persisted.
//
// Threading: all methods lock an internal mutex (the binder may run on the
// worker thread while the GUI reads snapshots).
class SessionStorage
{
public:
  SessionStorage() = default;
  ~SessionStorage() = default;

  SessionStorage(const SessionStorage&) = delete;
  SessionStorage& operator=(const SessionStorage&) = delete;

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

private:
  // Origin, key, value (insertion-ordered), mirroring LocalStorage.
  std::vector<std::tuple<std::string, std::string, std::string>> entries_;

  mutable std::mutex mutex_;
};

} // namespace neko::storage
