#include "neko/storage/local_storage.h"

#include "neko/base/logging.h"
#include "neko/storage/field_codec.h"
#include "neko/storage/file_util.h"

#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace neko::storage {
namespace {} // namespace

LocalStorage::LocalStorage(std::string profile_dir)
    : profile_dir_(std::move(profile_dir)), file_path_(profile_dir_ + "/local_storage.txt")
{}

base::Result<void> LocalStorage::Load()
{
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
  auto maybe_data = ReadFile(file_path_);
  if (!maybe_data) {
    if (maybe_data.error().category() == base::ErrorCategory::kIo)
      return base::Error();
    return maybe_data.error();
  }
  const std::string& data = maybe_data.value();
  std::size_t pos = 0;
  int line_no = 0;
  while (pos < data.size()) {
    const std::size_t nl = data.find('\n', pos);
    const std::size_t end = (nl == std::string::npos) ? data.size() : nl;
    const std::string_view line(data.data() + pos, end - pos);
    pos = (nl == std::string::npos) ? data.size() : nl + 1;
    ++line_no;
    if (line.empty() || line.front() == '#')
      continue;

    const auto fields = SplitTabFields(line);
    if (fields.size() != 3) {
      NEKO_LOG_WARNING_F("local storage: skipping malformed line {}", line_no);
      continue;
    }
    auto r_origin = DecodeField(fields[0]);
    auto r_key = DecodeField(fields[1]);
    auto r_value = DecodeField(fields[2]);
    if (!r_origin || !r_key || !r_value) {
      NEKO_LOG_WARNING_F("local storage: skipping undecodable line {}", line_no);
      continue;
    }
    entries_.emplace_back(
        std::move(r_origin.value()), std::move(r_key.value()), std::move(r_value.value()));
  }
  return base::Error();
}

base::Result<void> LocalStorage::Save() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::string out = "# neko-local-storage v1\n";
  for (const auto& [origin, key, value] : entries_) {
    out += EncodeField(origin);
    out += '\t';
    out += EncodeField(key);
    out += '\t';
    out += EncodeField(value);
    out += '\n';
  }
  return WriteFileAtomic(file_path_, out);
}

void LocalStorage::SetItem(std::string_view origin, std::string_view key, std::string_view value)
{
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [o, k, v] : entries_) {
    if (o == origin && k == key) {
      v = std::string(value);
      return;
    }
  }
  entries_.emplace_back(std::string(origin), std::string(key), std::string(value));
}

std::optional<std::string> LocalStorage::GetItem(std::string_view origin,
                                                 std::string_view key) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [o, k, v] : entries_) {
    if (o == origin && k == key) {
      return v;
    }
  }
  return std::nullopt;
}

bool LocalStorage::RemoveItem(std::string_view origin, std::string_view key)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const std::size_t before = entries_.size();
  std::erase_if(entries_,
                [&](const auto& e) { return std::get<0>(e) == origin && std::get<1>(e) == key; });
  return entries_.size() != before;
}

void LocalStorage::Clear(std::string_view origin)
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::erase_if(entries_, [&](const auto& e) { return std::get<0>(e) == origin; });
}

void LocalStorage::ClearAll()
{
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
}

std::vector<std::pair<std::string, std::string>> LocalStorage::All(std::string_view origin) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& [o, k, v] : entries_) {
    if (o == origin) {
      out.emplace_back(k, v);
    }
  }
  return out;
}

} // namespace neko::storage
