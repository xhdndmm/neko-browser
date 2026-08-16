#include "neko/storage/indexed_db.h"

#include "neko/storage/field_codec.h"
#include "neko/storage/file_util.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

namespace neko::storage {
namespace {

constexpr const char* kHeaderLine = "# neko-indexeddb v1";
// Line kinds: D(database+version), S(object store), R(record).
constexpr char kDb = 'D';
constexpr char kStore = 'S';
constexpr char kRecord = 'R';

} // namespace

// ---------------------------------------------------------------------------
// Key ordering: numbers before strings (IndexedDB key order), each ascending.
// ---------------------------------------------------------------------------

bool IndexedDbStore::Key::operator<(const Key& other) const
{
  if (is_number != other.is_number) {
    return is_number; // numbers sort first
  }
  if (is_number) {
    return number < other.number;
  }
  return string < other.string;
}

base::Result<IndexedDbStore::Key> IndexedDbStore::ParseKey(std::string_view json)
{
  base::Result<JsonValue> parsed = ParseJson(json);
  if (!parsed.has_value()) {
    return base::Error::InvalidArgument(std::string("IDB:DataError:") + parsed.error().message());
  }
  Key key;
  if (const auto* n = std::get_if<double>(&parsed.value().value)) {
    key.is_number = true;
    key.number = *n;
    return key;
  }
  if (const auto* str = std::get_if<std::string>(&parsed.value().value)) {
    key.is_number = false;
    key.string = *str;
    return key;
  }
  return base::Error::InvalidArgument("IDB:DataError:key must be a number or a string");
}

std::string IndexedDbStore::SerializeKey(const Key& key)
{
  if (key.is_number) {
    JsonValue number(key.number);
    return SerializeJson(number);
  }
  JsonValue str(key.string);
  return SerializeJson(str);
}

base::Result<std::optional<IndexedDbStore::Key>>
IndexedDbStore::KeyFromValue(const ObjectStore& store, const JsonValue& value)
{
  if (store.key_path.empty()) {
    return std::optional<Key>();
  }
  const auto* obj = std::get_if<JsonObject>(&value.value);
  if (obj == nullptr) {
    return base::Error::InvalidArgument(
        "IDB:DataError:value is not an object but the store has a key path");
  }
  const auto it = obj->find(store.key_path);
  if (it == obj->end()) {
    return std::optional<Key>(); // caller falls back to auto-increment / DataError
  }
  Key key;
  if (const auto* n = std::get_if<double>(&it->second.value)) {
    key.is_number = true;
    key.number = *n;
    return std::optional<Key>(std::move(key));
  }
  if (const auto* str = std::get_if<std::string>(&it->second.value)) {
    key.is_number = false;
    key.string = *str;
    return std::optional<Key>(std::move(key));
  }
  return base::Error::InvalidArgument(
      "IDB:DataError:key-path property is neither a number nor a string");
}

void IndexedDbStore::InjectKey(const ObjectStore& store, const Key& key, JsonValue& value)
{
  auto* obj = std::get_if<JsonObject>(&value.value);
  if (obj == nullptr) {
    return;
  }
  JsonValue member;
  if (key.is_number) {
    // Integral values serialize as JSON integers ("2", not "2.0").
    if (std::floor(key.number) == key.number && std::fabs(key.number) <= 9007199254740991.0) {
      member = JsonValue(static_cast<double>(static_cast<int64_t>(key.number)));
    } else {
      member = JsonValue(key.number);
    }
  } else {
    member = JsonValue(key.string);
  }
  (*obj)[store.key_path] = std::move(member);
}

base::Error IndexedDbStore::IdbError(std::string_view name, std::string message)
{
  return base::Error::InvalidArgument("IDB:" + std::string(name) + ":" + std::move(message));
}

// ---------------------------------------------------------------------------
// Construction / persistence
// ---------------------------------------------------------------------------

IndexedDbStore::IndexedDbStore(std::string profile_dir)
    : profile_dir_(std::move(profile_dir)), file_path_(profile_dir_ + "/indexed_db.txt")
{}

base::Result<void> IndexedDbStore::Load()
{
  std::lock_guard<std::mutex> lock(mutex_);
  databases_.clear();
  base::Result<std::string> file = ReadFile(file_path_);
  if (!file.has_value()) {
    if (file.error().category() == base::ErrorCategory::kIo) {
      return base::Ok(); // missing file == empty store
    }
    return file.error();
  }
  std::size_t line_no = 0;
  std::size_t pos = 0;
  const std::string& data = file.value();
  while (pos < data.size()) {
    ++line_no;
    const std::size_t nl = data.find('\n', pos);
    const std::size_t len = nl == std::string::npos ? data.size() - pos : nl - pos;
    // NOTE: view directly into |data| — binding to the temporary returned by
    // data.substr() would dangle.
    const std::string_view line(data.data() + pos, len);
    pos = nl == std::string::npos ? data.size() : nl + 1;
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const std::vector<std::string_view> fields = SplitTabFields(line);
    auto decode = [&](std::size_t i) -> base::Result<std::string> {
      if (i >= fields.size()) {
        return base::Error::Parse("indexed_db: short line");
      }
      return DecodeField(fields[i]);
    };
    const base::Result<std::string> kind = decode(0);
    const base::Result<std::string> origin = decode(1);
    if (!kind.has_value() || !origin.has_value() || kind.value().empty()) {
      continue; // bad line: skip, like the other stores
    }
    Database* db = nullptr;
    if (kind.value()[0] == kDb) {
      const base::Result<std::string> name = decode(2);
      const base::Result<std::string> version = decode(3);
      if (!name.has_value() || !version.has_value()) {
        continue;
      }
      long long parsed_version = 0;
      if (std::sscanf(version.value().c_str(), "%lld", &parsed_version) != 1) {
        continue;
      }
      Database fresh;
      fresh.name = name.value();
      fresh.version = static_cast<int64_t>(parsed_version);
      db = &databases_[origin.value()][name.value()];
      *db = std::move(fresh);
      continue;
    }
    auto& origin_dbs = databases_[origin.value()];
    const base::Result<std::string> dbname = decode(2);
    if (!dbname.has_value()) {
      continue;
    }
    const auto db_it = origin_dbs.find(dbname.value());
    if (db_it == origin_dbs.end()) {
      continue; // orphaned S/R line
    }
    db = &db_it->second;
    if (kind.value()[0] == kStore) {
      const base::Result<std::string> store_name = decode(3);
      const base::Result<std::string> key_path = decode(4);
      const base::Result<std::string> auto_inc = decode(5);
      const base::Result<std::string> next = decode(6);
      if (!store_name.has_value() || !key_path.has_value() || !auto_inc.has_value() ||
          !next.has_value()) {
        continue;
      }
      ObjectStore store;
      store.name = store_name.value();
      store.key_path = key_path.value();
      store.auto_increment = auto_inc.value() == "1";
      store.next_number = std::atof(next.value().c_str());
      if (!(store.next_number >= 1)) {
        store.next_number = 1;
      }
      db->stores[store.name] = std::move(store);
      continue;
    }
    if (kind.value()[0] == kRecord) {
      const base::Result<std::string> store_name = decode(3);
      const base::Result<std::string> key_json = decode(4);
      const base::Result<std::string> value_json = decode(5);
      if (!store_name.has_value() || !key_json.has_value() || !value_json.has_value()) {
        continue;
      }
      const auto store_it = db->stores.find(store_name.value());
      if (store_it == db->stores.end()) {
        continue;
      }
      const base::Result<Key> key = ParseKey(key_json.value());
      base::Result<JsonValue> value = ParseJson(value_json.value());
      if (!key.has_value() || !value.has_value()) {
        continue;
      }
      store_it->second.records.emplace(key.value(), std::move(value.value()));
      if (key.value().is_number && key.value().number + 1 > store_it->second.next_number) {
        store_it->second.next_number = key.value().number + 1;
      }
      continue;
    }
  }
  return base::Ok();
}

base::Result<void> IndexedDbStore::Save() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return SaveLocked();
}

