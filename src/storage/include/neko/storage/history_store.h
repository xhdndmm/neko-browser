#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "neko/base/status.h"

namespace neko::storage {

// One entry in the browsing history.
struct HistoryEntry {
  std::string url;
  std::string title;
  int64_t last_visit = 0;   // unix seconds
  int64_t visit_count = 1;
};

// A persistent, line-oriented browsing history.
//
// Visit(navigation) is O(n); fine for the current single-process design and
// honest about it.  Entries are keyed by URL: revisiting updates the visit
// count and timestamp instead of inserting a duplicate.
//
// Threading: single-threaded; call from the owning thread.
class HistoryStore {
 public:
  explicit HistoryStore(std::string profile_dir);
  ~HistoryStore() = default;

  HistoryStore(const HistoryStore&) = delete;
  HistoryStore& operator=(const HistoryStore&) = delete;

  base::Result<void> Load();
  base::Result<void> Save() const;

  // Records a visit to |url| at |now|, updating the existing entry if any.
  void RecordVisit(std::string_view url, std::string_view title, int64_t now);

  // All entries sorted by last_visit (most recent first).
  std::vector<HistoryEntry> All() const;

  // Entries whose URL or title contains |query| (case-insensitive).
  std::vector<HistoryEntry> Search(std::string_view query) const;

  // Removes one URL.  Returns true if an entry was removed.
  bool Remove(std::string_view url);

  void Clear();
  size_t size() const { return entries_.size(); }
  bool empty() const { return entries_.empty(); }

  const std::string& profile_dir() const { return profile_dir_; }

 private:
  std::string profile_dir_;
  std::string file_path_;
  std::vector<HistoryEntry> entries_;
};

}  // namespace neko::storage
