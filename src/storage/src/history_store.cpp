#include "neko/storage/history_store.h"

#include <algorithm>
#include <cctype>

#include "neko/base/logging.h"
#include "neko/storage/field_codec.h"
#include "neko/storage/file_util.h"

namespace neko::storage {
namespace {

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

std::string ToLowerAscii(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
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

}  // namespace

HistoryStore::HistoryStore(std::string profile_dir)
    : profile_dir_(std::move(profile_dir)),
      file_path_(profile_dir_ + "/history.txt") {}

base::Result<void> HistoryStore::Load() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
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
    if (fields.size() != 4) {
      NEKO_LOG_WARNING_F("history store: skipping malformed line {}", line_no);
      continue;
    }
    HistoryEntry e;
    auto r_url = DecodeField(fields[0]);
    auto r_title = DecodeField(fields[1]);
    if (!r_url || !r_title || !ParseInt64(fields[2], &e.last_visit) ||
        !ParseInt64(fields[3], &e.visit_count)) {
      NEKO_LOG_WARNING_F("history store: skipping undecodable line {}", line_no);
      continue;
    }
    e.url = std::move(r_url.value());
    e.title = std::move(r_title.value());
    entries_.push_back(std::move(e));
  }
  return base::Error();
}

base::Result<void> HistoryStore::Save() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string out = "# neko-history v1\n";
  for (const auto& e : entries_) {
    out += EncodeField(e.url);
    out += '\t';
    out += EncodeField(e.title);
    out += '\t';
    out += std::to_string(e.last_visit);
    out += '\t';
    out += std::to_string(e.visit_count);
    out += '\n';
  }
  return WriteFileAtomic(file_path_, out);
}

void HistoryStore::RecordVisit(std::string_view url, std::string_view title, int64_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& e : entries_) {
    if (e.url == url) {
      e.title = std::string(title);
      e.last_visit = now;
      ++e.visit_count;
      return;
    }
  }
  entries_.push_back(HistoryEntry{std::string(url), std::string(title), now, 1});
}

std::vector<HistoryEntry> HistoryStore::All() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return AllLocked();
}

std::vector<HistoryEntry> HistoryStore::AllLocked() const {
  std::vector<HistoryEntry> out = entries_;
  std::stable_sort(out.begin(), out.end(),
                   [](const HistoryEntry& a, const HistoryEntry& b) {
                     return a.last_visit > b.last_visit;
                   });
  return out;
}

std::vector<HistoryEntry> HistoryStore::Search(std::string_view query) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (query.empty()) return AllLocked();
  const std::string needle = ToLowerAscii(query);
  std::vector<HistoryEntry> out;
  for (const auto& e : entries_) {
    if (ToLowerAscii(e.url).find(needle) != std::string::npos ||
        ToLowerAscii(e.title).find(needle) != std::string::npos) {
      out.push_back(e);
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const HistoryEntry& a, const HistoryEntry& b) {
                     return a.last_visit > b.last_visit;
                   });
  return out;
}

bool HistoryStore::Remove(std::string_view url) {
  std::lock_guard<std::mutex> lock(mutex_);
  const size_t before = entries_.size();
  std::erase_if(entries_, [&](const HistoryEntry& e) { return e.url == url; });
  return entries_.size() != before;
}

void HistoryStore::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
}

}  // namespace neko::storage
