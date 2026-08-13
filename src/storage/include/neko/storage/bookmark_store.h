#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "neko/base/status.h"

namespace neko::storage {

// One bookmark entry.
struct Bookmark {
  std::string id;        // stable unique id (hex string)
  std::string url;
  std::string title;
  std::string folder;    // "" = top level
  int64_t created = 0;   // unix seconds
};

// A persistent, line-oriented bookmark store.
//
// Threading: single-threaded; call from the owning thread.
class BookmarkStore {
 public:
  explicit BookmarkStore(std::string profile_dir);
  ~BookmarkStore() = default;

  BookmarkStore(const BookmarkStore&) = delete;
  BookmarkStore& operator=(const BookmarkStore&) = delete;

  base::Result<void> Load();
  base::Result<void> Save() const;

  // Adds a bookmark.  Returns the assigned id (empty on failure).
  base::Result<std::string> Add(std::string_view url, std::string_view title,
                                std::string_view folder, int64_t now);

  // Removes a bookmark by id.  Returns true if one was removed.
  bool Remove(std::string_view id);

  // Updates the title of a bookmark by id.  Returns true on success.
  bool UpdateTitle(std::string_view id, std::string_view title);

  // All bookmarks (stable order: insertion order).
  const std::vector<Bookmark>& All() const { return bookmarks_; }

  // Bookmarks in a folder.
  std::vector<const Bookmark*> InFolder(std::string_view folder) const;

  void Clear();
  size_t size() const { return bookmarks_.size(); }

  const std::string& profile_dir() const { return profile_dir_; }

 private:
  std::string profile_dir_;
  std::string file_path_;
  std::vector<Bookmark> bookmarks_;
};

}  // namespace neko::storage