base::Result<void> IndexedDbStore::SaveLocked() const
{
  std::string out = kHeaderLine;
  out.push_back('\n');
  for (const auto& [origin, dbs] : databases_) {
    for (const auto& [name, db] : dbs) {
      out.push_back(kDb);
      out.push_back('\t');
      out += EncodeField(origin);
      out.push_back('\t');
      out += EncodeField(name);
      out.push_back('\t');
      out += EncodeField(std::to_string(db.version));
      out.push_back('\n');
      for (const auto& [store_name, store] : db.stores) {
        out.push_back(kStore);
        out.push_back('\t');
        out += EncodeField(origin);
        out.push_back('\t');
        out += EncodeField(name);
        out.push_back('\t');
        out += EncodeField(store_name);
        out.push_back('\t');
        out += EncodeField(store.key_path);
        out.push_back('\t');
        out += EncodeField(store.auto_increment ? "1" : "0");
        out.push_back('\t');
        char next_buf[32];
        std::snprintf(next_buf, sizeof(next_buf), "%.17g", store.next_number);
        out += EncodeField(next_buf);
        out.push_back('\n');
        for (const auto& [key, value] : store.records) {
          out.push_back(kRecord);
          out.push_back('\t');
          out += EncodeField(origin);
          out.push_back('\t');
          out += EncodeField(name);
          out.push_back('\t');
          out += EncodeField(store_name);
          out.push_back('\t');
          out += EncodeField(SerializeKey(key));
          out.push_back('\t');
          out += EncodeField(SerializeJson(value));
          out.push_back('\n');
        }
      }
    }
  }
  return WriteFileAtomic(file_path_, out);
}

