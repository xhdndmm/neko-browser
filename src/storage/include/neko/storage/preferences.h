#pragma once

#include "neko/base/status.h"

#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neko::storage {

// A small persistent key-value store for browser preferences (home page,
// default search engine, bookmark-bar visibility, ...).
//
// Format: one "key=value" line per entry, both sides percent-encoded with
// field_codec so values may contain '=', control characters and newlines.
// Unknown keys are preserved verbatim across Load/Save, so a profile written
// by a newer build survives a round-trip through an older one.  Saves are
// atomic (WriteFileAtomic).
//
// Threading: internally synchronized; every public method takes |mutex_|.
class PreferencesStore
{
public:
  explicit PreferencesStore(std::string profile_dir);
  ~PreferencesStore() = default;

  PreferencesStore(const PreferencesStore&) = delete;
  PreferencesStore& operator=(const PreferencesStore&) = delete;

  // Reads <profile>/preferences.txt.  A missing file is not an error (the
  // store stays empty).  Malformed lines are skipped; every other entry still
  // loads.
  base::Result<void> Load();
  base::Result<void> Save() const;

  // The value for |key|, or |fallback| when unset.
  std::string Get(std::string_view key, std::string_view fallback = {}) const;

  // Sets |key| to |value| (empty values are stored as-is).
  void Set(std::string_view key, std::string_view value);

  // Unsets |key|.  Returns true when it existed.
  bool Unset(std::string_view key);

  // True when |key| has a value.
  bool Has(std::string_view key) const;

  // All entries, in file order.  Copy under lock.
  std::vector<std::pair<std::string, std::string>> All() const;

  const std::string& profile_dir() const
  {
    return profile_dir_;
  }

private:
  mutable std::mutex mutex_;
  std::string profile_dir_;
  std::string file_path_;
  std::vector<std::pair<std::string, std::string>> entries_; // preserves order
};

} // namespace neko::storage
