// Unit tests for neko::storage (field codec + cookie / history / bookmark
// stores).  All stores round-trip through their on-disk format.

#include "neko/base/status.h"
#include "neko/storage/bookmark_store.h"
#include "neko/storage/cookie_store.h"
#include "neko/storage/field_codec.h"
#include "neko/storage/file_util.h"
#include "neko/storage/history_store.h"
#include "neko/storage/local_storage.h"
#include "neko/url/url.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace neko::storage {
namespace {

using testing::Eq;

// ---------------------------------------------------------------------------
// Temp profile fixture
// ---------------------------------------------------------------------------

class TempProfile
{
public:
  TempProfile()
  {
    dir_ = std::filesystem::temp_directory_path() /
           ("neko-storage-test-" + std::to_string(::getpid()) + "-" + std::to_string(++seq_));
    std::filesystem::create_directories(dir_);
  }
  ~TempProfile()
  {
    std::filesystem::remove_all(dir_);
  }

  const std::string path() const
  {
    return dir_.string();
  }

private:
  static int seq_;
  std::filesystem::path dir_;
};

int TempProfile::seq_ = 0;

url::Url MakeUrl(const std::string& s)
{
  auto u = url::Url::Parse(s);
  EXPECT_TRUE(u.has_value()) << s;
  return std::move(u).value();
}

// ---------------------------------------------------------------------------
// FieldCodec
// ---------------------------------------------------------------------------

TEST(FieldCodecTest, RoundTripsPlainText)
{
  EXPECT_THAT(EncodeField("hello world"), Eq("hello%20world"));
  auto decoded = DecodeField("hello%20world");
  ASSERT_TRUE(decoded.has_value());
  EXPECT_THAT(decoded.value(), Eq("hello world"));
}

TEST(FieldCodecTest, RoundTripsHostileBytes)
{
  const std::string input = "a\tb\nc\rd%20e\x01f g=h&i?j";
  EXPECT_THAT(DecodeField(EncodeField(input)).value(), Eq(input));
}

TEST(FieldCodecTest, UnreservedCharactersAreKept)
{
  EXPECT_THAT(EncodeField("ABCabc012._~-"), Eq("ABCabc012._~-"));
}

TEST(FieldCodecTest, RejectsMalformedEscapes)
{
  EXPECT_FALSE(DecodeField("abc%").has_value());
  EXPECT_FALSE(DecodeField("abc%2").has_value());
  EXPECT_FALSE(DecodeField("abc%zz").has_value());
}

// ---------------------------------------------------------------------------
// CookieStore
// ---------------------------------------------------------------------------

TEST(CookieStoreTest, ParsesSetCookieAttributes)
{
  TempProfile tp;
  CookieStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());

  const int64_t now = 1'700'000'000;
  const auto url = MakeUrl("http://example.com/path/page.html");
  ASSERT_TRUE(
      store.SetCookieFromHeader(url,
                                "sid=abc123; Path=/path; Expires=Sun, 06 Nov 2033 08:49:37 GMT; "
                                "Secure; HttpOnly; SameSite=Lax",
                                now));

  ASSERT_EQ(store.size(), 1u);
  const auto all = store.All();
  const Cookie& c = all[0];
  EXPECT_THAT(c.name, Eq("sid"));
  EXPECT_THAT(c.value, Eq("abc123"));
  EXPECT_THAT(c.domain, Eq("example.com"));
  EXPECT_TRUE(c.host_only);
  EXPECT_THAT(c.path, Eq("/path"));
  EXPECT_TRUE(c.secure);
  EXPECT_TRUE(c.http_only);
  EXPECT_THAT(c.same_site, Eq("lax"));
  EXPECT_GT(c.expiry, now);
}

TEST(CookieStoreTest, DomainAttributeSetsSharedDomain)
{
  TempProfile tp;
  CookieStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  const int64_t now = 1'700'000'000;

  ASSERT_TRUE(store.SetCookieFromHeader(
      MakeUrl("http://www.example.com/"), "theme=dark; Domain=example.com", now));
  ASSERT_EQ(store.size(), 1u);
  EXPECT_THAT(store.All()[0].domain, Eq("example.com"));
  EXPECT_FALSE(store.All()[0].host_only);

  // Leading dot is ignored.
  ASSERT_TRUE(store.SetCookieFromHeader(
      MakeUrl("http://www.example.com/"), "a=1; Domain=.example.com", now));
  EXPECT_THAT(store.All()[1].domain, Eq("example.com"));
}

