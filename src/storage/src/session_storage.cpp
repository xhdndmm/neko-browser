#include "neko/storage/session_storage.h"

#include <algorithm>

namespace neko::storage {

void SessionStorage::SetItem(std::string_view origin, std::string_view key, std::string_view value)
{
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [entry_origin, entry_key, entry_value] : entries_) {
    if (entry_origin == origin && entry_key == key) {
      entry_value.assign(value);
      return;
    }
  }
  entries_.emplace_back(std::string(origin), std::string(key), std::string(value));
}

std::optional<std::string> SessionStorage::GetItem(std::string_view origin,
                                                   std::string_view key) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [entry_origin, entry_key, entry_value] : entries_) {
    if (entry_origin == origin && entry_key == key) {
      return entry_value;
    }
  }
  return std::nullopt;
}

bool SessionStorage::RemoveItem(std::string_view origin, std::string_view key)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
    return std::get<0>(entry) == origin && std::get<1>(entry) == key;
  });
  if (it == entries_.end()) {
    return false;
  }
  entries_.erase(it);
  return true;
}

void SessionStorage::Clear(std::string_view origin)
{
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.erase(std::remove_if(entries_.begin(),
                                entries_.end(),
                                [&](const auto& entry) { return std::get<0>(entry) == origin; }),
                 entries_.end());
}

void SessionStorage::ClearAll()
{
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
}

std::vector<std::pair<std::string, std::string>> SessionStorage::All(std::string_view origin) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::pair<std::string, std::string>> result;
  for (const auto& [entry_origin, entry_key, entry_value] : entries_) {
    if (entry_origin == origin) {
      result.emplace_back(entry_key, entry_value);
    }
  }
  return result;
}

} // namespace neko::storage
