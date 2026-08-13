#include "neko/browser/download_manager.h"

#include <algorithm>
#include <cctype>

#include "neko/base/logging.h"
#include "neko/network/http.h"
#include "neko/storage/file_util.h"

namespace neko::browser {

std::string_view ToString(DownloadState state) {
  switch (state) {
    case DownloadState::kPending: return "pending";
    case DownloadState::kInProgress: return "in-progress";
    case DownloadState::kCompleted: return "completed";
    case DownloadState::kFailed: return "failed";
    case DownloadState::kCancelled: return "cancelled";
  }
  return "unknown";
}

namespace {

// Extracts a safe basename for |url|: the last path segment, percent-decoded
// and stripped of control characters and path separators.
std::string BasenameFromUrl(const url::Url& url) {
  std::string name = url.path();
  const size_t slash = name.find_last_of('/');
  if (slash != std::string::npos) name = name.substr(slash + 1);
  name = url::PercentDecode(name);
  // Strip characters that are unsafe in filenames.
  std::string clean;
  for (const char raw : name) {
    const unsigned char c = static_cast<unsigned char>(raw);
    if (c == '/' || c == '\\' || c < 0x20 || c == 0x7F) continue;
    clean.push_back(static_cast<char>(c));
  }
  if (clean.empty() || clean == "." || clean == "..") clean = "download";
  if (clean.size() > 120) clean = clean.substr(0, 120);
  return clean;
}

// Extracts filename= from a Content-Disposition header (quoted or bare).
std::string FilenameFromDisposition(std::string_view value) {
  const size_t pos = value.find("filename=");
  if (pos == std::string_view::npos) return {};
  std::string_view rest = value.substr(pos + 9);
  if (rest.empty()) return {};
  if (rest.front() == '"') {
    rest.remove_prefix(1);
    const size_t end = rest.find('"');
    return std::string(end == std::string_view::npos ? rest : rest.substr(0, end));
  }
  const size_t end = rest.find(';');
  return std::string(end == std::string_view::npos ? rest : rest.substr(0, end));
}

}  // namespace

DownloadManager::DownloadManager(std::string download_dir, FetchFn fetch)
    : download_dir_(std::move(download_dir)), fetch_(std::move(fetch)) {
  custom_fetch_ = static_cast<bool>(fetch_);
}

const Download* DownloadManager::Find(int64_t id) const {
  const auto it = std::find_if(items_.begin(), items_.end(),
                               [id](const Download& d) { return d.id == id; });
  return it == items_.end() ? nullptr : &*it;
}

base::Result<Download> DownloadManager::Start(const url::Url& url,
                                              std::string_view cookie_header) {
  Download record;
  record.id = next_id_++;
  record.url = url.Serialize();
  record.state = DownloadState::kInProgress;

  auto fetch_lambda = [&]() -> base::Result<network::HttpResponse> {
    if (custom_fetch_) {
      // Injectable fetcher (used by tests); cookie handling is the caller's
      // concern there.
      return fetch_(url);
    }
    network::HeaderProvider provider;
    if (!cookie_header.empty()) {
      provider = [cookie_header = std::string(cookie_header)](const url::Url&) {
        return std::vector<network::HttpHeader>{{ "cookie", cookie_header }};
      };
    }
    return network::HttpGet(url, 5, provider);
  };
  auto response = fetch_lambda();
  if (!response) {
    record.state = DownloadState::kFailed;
    record.error = response.error().message();
    items_.push_back(record);
    return base::Err(response.error());
  }

  record.mime_type = response.value().GetHeader("content-type");
  record.total_bytes = static_cast<int64_t>(response.value().body.size());
  record.received_bytes = record.total_bytes;

  // Destination filename: Content-Disposition wins, else URL basename.
  std::string filename = FilenameFromDisposition(response.value().GetHeader("content-disposition"));
  if (filename.empty()) filename = BasenameFromUrl(url);
  // Prevent directory traversal in Content-Disposition.
  std::replace(filename.begin(), filename.end(), '/', '_');
  std::replace(filename.begin(), filename.end(), '\\', '_');
  if (filename.empty()) filename = "download";
  record.filename = download_dir_ + "/" + filename;

  auto written = storage::WriteFileAtomic(record.filename, response.value().body);
  if (!written) {
    record.state = DownloadState::kFailed;
    record.error = written.error().message();
    items_.push_back(record);
    return base::Err(written.error());
  }

  record.state = DownloadState::kCompleted;
  items_.push_back(record);
  NEKO_LOG_INFO("download " + std::to_string(record.id) + " -> " + record.filename +
                " (" + std::to_string(record.total_bytes) + " bytes)");
  return record;
}

}  // namespace neko::browser