TEST(CookieStoreTest, RejectsForeignDomainAttribute)
{
  TempProfile tp;
  CookieStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  const int64_t now = 1'700'000'000;

  EXPECT_FALSE(store.SetCookieFromHeader(
      MakeUrl("http://www.example.com/"), "evil=1; Domain=attacker.com", now));
  EXPECT_TRUE(store.empty());
}

TEST(CookieStoreTest, RejectsNameWithSeparators)
{
  TempProfile tp;
  CookieStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  const int64_t now = 1'700'000'000;

  EXPECT_FALSE(store.SetCookieFromHeader(MakeUrl("http://example.com/"), "bad name=x", now));
  EXPECT_FALSE(store.SetCookieFromHeader(MakeUrl("http://example.com/"), "=novalue", now));
  EXPECT_TRUE(store.empty());
}

TEST(CookieStoreTest, DefaultPathIsDirectoryOfRequest)
{
  TempProfile tp;
  CookieStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  const int64_t now = 1'700'000'000;

  ASSERT_TRUE(store.SetCookieFromHeader(MakeUrl("http://example.com/a/b/c.html"), "x=1", now));
  EXPECT_THAT(store.All()[0].path, Eq("/a/b"));

  ASSERT_TRUE(store.SetCookieFromHeader(MakeUrl("http://example.com/"), "y=2", now));
  EXPECT_THAT(store.All()[1].path, Eq("/"));
}

TEST(CookieStoreTest, MaxAgeOverridesExpires)
{
  TempProfile tp;
  CookieStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  const int64_t now = 1'700'000'000;

  ASSERT_TRUE(
      store.SetCookieFromHeader(MakeUrl("http://example.com/"),
                                "short=1; Expires=Sun, 06 Nov 2033 08:49:37 GMT; Max-Age=60",
                                now));
  EXPECT_THAT(store.All()[0].expiry, Eq(now + 60));
}

TEST(CookieStoreTest, MaxAgeZeroDeletesExistingCookie)
{
  TempProfile tp;
  CookieStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  const int64_t now = 1'700'000'000;

  ASSERT_TRUE(store.SetCookieFromHeader(MakeUrl("http://example.com/"), "sid=abc", now));
  ASSERT_EQ(store.size(), 1u);
  ASSERT_TRUE(store.SetCookieFromHeader(MakeUrl("http://example.com/"), "sid=; Max-Age=0", now));
  EXPECT_TRUE(store.empty());
}

TEST(CookieStoreTest, ExpiredCookieIsNotStored)
{
  TempProfile tp;
  CookieStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  const int64_t now = 1'700'000'000;

  ASSERT_TRUE(store.SetCookieFromHeader(
      MakeUrl("http://example.com/"), "old=1; Expires=Wed, 01 Jan 1990 00:00:00 GMT", now));
  EXPECT_TRUE(store.empty());
}

TEST(CookieStoreTest, DomainMatchingSelectsSubdomains)
{
  TempProfile tp;
  CookieStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  const int64_t now = 1'700'000'000;

  ASSERT_TRUE(store.SetCookieFromHeader(
      MakeUrl("http://example.com/"), "shared=1; Domain=example.com", now));
  ASSERT_TRUE(store.SetCookieFromHeader(MakeUrl("http://www.example.com/"), "hostonly=1", now));

  // Domain cookie is sent to a subdomain; host-only cookie is not.
  const auto sub = MakeUrl("http://sub.example.com/page");
  const auto host_header = store.CookieHeaderFor(sub, now);
  EXPECT_THAT(host_header, Eq("shared=1"));

  // Both are sent to the exact host.
  const auto exact = MakeUrl("http://www.example.com/page");
  EXPECT_THAT(store.CookieHeaderFor(exact, now), testing::HasSubstr("shared=1"));
  EXPECT_THAT(store.CookieHeaderFor(exact, now), testing::HasSubstr("hostonly=1"));
}

TEST(CookieStoreTest, PathMatching)
{
  TempProfile tp;
  CookieStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  const int64_t now = 1'700'000'000;

  ASSERT_TRUE(store.SetCookieFromHeader(MakeUrl("http://example.com/a/b"), "p=1; Path=/a", now));
  EXPECT_THAT(store.CookieHeaderFor(MakeUrl("http://example.com/a/b/c"), now), Eq("p=1"));
  // /ab does not match Path=/a (segment boundary required).
  EXPECT_THAT(store.CookieHeaderFor(MakeUrl("http://example.com/ab"), now), Eq(""));
  EXPECT_THAT(store.CookieHeaderFor(MakeUrl("http://example.com/"), now), Eq(""));
}

