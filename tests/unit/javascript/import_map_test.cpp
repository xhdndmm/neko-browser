// Unit tests for neko::javascript import maps (HTML §4.12.11).

#include "neko/javascript/import_map.h"

#include <gtest/gtest.h>
#include <string>

namespace neko::javascript {
namespace {

TEST(ImportMapTest, ParsesImports)
{
  auto map = ParseImportMap(R"({"imports": {"react": "./vendor/react.js"}})");
  ASSERT_TRUE(map.has_value()) << map.error().message();
  ASSERT_EQ(map.value().imports.size(), 1u);
  EXPECT_EQ(map.value().imports[0].first, "react");
  EXPECT_EQ(map.value().imports[0].second, "./vendor/react.js");
  EXPECT_TRUE(map.value().scopes.empty());
}

TEST(ImportMapTest, ParsesScopes)
{
  auto map = ParseImportMap(
      R"({"imports": {"a": "/a.js"}, "scopes": {"/admin/": {"a": "/admin-a.js"}}})");
  ASSERT_TRUE(map.has_value()) << map.error().message();
  ASSERT_EQ(map.value().imports.size(), 1u);
  ASSERT_EQ(map.value().scopes.size(), 1u);
  EXPECT_EQ(map.value().scopes[0].first, "/admin/");
  ASSERT_EQ(map.value().scopes[0].second.size(), 1u);
  EXPECT_EQ(map.value().scopes[0].second[0].second, "/admin-a.js");
}

TEST(ImportMapTest, RejectsInvalidJson)
{
  auto map = ParseImportMap("{not json");
  ASSERT_FALSE(map.has_value());
  EXPECT_EQ(map.error().category(), base::ErrorCategory::kParse);
}

TEST(ImportMapTest, RejectsNonObjectRootAndNonStringValues)
{
  auto root = ParseImportMap("[]");
  ASSERT_FALSE(root.has_value());
  auto values = ParseImportMap(R"({"imports": {"a": 42}})");
  ASSERT_FALSE(values.has_value());
}

TEST(ImportMapTest, RejectsPrefixKeyWithoutSlashValue)
{
  auto map = ParseImportMap(R"({"imports": {"./lib/": "./other.js"}})");
  ASSERT_FALSE(map.has_value());
}

TEST(ImportMapTest, ExactBareSpecifierMapping)
{
  auto map = ParseImportMap(R"({"imports": {"react": "./vendor/react.js"}})");
  ASSERT_TRUE(map.has_value());
  auto resolved =
      ResolveImportMap(map.value(), "http://test/app/main.js", "http://test/index.html", "react");
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved.value(), "http://test/vendor/react.js");
}

TEST(ImportMapTest, PrefixMappingAppendsRemainder)
{
  auto map = ParseImportMap(R"({"imports": {"./lib/": "./vendor/lib/"}})");
  ASSERT_TRUE(map.has_value());
  auto resolved = ResolveImportMap(
      map.value(), "http://test/app/main.js", "http://test/index.html", "./lib/util.js");
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved.value(), "http://test/vendor/lib/util.js");
}

TEST(ImportMapTest, LongestScopeWinsAndFallsBackToImports)
{
  // Scope keys resolve against the document base (http://test/index.html):
  // "/app/" -> "http://test/app/", "/app/admin/" -> "http://test/app/admin/".
  auto map = ParseImportMap(
      R"({"imports": {"dep": "./shared/dep.js"},)"
      R"("scopes": {"/app/admin/": {"dep": "./admin/dep.js"}, "/app/": {"x": "/x.js"}}})");
  ASSERT_TRUE(map.has_value());
  // Inside the admin scope: the admin mapping applies.
  auto admin = ResolveImportMap(
      map.value(), "http://test/app/admin/panel.js", "http://test/index.html", "dep");
  ASSERT_TRUE(admin.has_value());
  EXPECT_EQ(admin.value(), "http://test/admin/dep.js");
  // Inside /app/ but outside admin: no "dep" entry in that scope, so the
  // top-level imports apply.
  const ImportMap& m = map.value();
  std::optional<std::string> app = ResolveImportMap(m, "http://test/app/user.js", "http://test/index.html", "dep");
  ASSERT_TRUE(app.has_value());
  EXPECT_EQ(app.value(), "http://test/shared/dep.js");
  // Outside every scope: top-level only.
  auto outside = ResolveImportMap(m, "http://other.site/x.js", "http://test/index.html", "dep");
  ASSERT_TRUE(outside.has_value());
  EXPECT_EQ(outside.value(), "http://test/shared/dep.js");
}

TEST(ImportMapTest, UnmappedSpecifierReturnsNullopt)
{
  auto map = ParseImportMap(R"({"imports": {"react": "./vendor/react.js"}})");
  ASSERT_TRUE(map.has_value());
  auto resolved =
      ResolveImportMap(map.value(), "http://test/main.js", "http://test/index.html", "vue");
  EXPECT_FALSE(resolved.has_value());
}

} // namespace
} // namespace neko::javascript
