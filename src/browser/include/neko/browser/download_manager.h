#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "neko/base/status.h"
#include "neko/network/http.h"
#include "neko/url/url.h"

namespace neko::browser {

enum class DownloadState { kPending, kInProgress, kCompleted, kFailed, kCancelled };

std::string_view ToString(DownloadState state);

// One download record.
struct Download {
  int64_t id = 0;
  std::string url;
  std::string filename;  // full destination path
  std::string mime_type;
  int64_t total_bytes = -1;   // -1 when unknown
  int64_t received_bytes = 0;
  DownloadState state = DownloadState::kPending;
  std::string error;
};

// A synchronous download manager.
//
// Downloads run in the calling thread (the project has no async/task
// infrastructure yet; a GUI layer may run Start() on a worker thread).
// Every download is recorded in |items()| so the UI can list past and
// present downloads.
class DownloadManager {
 public:
  using FetchFn = std::function<base::Result<network::HttpResponse>(const url::Url&)>;

  explicit DownloadManager(std::string download_dir, FetchFn fetch = {});

  // Downloads |url| to the download directory and records the outcome.
  // |cookie_header| is an optional "Cookie: ..." value to attach.
  base::Result<Download> Start(const url::Url& url, std::string_view cookie_header);

  const std::vector<Download>& items() const { return items_; }
  size_t size() const { return items_.size(); }
  const Download* Find(int64_t id) const;

  const std::string& download_dir() const { return download_dir_; }

 private:
  std::string download_dir_;
  FetchFn fetch_;
  bool custom_fetch_ = false;
  std::vector<Download> items_;
  int64_t next_id_ = 1;
};

}  // namespace neko::browser