TEST(CookieStoreTest, SecureCookiesNotSentOverHttp)
{
  TempProfile tp;
  CookieStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  const int64_t now = 1'700'000'000;

  ASSERT_TRUE(store.SetCookieFromHeader(MakeUrl("http://example.com/"), "s=1; Secure", now));
  EXPECT_THAT(store.CookieHeaderFor(MakeUrl("http://example.com/"), now), Eq(""));
}

TEST(CookieStoreTest, ExpiredCookiesAreNotSent)
{
  TempProfile tp;
  CookieStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  const int64_t now = 1'700'000'000;

  ASSERT_TRUE(store.SetCookieFromHeader(MakeUrl("http://example.com/"), "old=1; Max-Age=5", now));
  EXPECT_THAT(store.CookieHeaderFor(MakeUrl("http://example.com/"), now), Eq("old=1"));
  EXPECT_THAT(store.CookieHeaderFor(MakeUrl("http://example.com/"), now + 60), Eq(""));
  store.PurgeExpired(now + 60);
  EXPECT_TRUE(store.empty());
}

TEST(CookieStoreTest, SameNameReplaces)
{
  TempProfile tp;
  CookieStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  const int64_t now = 1'700'000'000;

  ASSERT_TRUE(store.SetCookieFromHeader(MakeUrl("http://example.com/"), "sid=old", now));
  ASSERT_TRUE(store.SetCookieFromHeader(MakeUrl("http://example.com/"), "sid=new", now));
  ASSERT_EQ(store.size(), 1u);
  EXPECT_THAT(store.All()[0].value, Eq("new"));
}

TEST(CookieStoreTest, DeleteCookie)
{
  TempProfile tp;
  CookieStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  const int64_t now = 1'700'000'000;

  ASSERT_TRUE(store.SetCookieFromHeader(MakeUrl("http://example.com/"), "a=1", now));
  ASSERT_TRUE(store.SetCookieFromHeader(MakeUrl("http://example.com/"), "b=2", now));
  EXPECT_TRUE(store.DeleteCookie("a", "example.com", "/"));
  EXPECT_EQ(store.size(), 1u);
  EXPECT_FALSE(store.DeleteCookie("a", "example.com", "/"));
}

TEST(CookieStoreTest, SaveLoadRoundTrip)
{
  TempProfile tp;
  {
    CookieStore store(tp.path());
    ASSERT_TRUE(store.Load().has_value());
    const int64_t now = 1'700'000'000;
    ASSERT_TRUE(store.SetCookieFromHeader(
        MakeUrl("http://example.com/"), "name=value%20with%20spaces; Path=/x; Max-Age=3600", now));
    ASSERT_TRUE(store.SetCookieFromHeader(MakeUrl("http://example.com/"), "session=only", now));
    ASSERT_TRUE(store.Save().has_value());
  }
  {
    CookieStore store(tp.path());
    ASSERT_TRUE(store.Load().has_value());
    ASSERT_EQ(store.size(), 1u); // session cookie is not persisted
    EXPECT_THAT(store.All()[0].name, Eq("name"));
    // Cookie values are opaque: percent sequences are kept verbatim.
    EXPECT_THAT(store.All()[0].value, Eq("value%20with%20spaces"));
    EXPECT_THAT(store.All()[0].path, Eq("/x"));
  }
}

TEST(CookieStoreTest, MissingFileIsEmptyProfile)
{
  TempProfile tp;
  CookieStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  EXPECT_TRUE(store.empty());
}

// ---------------------------------------------------------------------------
// HistoryStore
// ---------------------------------------------------------------------------

TEST(HistoryStoreTest, RecordsAndDeduplicatesVisits)
{
  TempProfile tp;
  HistoryStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());

  store.RecordVisit("http://example.com/", "Example", 100);
  store.RecordVisit("http://example.com/about", "About", 200);
  store.RecordVisit("http://example.com/", "Example", 300);

  EXPECT_EQ(store.size(), 2u);
  const auto all = store.All();
  ASSERT_EQ(all.size(), 2u);
  EXPECT_THAT(all[0].url, Eq("http://example.com/")); // most recent first
  EXPECT_EQ(all[0].visit_count, 2);
  EXPECT_EQ(all[0].last_visit, 300);
}