// ---------------------------------------------------------------------------
// Databases
// ---------------------------------------------------------------------------

base::Result<int64_t> IndexedDbStore::CurrentVersion(std::string_view origin,
                                                     std::string_view db) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto origin_it = databases_.find(std::string(origin));
  if (origin_it == databases_.end()) {
    return 0;
  }
  const auto db_it = origin_it->second.find(std::string(db));
  return db_it == origin_it->second.end() ? 0 : db_it->second.version;
}

base::Result<int64_t> IndexedDbStore::CreateDatabase(std::string_view origin, std::string_view db)
{
  std::lock_guard<std::mutex> lock(mutex_);
  auto& dbs = databases_[std::string(origin)];
  const std::string name(db);
  if (dbs.count(name) != 0) {
    return IdbError("InvalidStateError", "database already exists");
  }
  Database fresh;
  fresh.name = name;
  fresh.version = 0;
  dbs[name] = std::move(fresh);
  base::Result<void> saved = SaveLocked();
  if (!saved) {
    dbs.erase(name);
    return saved.error();
  }
  return 0;
}

base::Result<void>
IndexedDbStore::SetVersion(std::string_view origin, std::string_view db, int64_t version)
{
  if (version < 1) {
    return IdbError("VersionError", "version must be at least 1");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto origin_it = databases_.find(std::string(origin));
  if (origin_it == databases_.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  const auto db_it = origin_it->second.find(std::string(db));
  if (db_it == origin_it->second.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  if (version < db_it->second.version) {
    return IdbError("VersionError", "version cannot decrease");
  }
  db_it->second.version = version;
  return SaveLocked();
}

base::Result<void> IndexedDbStore::DeleteDatabase(std::string_view origin, std::string_view db)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto origin_it = databases_.find(std::string(origin));
  if (origin_it == databases_.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  origin_it->second.erase(std::string(db));
  if (origin_it->second.empty()) {
    databases_.erase(origin_it);
  }
  return SaveLocked();
}

base::Result<std::vector<IndexedDbStore::ObjectStoreMeta>>
IndexedDbStore::ObjectStores(std::string_view origin, std::string_view db) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto origin_it = databases_.find(std::string(origin));
  if (origin_it == databases_.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  const auto db_it = origin_it->second.find(std::string(db));
  if (db_it == origin_it->second.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  std::vector<ObjectStoreMeta> out;
  for (const auto& [name, store] : db_it->second.stores) {
    ObjectStoreMeta meta;
    meta.name = name;
    meta.key_path = store.key_path;
    meta.auto_increment = store.auto_increment;
    out.push_back(std::move(meta));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Object stores
// ---------------------------------------------------------------------------

base::Result<void> IndexedDbStore::CreateObjectStore(std::string_view origin,
                                                     std::string_view db,
                                                     std::string_view store,
                                                     std::string_view key_path,
                                                     bool auto_increment)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto origin_it = databases_.find(std::string(origin));
  if (origin_it == databases_.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  const auto db_it = origin_it->second.find(std::string(db));
  if (db_it == origin_it->second.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  if (db_it->second.stores.count(std::string(store)) != 0) {
    return IdbError("ConstraintError", "object store already exists");
  }
  ObjectStore fresh;
  fresh.name = std::string(store);
  fresh.key_path = std::string(key_path);
  fresh.auto_increment = auto_increment;
  db_it->second.stores[fresh.name] = std::move(fresh);
  return SaveLocked();
}

base::Result<void> IndexedDbStore::DeleteObjectStore(std::string_view origin,
                                                     std::string_view db,
                                                     std::string_view store)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto origin_it = databases_.find(std::string(origin));
  if (origin_it == databases_.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  const auto db_it = origin_it->second.find(std::string(db));
  if (db_it == origin_it->second.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  if (db_it->second.stores.erase(std::string(store)) == 0) {
    return IdbError("NotFoundError", "object store does not exist");
  }
  return SaveLocked();
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

base::Result<std::string> IndexedDbStore::WriteRecord(std::string_view origin,
                                                      std::string_view db,
                                                      std::string_view store,
                                                      std::optional<std::string> key_json,
                                                      std::string value_json,
                                                      bool add_only)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto origin_it = databases_.find(std::string(origin));
  if (origin_it == databases_.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  const auto db_it = origin_it->second.find(std::string(db));
  if (db_it == origin_it->second.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  const auto store_it = db_it->second.stores.find(std::string(store));
  if (store_it == db_it->second.stores.end()) {
    return IdbError("NotFoundError", "object store does not exist");
  }
  ObjectStore& store_state = store_it->second;

  base::Result<JsonValue> value = ParseJson(value_json);
  if (!value.has_value()) {
    return IdbError("DataError", value.error().message());
  }

  std::optional<Key> key;
  if (key_json.has_value()) {
    if (!store_state.key_path.empty()) {
      return IdbError("DataError", "out-of-line key on a key-path store");
    }
    base::Result<Key> parsed = ParseKey(*key_json);
    if (!parsed.has_value()) {
      return parsed.error();
    }
    key = parsed.value();
  }
  if (!key.has_value() && !store_state.key_path.empty()) {
    base::Result<std::optional<Key>> from_value = KeyFromValue(store_state, value.value());
    if (!from_value.has_value()) {
      return from_value.error();
    }
    key = from_value.value();
  }
  if (!key.has_value() && store_state.auto_increment) {
    Key generated;
    generated.is_number = true;
    generated.number = store_state.next_number;
    store_state.next_number += 1;
    if (!store_state.key_path.empty()) {
      InjectKey(store_state, generated, value.value());
    }
    key = generated;
  }
  if (!key.has_value()) {
    return IdbError("DataError", "record is missing a key");
  }

  const auto existing = store_state.records.find(*key);
  if (existing != store_state.records.end()) {
    if (add_only) {
      return IdbError("ConstraintError", "a record with this key already exists");
    }
    existing->second = value.value();
  } else {
    store_state.records.emplace(*key, value.value());
  }
  if (key->is_number && key->number + 1 > store_state.next_number) {
    store_state.next_number = key->number + 1;
  }
  base::Result<void> saved = SaveLocked();
  if (!saved) {
    return saved.error();
  }
  return SerializeKey(*key);
}

base::Result<std::string> IndexedDbStore::Add(std::string_view origin,
                                              std::string_view db,
                                              std::string_view store,
                                              std::optional<std::string> key_json,
                                              std::string value_json)
{
  return WriteRecord(origin,
                     db,
                     store,
                     std::move(key_json),
                     std::move(value_json),
                     /*add_only=*/true);
}

base::Result<std::string> IndexedDbStore::Put(std::string_view origin,
                                              std::string_view db,
                                              std::string_view store,
                                              std::optional<std::string> key_json,
                                              std::string value_json)
{
  return WriteRecord(origin,
                     db,
                     store,
                     std::move(key_json),
                     std::move(value_json),
                     /*add_only=*/false);
}

base::Result<std::optional<std::string>> IndexedDbStore::Get(std::string_view origin,
                                                             std::string_view db,
                                                             std::string_view store,
                                                             std::string key_json) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  const base::Result<Key> key = ParseKey(key_json);
  if (!key.has_value()) {
    return key.error();
  }
  const auto origin_it = databases_.find(std::string(origin));
  if (origin_it == databases_.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  const auto db_it = origin_it->second.find(std::string(db));
  if (db_it == origin_it->second.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  const auto store_it = db_it->second.stores.find(std::string(store));
  if (store_it == db_it->second.stores.end()) {
    return IdbError("NotFoundError", "object store does not exist");
  }
  const auto it = store_it->second.records.find(key.value());
  if (it == store_it->second.records.end()) {
    return std::optional<std::string>();
  }
  return std::optional<std::string>(SerializeJson(it->second));
}

base::Result<void> IndexedDbStore::Delete(std::string_view origin,
                                          std::string_view db,
                                          std::string_view store,
                                          std::string key_json)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const base::Result<Key> key = ParseKey(key_json);
  if (!key.has_value()) {
    return key.error();
  }
  const auto origin_it = databases_.find(std::string(origin));
  if (origin_it == databases_.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  const auto db_it = origin_it->second.find(std::string(db));
  if (db_it == origin_it->second.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  const auto store_it = db_it->second.stores.find(std::string(store));
  if (store_it == db_it->second.stores.end()) {
    return IdbError("NotFoundError", "object store does not exist");
  }
  store_it->second.records.erase(key.value());
  return SaveLocked();
}

base::Result<void>
IndexedDbStore::Clear(std::string_view origin, std::string_view db, std::string_view store)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto origin_it = databases_.find(std::string(origin));
  if (origin_it == databases_.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  const auto db_it = origin_it->second.find(std::string(db));
  if (db_it == origin_it->second.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  const auto store_it = db_it->second.stores.find(std::string(store));
  if (store_it == db_it->second.stores.end()) {
    return IdbError("NotFoundError", "object store does not exist");
  }
  store_it->second.records.clear();
  return SaveLocked();
}

base::Result<int64_t>
IndexedDbStore::Count(std::string_view origin, std::string_view db, std::string_view store) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto origin_it = databases_.find(std::string(origin));
  if (origin_it == databases_.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  const auto db_it = origin_it->second.find(std::string(db));
  if (db_it == origin_it->second.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  const auto store_it = db_it->second.stores.find(std::string(store));
  if (store_it == db_it->second.stores.end()) {
    return IdbError("NotFoundError", "object store does not exist");
  }
  return static_cast<int64_t>(store_it->second.records.size());
}

base::Result<std::vector<std::string>>
IndexedDbStore::GetAll(std::string_view origin, std::string_view db, std::string_view store) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto origin_it = databases_.find(std::string(origin));
  if (origin_it == databases_.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  const auto db_it = origin_it->second.find(std::string(db));
  if (db_it == origin_it->second.end()) {
    return IdbError("NotFoundError", "database does not exist");
  }
  const auto store_it = db_it->second.stores.find(std::string(store));
  if (store_it == db_it->second.stores.end()) {
    return IdbError("NotFoundError", "object store does not exist");
  }
  std::vector<std::string> values;
  values.reserve(store_it->second.records.size());
  for (const auto& [key, value] : store_it->second.records) {
    values.push_back(SerializeJson(value));
  }
  return values;
}

void IndexedDbStore::ClearAll()
{
  std::lock_guard<std::mutex> lock(mutex_);
  databases_.clear();
}

} // namespace neko::storage
