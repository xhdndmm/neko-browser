#include "neko/storage/preferences.h"

#include "neko/storage/field_codec.h"
#include "neko/storage/file_util.h"

#include <algorithm>

namespace neko::storage {

PreferencesStore::PreferencesStore(std::string profile_dir)
    : profile_dir_(std::move(profile_dir)), file_path_(profile_dir_ + "/preferences.txt")
{}

base::Result<void> PreferencesStore::Load()
{
  auto content = ReadFile(file_path_);
  if (!content.has_value()) {
    // A profile without a preferences file is the normal first run.
    return base::Ok();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
  std::string_view rest = content.value();
  while (!rest.empty()) {
    const std::size_t newline = rest.find('\n');
    const std::string_view line = rest.substr(0, newline);
    rest = newline == std::string_view::npos ? std::string_view{} : rest.substr(newline + 1);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
      continue; // malformed line: skip, keep the other entries
    }
    auto key = DecodeField(line.substr(0, eq));
    auto value = DecodeField(line.substr(eq + 1));
    if (!key.has_value() || !value.has_value() || key.value().empty()) {
      continue;
    }
    // The last occurrence wins, keeping the first occurrence's position.
    const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
      return entry.first == key.value();
    });
    if (it != entries_.end()) {
      it->second = std::move(value.value());
    } else {
      entries_.emplace_back(std::move(key.value()), std::move(value.value()));
    }
  }
  return base::Ok();
}

base::Result<void> PreferencesStore::Save() const
{
  std::string out;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, value] : entries_) {
      out += EncodeField(key);
      out += '=';
      out += EncodeField(value);
      out += '\n';
    }
  }
  return WriteFileAtomic(file_path_, out);
}

std::string PreferencesStore::Get(std::string_view key, std::string_view fallback) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [entry_key, entry_value] : entries_) {
    if (entry_key == key) {
      return entry_value;
    }
  }
  return std::string(fallback);
}

void PreferencesStore::Set(std::string_view key, std::string_view value)
{
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [entry_key, entry_value] : entries_) {
    if (entry_key == key) {
      entry_value = std::string(value);
      return;
    }
  }
  entries_.emplace_back(std::string(key), std::string(value));
}

bool PreferencesStore::Unset(std::string_view key)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = std::find_if(
      entries_.begin(), entries_.end(), [&](const auto& entry) { return entry.first == key; });
  if (it == entries_.end()) {
    return false;
  }
  entries_.erase(it);
  return true;
}

bool PreferencesStore::Has(std::string_view key) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return std::any_of(
      entries_.begin(), entries_.end(), [&](const auto& entry) { return entry.first == key; });
}

std::vector<std::pair<std::string, std::string>> PreferencesStore::All() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_;
}

} // namespace neko::storage