TEST(HistoryStoreTest, SearchMatchesUrlAndTitle)
{
  TempProfile tp;
  HistoryStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  store.RecordVisit("http://example.com/", "Neko Home", 100);
  store.RecordVisit("http://news.example.com/", "Daily News", 200);

  EXPECT_EQ(store.Search("neko").size(), 1u);
  EXPECT_EQ(store.Search("news").size(), 1u);
  EXPECT_EQ(store.Search("EXAMPLE").size(), 2u);
  EXPECT_TRUE(store.Search("zzz").empty());
}

TEST(HistoryStoreTest, RemoveAndClear)
{
  TempProfile tp;
  HistoryStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  store.RecordVisit("http://example.com/", "x", 100);
  EXPECT_TRUE(store.Remove("http://example.com/"));
  EXPECT_TRUE(store.empty());
  store.RecordVisit("http://example.com/", "x", 100);
  store.Clear();
  EXPECT_TRUE(store.empty());
}

TEST(HistoryStoreTest, SaveLoadRoundTrip)
{
  TempProfile tp;
  {
    HistoryStore store(tp.path());
    ASSERT_TRUE(store.Load().has_value());
    store.RecordVisit("http://example.com/", "Title with\ttabs and\nnewlines", 42);
    store.RecordVisit("http://example.com/a", "A", 7);
    ASSERT_TRUE(store.Save().has_value());
  }
  {
    HistoryStore store(tp.path());
    ASSERT_TRUE(store.Load().has_value());
    ASSERT_EQ(store.size(), 2u);
    EXPECT_THAT(store.All()[0].title, Eq("Title with\ttabs and\nnewlines"));
    EXPECT_EQ(store.All()[0].last_visit, 42); // most recent first
    EXPECT_THAT(store.All()[1].title, Eq("A"));
    EXPECT_EQ(store.All()[1].last_visit, 7);
  }
}

// ---------------------------------------------------------------------------
// BookmarkStore
// ---------------------------------------------------------------------------

TEST(BookmarkStoreTest, AddRemoveUpdateList)
{
  TempProfile tp;
  BookmarkStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());

  auto id1 = store.Add("http://example.com/", "Example", "", 100);
  ASSERT_TRUE(id1.has_value());
  auto id2 = store.Add("http://example.com/dev", "Dev", "Work", 200);
  ASSERT_TRUE(id2.has_value());

  ASSERT_EQ(store.size(), 2u);
  EXPECT_EQ(store.InFolder("Work").size(), 1u);
  EXPECT_EQ(store.InFolder("").size(), 1u);

  EXPECT_TRUE(store.UpdateTitle(id1.value(), "Renamed"));
  EXPECT_THAT(store.All()[0].title, Eq("Renamed"));

  EXPECT_TRUE(store.Remove(id1.value()));
  EXPECT_EQ(store.size(), 1u);
  EXPECT_FALSE(store.Remove(id1.value()));
}

TEST(BookmarkStoreTest, RejectsEmptyUrl)
{
  TempProfile tp;
  BookmarkStore store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  auto r = store.Add("", "t", "", 0);
  EXPECT_FALSE(r.has_value());
}

TEST(BookmarkStoreTest, SaveLoadRoundTrip)
{
  TempProfile tp;
  {
    BookmarkStore store(tp.path());
    ASSERT_TRUE(store.Load().has_value());
    auto id = store.Add("http://example.com/", "a title\twith tab", "Folder", 123);
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(store.Save().has_value());
  }
  {
    BookmarkStore store(tp.path());
    ASSERT_TRUE(store.Load().has_value());
    ASSERT_EQ(store.size(), 1u);
    EXPECT_THAT(store.All()[0].title, Eq("a title\twith tab"));
    EXPECT_THAT(store.All()[0].folder, Eq("Folder"));
    EXPECT_EQ(store.All()[0].created, 123);
  }
}

// ---------------------------------------------------------------------------
// LocalStorage
// ---------------------------------------------------------------------------

TEST(LocalStorageTest, SetGetRemove)
{
  TempProfile tp;
  LocalStorage store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  EXPECT_TRUE(store.empty());

  store.SetItem("https://example.com/", "theme", "dark");
  store.SetItem("https://example.com/", "lang", "zh-CN");
  ASSERT_EQ(store.size(), 2u);

  EXPECT_THAT(store.GetItem("https://example.com/", "theme").value(), Eq("dark"));
  EXPECT_THAT(store.GetItem("https://example.com/", "lang").value(), Eq("zh-CN"));
  EXPECT_FALSE(store.GetItem("https://example.com/", "missing").has_value());

  // Empty-string values are stored and retrievable.
  store.SetItem("https://example.com/", "empty", "");
  EXPECT_THAT(store.GetItem("https://example.com/", "empty").value(), Eq(""));

  EXPECT_TRUE(store.RemoveItem("https://example.com/", "lang"));
  EXPECT_FALSE(store.GetItem("https://example.com/", "lang").has_value());
  EXPECT_FALSE(store.RemoveItem("https://example.com/", "lang"));
  EXPECT_EQ(store.size(), 2u);
}

