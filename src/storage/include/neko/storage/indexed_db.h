#pragma once

#include "neko/base/status.h"
#include "neko/storage/json_value.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neko::storage {

// ---------------------------------------------------------------------------
// A persistent IndexedDB-flavored store, partitioned by origin and database
// name.  This is the C++ core behind window.indexedDB; the JavaScript layer
// owns the object model (databases, transactions, requests) and drives this
// class through synchronous operations.
//
// Scope (documented subset):
//   * versioned databases (version 0 = "created, not yet upgraded"),
//   * object stores with an optional key path (top-level property only) and
//     auto-increment key generation,
//   * records with structured-clone-like values (JSON subset: null, bool,
//     number, string, array, object — no Date/BinaryData/cycles),
//   * keys are numbers or strings (numbers sort before strings, per spec),
//   * add (duplicate -> ConstraintError), put (upsert), get, delete, clear,
//     count, getAll (key order).
//
// Persistence: one line-oriented file (indexed_db.txt) with percent-encoded
// tab-separated fields and atomic writes, following the other stores.
// Every mutation persists immediately (small files; a page typically stores
// far less than a MiB).
//
// Errors carry the IndexedDB exception name in a "IDB:<name>:" message
// prefix so the JS binding can surface it as a DOMException-like Error.
//
// Threading: internally synchronized, matching the other stores.
class IndexedDbStore
{
public:
  explicit IndexedDbStore(std::string profile_dir);
  ~IndexedDbStore() = default;

  IndexedDbStore(const IndexedDbStore&) = delete;
  IndexedDbStore& operator=(const IndexedDbStore&) = delete;

  // Loads the store file if present; a missing file is an empty store.
  base::Result<void> Load();

  // Persists everything atomically.
  base::Result<void> Save() const;

  // -------------------------------------------------------------------------
  // Databases
  // -------------------------------------------------------------------------

  // Current version of |db| for |origin|, or 0 when it does not exist.
  base::Result<int64_t> CurrentVersion(std::string_view origin, std::string_view db) const;

  // Creates |db| (version 0) for |origin|.  Error when it already exists.
  base::Result<int64_t> CreateDatabase(std::string_view origin, std::string_view db);

  // Sets the version (the upgrade flow finishes with this call).
  base::Result<void> SetVersion(std::string_view origin, std::string_view db, int64_t version);

  base::Result<void> DeleteDatabase(std::string_view origin, std::string_view db);

  // An object store's key configuration (for the JS layer's metadata).
  struct ObjectStoreMeta
  {
    std::string name;
    std::string key_path; // empty = out-of-line keys
    bool auto_increment = false;
  };

  // All object stores of |db|, in creation order, with key metadata.
  base::Result<std::vector<ObjectStoreMeta>> ObjectStores(std::string_view origin,
                                                          std::string_view db) const;

  // -------------------------------------------------------------------------
  // Object stores (version-change operations; the JS layer only calls these
  // while a version-change transaction is active)
  // -------------------------------------------------------------------------

  base::Result<void> CreateObjectStore(std::string_view origin,
                                       std::string_view db,
                                       std::string_view store,
                                       std::string_view key_path,
                                       bool auto_increment);

  base::Result<void>
  DeleteObjectStore(std::string_view origin, std::string_view db, std::string_view store);

  // -------------------------------------------------------------------------
  // Data.  Keys and values travel as JSON text; |key| is nullopt for
  // out-of-line-key stores (the key then comes from the key path or the
  // auto-increment generator).  Add/Put return the record's key as JSON.
  // -------------------------------------------------------------------------

  base::Result<std::string> Add(std::string_view origin,
                                std::string_view db,
                                std::string_view store,
                                std::optional<std::string> key_json,
                                std::string value_json);

  base::Result<std::string> Put(std::string_view origin,
                                std::string_view db,
                                std::string_view store,
                                std::optional<std::string> key_json,
                                std::string value_json);

  base::Result<std::optional<std::string>> Get(std::string_view origin,
                                               std::string_view db,
                                               std::string_view store,
                                               std::string key_json) const;

  base::Result<void> Delete(std::string_view origin,
                            std::string_view db,
                            std::string_view store,
                            std::string key_json);

  base::Result<void> Clear(std::string_view origin, std::string_view db, std::string_view store);

  base::Result<int64_t>
  Count(std::string_view origin, std::string_view db, std::string_view store) const;

  // All record values of |store| in key order (JSON text each).
  base::Result<std::vector<std::string>>
  GetAll(std::string_view origin, std::string_view db, std::string_view store) const;

  // Removes every database for every origin (storage clearing).
  void ClearAll();

  const std::string& profile_dir() const
  {
    return profile_dir_;
  }

private:
  // A record key: a number or a string.
  struct Key
  {
    bool is_number = false;
    double number = 0;
    std::string string;

    bool operator<(const Key& other) const;
  };

  struct ObjectStore
  {
    std::string name;
    std::string key_path; // "" = out-of-line keys
    bool auto_increment = false;
    std::map<Key, JsonValue> records;
    double next_number = 1; // key generator: max numeric key + 1
  };

  struct Database
  {
    std::string name;
    int64_t version = 0;
    std::map<std::string, ObjectStore> stores;
  };

  // Parses a key from its JSON text ("123", "4.5", "\"foo\"").
  static base::Result<Key> ParseKey(std::string_view json);
  static std::string SerializeKey(const Key& key);

  // Extracts the key-path property from |value| (must be a number or string
  // member of a JSON object).
  static base::Result<std::optional<Key>> KeyFromValue(const ObjectStore& store,
                                                       const JsonValue& value);

  // Injects |key| into |value| at the key path (auto-increment only).
  static void InjectKey(const ObjectStore& store, const Key& key, JsonValue& value);

  // Errors carrying the IndexedDB exception name ("IDB:<Name>:<message>").
  static base::Error IdbError(std::string_view name, std::string message);

  // Serializes and atomically writes the store file.  Caller holds mutex_.
  base::Result<void> SaveLocked() const;

  base::Result<std::string> WriteRecord(std::string_view origin,
                                        std::string_view db,
                                        std::string_view store,
                                        std::optional<std::string> key_json,
                                        std::string value_json,
                                        bool add_only);

  std::string profile_dir_;
  std::string file_path_;
  mutable std::mutex mutex_;
  // origin -> (db name -> database)
  std::map<std::string, std::map<std::string, Database>> databases_;
};

} // namespace neko::storage
