// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

int BrowserController::NewTab(const std::string& url, bool activate)
{
  int id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto tab = std::make_unique<Tab>();
    tab->id = next_tab_id_++;
    tabs_.push_back(std::move(tab));
    id = tabs_.back()->id;
    if (activate) {
      active_tab_ = static_cast<int>(tabs_.size()) - 1;
    }
  }
  if (!url.empty())
    (void)Navigate(id, url);
  return id;
}

void BrowserController::ActivateTab(int id)
{
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i]->id == id) {
      active_tab_ = static_cast<int>(i);
      return;
    }
  }
}

void BrowserController::CloseTab(int id)
{
  std::shared_ptr<RendererSession> retired;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it =
        std::find_if(tabs_.begin(), tabs_.end(), [id](const auto& t) { return t->id == id; });
    if (it == tabs_.end())
      return;
    const size_t index = static_cast<size_t>(it - tabs_.begin());
    // Take the renderer session out of the tab so the child is shut down
    // after the lock is released (its destructor reaps the process).
    retired = std::move((*it)->session);
    tabs_.erase(it);
    if (tabs_.empty()) {
      active_tab_ = -1;
    } else {
      if (active_tab_ >= static_cast<int>(tabs_.size()))
        active_tab_--;
      // Keep the tab at (or after) the closed one active.
      if (static_cast<int>(index) == active_tab_) {
        active_tab_ = std::min(static_cast<int>(index), static_cast<int>(tabs_.size()) - 1);
      }
    }
  }
  retired.reset();
  // The closed tab's document (DOM, style store, images, frame buffers) is
  // destroyed just above; hand its arena pages back to the OS so closing a
  // tab actually lowers the process footprint (see base::ReleaseFreeMemory).
  base::ReleaseFreeMemory();
}

int BrowserController::active_tab() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return active_tab_;
}

Tab* BrowserController::ActiveTab()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_tab_ < 0 || active_tab_ >= static_cast<int>(tabs_.size()))
    return nullptr;
  return tabs_[static_cast<size_t>(active_tab_)].get();
}

Tab* BrowserController::FindTab(int id)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it =
      std::find_if(tabs_.begin(), tabs_.end(), [id](const auto& t) { return t->id == id; });
  return it == tabs_.end() ? nullptr : it->get();
}

ContentType BrowserController::active_content_type() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_tab_ < 0 || active_tab_ >= static_cast<int>(tabs_.size())) {
    return ContentType::kError;
  }
  return tabs_[static_cast<size_t>(active_tab_)]->content_type;
}

// ---------------------------------------------------------------------------
// GUI snapshots
// ---------------------------------------------------------------------------

std::vector<TabSnapshot> BrowserController::SnapshotTabs() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<TabSnapshot> out;
  out.reserve(tabs_.size());
  for (const auto& tab : tabs_) {
    out.push_back(ToSnapshot(*tab));
  }
  return out;
}

TabSnapshot BrowserController::SnapshotTab(int id) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& tab : tabs_) {
    if (tab->id == id)
      return ToSnapshot(*tab);
  }
  return TabSnapshot{};
}

TabSnapshot BrowserController::SnapshotActiveTab() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_tab_ < 0 || active_tab_ >= static_cast<int>(tabs_.size())) {
    return TabSnapshot{};
  }
  return ToSnapshot(*tabs_[static_cast<size_t>(active_tab_)]);
}

std::vector<storage::HistoryEntry> BrowserController::SnapshotHistory() const
{
  return history_.All();
}

std::vector<storage::Bookmark> BrowserController::SnapshotBookmarks() const
{
  return bookmarks_.All();
}

std::vector<Download> BrowserController::SnapshotDownloads() const
{
  return downloads_.items();
}

size_t BrowserController::SnapshotCookieCount() const
{
  return cookies_.size();
}

std::vector<storage::Cookie> BrowserController::SnapshotCookies() const
{
  return cookies_.All();
}

std::vector<NetworkLogEntry> BrowserController::SnapshotNetworkLog() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return network_log_;
}

std::vector<ConsoleEntry> BrowserController::SnapshotConsoleLog() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return console_log_;
}