TEST(LocalStorageTest, SetReplacesValue)
{
  TempProfile tp;
  LocalStorage store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  store.SetItem("https://example.com/", "key", "one");
  store.SetItem("https://example.com/", "key", "two");
  ASSERT_EQ(store.size(), 1u);
  EXPECT_THAT(store.GetItem("https://example.com/", "key").value(), Eq("two"));
}

TEST(LocalStorageTest, OriginsAreIsolated)
{
  TempProfile tp;
  LocalStorage store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  store.SetItem("https://a.example/", "k", "va");
  store.SetItem("https://b.example/", "k", "vb");
  EXPECT_THAT(store.GetItem("https://a.example/", "k").value(), Eq("va"));
  EXPECT_THAT(store.GetItem("https://b.example/", "k").value(), Eq("vb"));
  EXPECT_EQ(store.size(), 2u);

  store.Clear("https://a.example/");
  EXPECT_FALSE(store.GetItem("https://a.example/", "k").has_value());
  EXPECT_THAT(store.GetItem("https://b.example/", "k").value(), Eq("vb"));
  EXPECT_EQ(store.size(), 1u);
}

TEST(LocalStorageTest, AllReturnsInsertionOrder)
{
  TempProfile tp;
  LocalStorage store(tp.path());
  ASSERT_TRUE(store.Load().has_value());
  store.SetItem("https://example.com/", "b", "2");
  store.SetItem("https://example.com/", "a", "1");
  store.SetItem("https://other.example/", "c", "3");

  const auto pairs = store.All("https://example.com/");
  ASSERT_EQ(pairs.size(), 2u);
  EXPECT_THAT(pairs[0].first, Eq("b"));
  EXPECT_THAT(pairs[1].first, Eq("a"));
  EXPECT_EQ(store.All("https://other.example/").size(), 1u);
  EXPECT_TRUE(store.All("https://none.example/").empty());
}

TEST(LocalStorageTest, SaveLoadRoundTrip)
{
  TempProfile tp;
  {
    LocalStorage store(tp.path());
    ASSERT_TRUE(store.Load().has_value());
    store.SetItem("https://example.com/", "theme", "dark");
    // Values may contain tabs, newlines and other hostile bytes.
    store.SetItem("https://example.com/", "note", "a\tb\nc\x01d");
    ASSERT_TRUE(store.Save().has_value());
  }
  {
    LocalStorage store(tp.path());
    ASSERT_TRUE(store.Load().has_value());
    ASSERT_EQ(store.size(), 2u);
    EXPECT_THAT(store.GetItem("https://example.com/", "theme").value(), Eq("dark"));
    EXPECT_THAT(store.GetItem("https://example.com/", "note").value(), Eq("a\tb\nc\x01d"));
  }
}

TEST(LocalStorageTest, MissingFileIsEmptyStore)
{
  TempProfile tp;
  LocalStorage store(tp.path());
  EXPECT_TRUE(store.Load().has_value());
  EXPECT_TRUE(store.empty());
}

// ---------------------------------------------------------------------------
// FileUtil
// ---------------------------------------------------------------------------

TEST(FileUtilTest, AtomicWriteCreatesParents)
{
  TempProfile tp;
  const std::string path = tp.path() + "/deep/nested/store.txt";
  ASSERT_TRUE(WriteFileAtomic(path, "hello").has_value());
  auto data = ReadFile(path);
  ASSERT_TRUE(data.has_value());
  EXPECT_THAT(data.value(), Eq("hello"));
}

TEST(FileUtilTest, WriteReplacesExistingContent)
{
  TempProfile tp;
  const std::string path = tp.path() + "/f.txt";
  ASSERT_TRUE(WriteFileAtomic(path, "one").has_value());
  ASSERT_TRUE(WriteFileAtomic(path, "two").has_value());
  EXPECT_THAT(ReadFile(path).value(), Eq("two"));
}

} // namespace
} // namespace neko::storage
