#include "neko/storage/bookmark_store.h"

#include <cctype>
#include <random>

#include "neko/base/logging.h"
#include "neko/storage/field_codec.h"
#include "neko/storage/file_util.h"

namespace neko::storage {
namespace {

constexpr char kHex[] = "0123456789abcdef";

bool ParseInt64(std::string_view s, int64_t* out) {
  if (s.empty()) return false;
  int64_t v = 0;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    v = v * 10 + (c - '0');
    if (v < 0) return false;
  }
  *out = v;
  return true;
}

std::vector<std::string_view> SplitTabs(std::string_view line) {
  std::vector<std::string_view> fields;
  size_t start = 0;
  while (true) {
    const size_t tab = line.find('\t', start);
    if (tab == std::string_view::npos) {
      fields.push_back(line.substr(start));
      break;
    }
    fields.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
  return fields;
}

std::string NewId() {
  std::random_device rd;
  std::uniform_int_distribution<int> dist(0, 15);
  std::string id(16, '0');
  for (char& c : id) c = kHex[dist(rd)];
  return id;
}

}  // namespace

BookmarkStore::BookmarkStore(std::string profile_dir)
    : profile_dir_(std::move(profile_dir)),
      file_path_(profile_dir_ + "/bookmarks.txt") {}

base::Result<void> BookmarkStore::Load() {
  bookmarks_.clear();
  auto maybe_data = ReadFile(file_path_);
  if (!maybe_data) {
    if (maybe_data.error().category() == base::ErrorCategory::kIo) return base::Error();
    return maybe_data.error();
  }
  const std::string& data = maybe_data.value();
  size_t pos = 0;
  int line_no = 0;
  while (pos < data.size()) {
    const size_t nl = data.find('\n', pos);
    const size_t end = (nl == std::string::npos) ? data.size() : nl;
    const std::string_view line(data.data() + pos, end - pos);
    pos = (nl == std::string::npos) ? data.size() : nl + 1;
    ++line_no;
    if (line.empty() || line.front() == '#') continue;

    const auto fields = SplitTabs(line);
    if (fields.size() != 5) {
      NEKO_LOG_WARNING_F("bookmark store: skipping malformed line {}", line_no);
      continue;
    }
    Bookmark b;
    auto r_id = DecodeField(fields[0]);
    auto r_url = DecodeField(fields[1]);
    auto r_title = DecodeField(fields[2]);
    auto r_folder = DecodeField(fields[3]);
    if (!r_id || !r_url || !r_title || !r_folder || !ParseInt64(fields[4], &b.created)) {
      NEKO_LOG_WARNING_F("bookmark store: skipping undecodable line {}", line_no);
      continue;
    }
    b.id = std::move(r_id.value());
    b.url = std::move(r_url.value());
    b.title = std::move(r_title.value());
    b.folder = std::move(r_folder.value());
    bookmarks_.push_back(std::move(b));
  }
  return base::Error();
}

base::Result<void> BookmarkStore::Save() const {
  std::string out = "# neko-bookmarks v1\n";
  for (const auto& b : bookmarks_) {
    out += EncodeField(b.id);
    out += '\t';
    out += EncodeField(b.url);
    out += '\t';
    out += EncodeField(b.title);
    out += '\t';
    out += EncodeField(b.folder);
    out += '\t';
    out += std::to_string(b.created);
    out += '\n';
  }
  return WriteFileAtomic(file_path_, out);
}

base::Result<std::string> BookmarkStore::Add(std::string_view url, std::string_view title,
                                             std::string_view folder, int64_t now) {
  if (url.empty()) {
    return base::Error::InvalidArgument("bookmark url must not be empty");
  }
  const std::string id = NewId();
  bookmarks_.push_back(Bookmark{id, std::string(url), std::string(title),
                                std::string(folder), now});
  return id;
}

bool BookmarkStore::Remove(std::string_view id) {
  const size_t before = bookmarks_.size();
  std::erase_if(bookmarks_, [&](const Bookmark& b) { return b.id == id; });
  return bookmarks_.size() != before;
}

bool BookmarkStore::UpdateTitle(std::string_view id, std::string_view title) {
  for (auto& b : bookmarks_) {
    if (b.id == id) {
      b.title = std::string(title);
      return true;
    }
  }
  return false;
}

std::vector<const Bookmark*> BookmarkStore::InFolder(std::string_view folder) const {
  std::vector<const Bookmark*> out;
  for (const auto& b : bookmarks_) {
    if (b.folder == folder) out.push_back(&b);
  }
  return out;
}

void BookmarkStore::Clear() { bookmarks_.clear(); }

}  // namespace neko::storage
