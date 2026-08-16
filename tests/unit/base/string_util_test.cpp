#include "neko/base/string_util.h"

#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

namespace neko::base {
namespace {

TEST(StringUtilTest, AsciiEqualsIgnoreCase)
{
  EXPECT_TRUE(AsciiEqualsIgnoreCase("abc", "ABC"));
  EXPECT_TRUE(AsciiEqualsIgnoreCase("AbC", "aBc"));
  EXPECT_TRUE(AsciiEqualsIgnoreCase("", ""));
  EXPECT_FALSE(AsciiEqualsIgnoreCase("abc", "abd"));
  EXPECT_FALSE(AsciiEqualsIgnoreCase("abc", "abcd"));
  // Non-ASCII is not folded (deliberate ASCII scope).
  EXPECT_FALSE(AsciiEqualsIgnoreCase("\xC3\xA9", "\xC3\x89"));
}

TEST(StringUtilTest, AsciiStartsWith)
{
  EXPECT_TRUE(AsciiStartsWith("hello world", "hello"));
  EXPECT_TRUE(AsciiStartsWith("hello", "hello"));
  EXPECT_FALSE(AsciiStartsWith("hello", "hello world"));
  // The empty string is a prefix of every string.
  EXPECT_TRUE(AsciiStartsWith("hello", ""));
  EXPECT_TRUE(AsciiStartsWith("", ""));
}

TEST(StringUtilTest, AsciiEndsWith)
{
  EXPECT_TRUE(AsciiEndsWith("hello world", "world"));
  EXPECT_TRUE(AsciiEndsWith("hello", "hello"));
  EXPECT_FALSE(AsciiEndsWith("world", "hello"));
  EXPECT_TRUE(AsciiEndsWith("hello", ""));
}

TEST(StringUtilTest, Contains)
{
  EXPECT_TRUE(Contains("a quick brown fox", "quick"));
  EXPECT_TRUE(Contains("abc", "a"));
  EXPECT_TRUE(Contains("abc", ""));
  EXPECT_FALSE(Contains("abc", "z"));
}

TEST(StringUtilTest, Trim)
{
  EXPECT_EQ(Trim("  hello  "), "hello");
  EXPECT_EQ(Trim("\t\r\n spaced \n\t"), "spaced");
  EXPECT_EQ(Trim(""), "");
  EXPECT_EQ(Trim("   "), "");
  EXPECT_EQ(Trim("untouched"), "untouched");
}

TEST(StringUtilTest, TrimLeftRight)
{
  EXPECT_EQ(TrimLeft("  abc"), "abc");
  EXPECT_EQ(TrimRight("abc  "), "abc");
  EXPECT_EQ(TrimLeft("abc"), "abc");
  EXPECT_EQ(TrimRight("abc"), "abc");
}

TEST(StringUtilTest, ToLowerUpper)
{
  EXPECT_EQ(ToLower("HeLLo World"), "hello world");
  EXPECT_EQ(ToUpper("HeLLo World"), "HELLO WORLD");
  EXPECT_EQ(ToLower(""), "");
  // ASCII-only: non-ASCII bytes pass through untouched.
  EXPECT_EQ(ToLower("\xC3\x84"), "\xC3\x84");
}

TEST(StringUtilTest, SplitByChar)
{
  const auto parts = Split("a,b,c", ',');
  ASSERT_EQ(parts.size(), 3u);
  EXPECT_EQ(parts[0], "a");
  EXPECT_EQ(parts[1], "b");
  EXPECT_EQ(parts[2], "c");
}

TEST(StringUtilTest, SplitPreservesEmptySegments)
{
  const auto parts = Split("a,,c,", ',');
  ASSERT_EQ(parts.size(), 4u);
  EXPECT_EQ(parts[0], "a");
  EXPECT_EQ(parts[1], "");
  EXPECT_EQ(parts[2], "c");
  EXPECT_EQ(parts[3], "");
}

TEST(StringUtilTest, SplitSingleElement)
{
  const auto parts = Split("solo", ',');
  ASSERT_EQ(parts.size(), 1u);
  EXPECT_EQ(parts[0], "solo");
}

TEST(StringUtilTest, SplitByString)
{
  const auto parts = Split("x--y--z", "--");
  ASSERT_EQ(parts.size(), 3u);
  EXPECT_EQ(parts[0], "x");
  EXPECT_EQ(parts[1], "y");
  EXPECT_EQ(parts[2], "z");
}

TEST(StringUtilTest, SplitEmptyDelimiter)
{
  const auto parts = Split("abc", "");
  ASSERT_EQ(parts.size(), 1u);
  EXPECT_EQ(parts[0], "abc");
}

TEST(StringUtilTest, Join)
{
  EXPECT_EQ(Join({"a", "b", "c"}, ", "), "a, b, c");
  EXPECT_EQ(Join({"single"}, ","), "single");
  EXPECT_EQ(Join({}, ","), "");
  EXPECT_EQ(Join({"", "x"}, "-"), "-x");
}

TEST(StringUtilTest, ReplaceAll)
{
  EXPECT_EQ(ReplaceAll("hello world", "o", "0"), "hell0 w0rld");
  EXPECT_EQ(ReplaceAll("aaaa", "aa", "b"), "bb");
  EXPECT_EQ(ReplaceAll("abc", "z", "x"), "abc");
  EXPECT_EQ(ReplaceAll("abc", "", "x"), "abc");
}

} // namespace
} // namespace neko::base
