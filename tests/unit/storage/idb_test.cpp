// Unit tests for the IndexedDB storage core (JSON wire model + IndexedDbStore).
// These exercise the C++ layer behind window.indexedDB; the JS-facing tests
// live in tests/unit/javascript/dom_binding_test.cpp.

#include "neko/storage/indexed_db.h"
#include "neko/storage/json_value.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>

namespace neko::storage {
namespace {

// A throw-away profile directory, removed on destruction.
class TempProfile
{
public:
  TempProfile()
  {
    path_ = std::filesystem::temp_directory_path() /
            ("neko_idb_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter_++));
    std::filesystem::create_directories(path_);
  }
  ~TempProfile()
  {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::string& path() const
  {
    return path_;
  }

private:
  static int counter_;
  std::string path_;
};
int TempProfile::counter_ = 0;

TEST(JsonValueTest, ParsesAndSerializesRoundTrip)
{
  const std::string text =
      R"({"a":1,"b":[true,false,null,"x\n\"y"],"c":{"n":-2.5e3,"u":"\u4f60\u597d"}})";
  base::Result<JsonValue> parsed = ParseJson(text);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
  EXPECT_EQ(SerializeJson(parsed.value()),
            R"({"a":1,"b":[true,false,null,"x\n\"y"],"c":{"n":-2500,"u":"你好"}})");
}

TEST(JsonValueTest, ParsesScalars)
{
  auto num = ParseJson("42");
  ASSERT_TRUE(num.has_value());
  EXPECT_DOUBLE_EQ(std::get<double>(num.value().value), 42.0);
  auto neg = ParseJson("-0.5");
  ASSERT_TRUE(neg.has_value());
  EXPECT_DOUBLE_EQ(std::get<double>(neg.value().value), -0.5);
  auto str = ParseJson(R"("hi")");
  ASSERT_TRUE(str.has_value());
  EXPECT_EQ(std::get<std::string>(str.value().value), "hi");
  auto nul = ParseJson("null");
  ASSERT_TRUE(nul.has_value());
  EXPECT_TRUE(nul.value().IsNull());
}

