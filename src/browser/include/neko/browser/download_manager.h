#pragma once

#include "neko/base/status.h"
#include "neko/network/http.h"
#include "neko/url/url.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace neko::browser {

enum class DownloadState
{
  kPending,
  kInProgress,
  kCompleted,
  kFailed,
  kCancelled
};

std::string_view ToString(DownloadState state);

// One download record.
struct Download
{
  int64_t id = 0;
  std::string url;
  std::string filename; // full destination path
  std::string mime_type;
  int64_t total_bytes = -1; // -1 when unknown
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
//
// Threading: internally synchronized — |mutex_| guards |items_|/|next_id_|
// and is held only around short mutations (never across the network fetch),
// so the GUI thread can read copies through items() while a download runs.
class DownloadManager
{
public:
  using FetchFn = std::function<base::Result<network::HttpResponse>(const url::Url&)>;

  explicit DownloadManager(std::string download_dir, FetchFn fetch = {});

  // Downloads |url| to the download directory and records the outcome.
  // |cookie_header| is an optional "Cookie: ..." value to attach.
  base::Result<Download> Start(const url::Url& url, std::string_view cookie_header);

  // All download records, copied under the lock (safe from any thread).
  std::vector<Download> items() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_;
  }
  size_t size() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
  }
  const Download* Find(int64_t id) const;

  const std::string& download_dir() const
  {
    return download_dir_;
  }

private:
  mutable std::mutex mutex_;
  std::string download_dir_;
  FetchFn fetch_;
  bool custom_fetch_ = false;
  std::vector<Download> items_;
  int64_t next_id_ = 1;
};

} // namespace neko::browser