TEST(JsonValueTest, RejectsMalformed)
{
  EXPECT_FALSE(ParseJson("").has_value());
  EXPECT_FALSE(ParseJson("{").has_value());
  EXPECT_FALSE(ParseJson("[1,]").has_value());
  EXPECT_FALSE(ParseJson("01").has_value());
  EXPECT_FALSE(ParseJson("1.2.3").has_value());
  EXPECT_FALSE(ParseJson(R"("unterminated)").has_value());
  EXPECT_FALSE(ParseJson("true false").has_value());
  EXPECT_FALSE(ParseJson("NaN").has_value());
  EXPECT_FALSE(ParseJson(R"("\ud800")").has_value()); // lone surrogate
}

// Creates a database and an object store through the public API.  Tolerates
// an already-existing database (version set only when needed).
void SetupStore(IndexedDbStore& store,
                const char* origin,
                const char* db,
                const char* store_name,
                const char* key_path,
                bool auto_increment)
{
  if (store.CurrentVersion(origin, db).value_or(0) == 0) {
    ASSERT_TRUE(store.CreateDatabase(origin, db).has_value());
    ASSERT_TRUE(store.SetVersion(origin, db, 1).has_value());
  }
  ASSERT_TRUE(
      store.CreateObjectStore(origin, db, store_name, key_path, auto_increment).has_value());
}

TEST(IndexedDbTest, CreateAndVersion)
{
  TempProfile tp;
  IndexedDbStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  EXPECT_EQ(store.CurrentVersion("https://a", "kv").value(), 0);
  EXPECT_EQ(store.CreateDatabase("https://a", "kv").value(), 0);
  EXPECT_EQ(store.CurrentVersion("https://a", "kv").value(), 0);
  EXPECT_FALSE(store.CreateDatabase("https://a", "kv").has_value());
  ASSERT_TRUE(store.SetVersion("https://a", "kv", 3).has_value());
  EXPECT_EQ(store.CurrentVersion("https://a", "kv").value(), 3);
  EXPECT_FALSE(store.SetVersion("https://a", "kv", 2).has_value()); // cannot decrease
  // A different origin sees nothing.
  EXPECT_EQ(store.CurrentVersion("https://b", "kv").value(), 0);
}

TEST(IndexedDbTest, AddGetPutOutOfLineKeys)
{
  TempProfile tp;
  IndexedDbStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  SetupStore(store, "https://a", "kv", "records", "", false);

  const base::Result<std::string> key =
      store.Add("https://a", "kv", "records", "\"id1\"", "{\"name\":\"one\"}");
  ASSERT_TRUE(key.has_value()) << key.error().message();
  EXPECT_EQ(key.value(), "\"id1\"");

  const auto got = store.Get("https://a", "kv", "records", "\"id1\"");
  ASSERT_TRUE(got.has_value()) << got.error().message();
  ASSERT_TRUE(got.value().has_value());
  EXPECT_EQ(*got.value(), "{\"name\":\"one\"}");

  // Add with the same key fails; Put upserts.
  EXPECT_FALSE(store.Add("https://a", "kv", "records", "\"id1\"", "{}").has_value());
  ASSERT_TRUE(store.Put("https://a", "kv", "records", "\"id1\"", "{\"name\":\"uno\"}").has_value());
  const auto updated = store.Get("https://a", "kv", "records", "\"id1\"");
  ASSERT_TRUE(updated.value().has_value());
  EXPECT_EQ(*updated.value(), "{\"name\":\"uno\"}");

  // Missing keys return nullopt, not an error.
  const auto missing = store.Get("https://a", "kv", "records", "\"nope\"");
  ASSERT_TRUE(missing.has_value());
  EXPECT_FALSE(missing.value().has_value());
}

TEST(IndexedDbTest, KeyPathExtraction)
{
  TempProfile tp;
  IndexedDbStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  SetupStore(store, "https://a", "kv", "records", "id", false);

  ASSERT_TRUE(
      store.Add("https://a", "kv", "records", std::nullopt, "{\"id\":\"k1\",\"v\":1}").has_value());
  const auto got = store.Get("https://a", "kv", "records", "\"k1\"");
  ASSERT_TRUE(got.value().has_value());
  EXPECT_EQ(*got.value(), "{\"id\":\"k1\",\"v\":1}");

  // A value without the key path fails.
  EXPECT_FALSE(store.Add("https://a", "kv", "records", std::nullopt, "{\"v\":2}").has_value());
  // An out-of-line key on a key-path store fails.
  EXPECT_FALSE(store.Add("https://a", "kv", "records", "\"x\"", "{\"id\":\"k2\"}").has_value());
}

TEST(IndexedDbTest, AutoIncrementGeneratesAndInjectsKeys)
{
  TempProfile tp;
  IndexedDbStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  SetupStore(store, "https://a", "kv", "items", "id", true);

  const base::Result<std::string> k1 =
      store.Add("https://a", "kv", "items", std::nullopt, "{\"label\":\"first\"}");
  ASSERT_TRUE(k1.has_value());
  EXPECT_EQ(k1.value(), "1");
  const base::Result<std::string> k2 =
      store.Add("https://a", "kv", "items", std::nullopt, "{\"label\":\"second\"}");
  EXPECT_EQ(k2.value(), "2");

  // The generated keys were injected into the values at the key path.
  const auto got = store.Get("https://a", "kv", "items", "1");
  ASSERT_TRUE(got.value().has_value());
  EXPECT_EQ(*got.value(), "{\"id\":1,\"label\":\"first\"}");

  // A numeric key bumps the generator (5 -> next is 6).
  ASSERT_TRUE(
      store.Add("https://a", "kv", "items", std::nullopt, "{\"id\":5,\"x\":0}").has_value());
  EXPECT_EQ(store.Add("https://a", "kv", "items", std::nullopt, "{\"label\":\"after\"}").value(),
            "6");
}

TEST(IndexedDbTest, GetAllSortedKeys)
{
  TempProfile tp;
  IndexedDbStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  SetupStore(store, "https://a", "kv", "records", "", false);

  ASSERT_TRUE(store.Add("https://a", "kv", "records", "\"s1\"", "\"string-one\"").has_value());
  ASSERT_TRUE(store.Add("https://a", "kv", "records", "2", "\"two\"").has_value());
  ASSERT_TRUE(store.Add("https://a", "kv", "records", "\"a\"", "\"string-a\"").has_value());
  ASSERT_TRUE(store.Add("https://a", "kv", "records", "1", "\"one\"").has_value());

  const base::Result<std::vector<std::string>> all = store.GetAll("https://a", "kv", "records");
  ASSERT_TRUE(all.has_value()) << all.error().message();
  // Numbers (ascending) before strings (ascending).
  ASSERT_EQ(all.value().size(), 4u);
  EXPECT_EQ(all.value()[0], "\"one\"");
  EXPECT_EQ(all.value()[1], "\"two\"");
  EXPECT_EQ(all.value()[2], "\"string-a\"");
  EXPECT_EQ(all.value()[3], "\"string-one\"");
  EXPECT_EQ(store.Count("https://a", "kv", "records").value(), 4);
}

TEST(IndexedDbTest, DeleteClearCount)
{
  TempProfile tp;
  IndexedDbStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  SetupStore(store, "https://a", "kv", "records", "", false);

  ASSERT_TRUE(store.Add("https://a", "kv", "records", "\"a\"", "1").has_value());
  ASSERT_TRUE(store.Add("https://a", "kv", "records", "\"b\"", "2").has_value());
  ASSERT_TRUE(store.Delete("https://a", "kv", "records", "\"a\"").has_value());
  EXPECT_EQ(store.Count("https://a", "kv", "records").value(), 1);
  ASSERT_TRUE(store.Clear("https://a", "kv", "records").has_value());
  EXPECT_EQ(store.Count("https://a", "kv", "records").value(), 0);
}

TEST(IndexedDbTest, PersistsAcrossReload)
{
  TempProfile tp;
  const std::string dir = tp.path();
  {
    IndexedDbStore store(dir);
    ASSERT_TRUE(store.Load().has_value());
    SetupStore(store, "https://a", "kv", "records", "", false);
    ASSERT_TRUE(
        store.Add("https://a", "kv", "records", "\"k\"", "{\"nested\":[1,2,3]}").has_value());
  }
  // A brand-new instance over the same profile sees the data.
  IndexedDbStore reloaded(dir);
  ASSERT_TRUE(reloaded.Load().has_value());
  EXPECT_EQ(reloaded.CurrentVersion("https://a", "kv").value(), 1);
  const auto stores = reloaded.ObjectStores("https://a", "kv");
  ASSERT_TRUE(stores.has_value());
  ASSERT_EQ(stores.value().size(), 1u);
  EXPECT_EQ(stores.value()[0].name, "records");
  const auto got = reloaded.Get("https://a", "kv", "records", "\"k\"");
  ASSERT_TRUE(got.value().has_value());
  EXPECT_EQ(*got.value(), "{\"nested\":[1,2,3]}");
}

TEST(IndexedDbTest, DeleteDatabaseAndStores)
{
  TempProfile tp;
  IndexedDbStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  SetupStore(store, "https://a", "kv", "records", "", false);
  SetupStore(store, "https://a", "kv", "other", "", false);
  ASSERT_TRUE(store.DeleteObjectStore("https://a", "kv", "other").has_value());
  EXPECT_EQ(store.ObjectStores("https://a", "kv").value().size(), 1u);
  ASSERT_TRUE(store.DeleteDatabase("https://a", "kv").has_value());
  EXPECT_EQ(store.CurrentVersion("https://a", "kv").value(), 0);
}

TEST(IndexedDbTest, ClearAllWipesEveryOrigin)
{
  TempProfile tp;
  IndexedDbStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  SetupStore(store, "https://a", "kv", "records", "", false);
  SetupStore(store, "https://b", "kv", "records", "", false);
  store.ClearAll();
  ASSERT_TRUE(store.Save().has_value());
  IndexedDbStore reloaded(tp.path());
  ASSERT_TRUE(reloaded.Load().has_value());
  EXPECT_EQ(reloaded.CurrentVersion("https://a", "kv").value(), 0);
  EXPECT_EQ(reloaded.CurrentVersion("https://b", "kv").value(), 0);
}

} // namespace
} // namespace neko::storage
