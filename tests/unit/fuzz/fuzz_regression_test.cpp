// Deterministic fuzz-regression suite (AGENTS.md §32/§17).
//
// Replays every committed corpus file under tests/fuzz/corpus/<area>/ plus a
// fixed adversarial battery through the same harness bodies the libFuzzer
// executables use (tests/fuzz/fuzz_targets.cpp).  Runs on every build on
// every toolchain, so:
//
//   * a crash reproducer committed as a corpus file is a permanent regression
//     test even where libFuzzer is unavailable (GCC/MSVC CI);
//   * the parsers' structural defenses (deep nesting, huge tokens, malformed
//     UTF-8, integer-overflow framing) stay covered between fuzzing runs.
//
// The battery is not a substitute for real fuzzing runs: it is the floor.
// See docs/testing/fuzzing.md.

#include "fuzz_targets.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include <vector>

namespace neko::fuzz {
namespace {

std::string ReadFile(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Every file under tests/fuzz/corpus/<area>/.  New crash reproducers land
// here as files and are picked up automatically.
std::vector<std::string> CorpusInputs(const std::string& area)
{
  std::vector<std::string> inputs;
  const std::filesystem::path dir = std::filesystem::path(NEKO_TEST_FUZZ_DIR) / "corpus" / area;
  if (std::filesystem::is_directory(dir)) {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (entry.is_regular_file()) {
        inputs.push_back(ReadFile(entry.path()));
      }
    }
  }
  // The harness must also survive the degenerate inputs libFuzzer starts
  // from even when the corpus directory is empty.
  inputs.emplace_back();
  inputs.emplace_back("\0", 1);
  return inputs;
}

void RunAll(void (*target)(const std::uint8_t*, std::size_t),
            const std::string& area,
            const std::vector<std::string>& battery)
{
  for (const std::string& input : CorpusInputs(area)) {
    target(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
  }
  for (const std::string& input : battery) {
    target(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
  }
}

std::string Repeat(const std::string& unit, int times)
{
  std::string out;
  out.reserve(unit.size() * static_cast<std::size_t>(times));
  for (int i = 0; i < times; ++i) {
    out += unit;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Adversarial batteries.  Each input targets a defense the parsers are
// supposed to have (AGENTS.md §67): deep nesting, huge tokens, malformed
// UTF-8, framing integer overflow, pathological repetition.
// ---------------------------------------------------------------------------

const std::vector<std::string>& UrlBattery()
{
  static const std::vector<std::string> battery = {
      Repeat("a", 1 << 20),                         // huge input
      "http://[" + Repeat(":", 1000),               // unterminated IPv6
      "http://h:" + Repeat("9", 40) + "/",          // port integer overflow
      "http://a\r\nX: y/",                          // header-injection bytes
      std::string("http://a\0b/", 9),               // embedded NUL
      "h" + Repeat("t", 10000) + "p://x/",          // huge scheme
      "https://example.com/" + Repeat("../", 5000), // path traversal chain
      std::string("\xff\xfe\xfd", 3),               // invalid UTF-8
      "data:" + Repeat("%", 10000),                 // percent-decoding flood
      "?x=" + Repeat("&y=1", 50000),                // huge query
  };
  return battery;
}

const std::vector<std::string>& HttpBattery()
{
  static const std::vector<std::string> battery = {
      "HTTP/1.1 200 OK\r\n" + Repeat("X: y\r\n", 10000) + "\r\n", // header flood
      "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\nx",
      "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999\r\n\r\n",
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + Repeat("1\r\nz\r\n", 10000) +
          "0\r\n\r\n", // chunk flood
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFF\r\n",
      "HTTP/1.1 " + Repeat("9", 100) + " x\r\n\r\n", // huge status code
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n" + Repeat("\r\n", 100) + "hi",
      std::string("HTTP/1.1 200 OK\r\nA: \xff\xfe\r\n\r\n", 22), // bad bytes
  };
  return battery;
}

const std::vector<std::string>& HtmlBattery()
{
  static const std::vector<std::string> battery = {
      Repeat("<div>", 10000) + Repeat("</div>", 10000), // deep element nesting
      Repeat("<b>", 5000) + "x",                        // deep active formatting
      "<div " + Repeat("a='1' ", 5000) + ">",           // attribute flood
      "<!--" + Repeat("x", 1 << 20) + "-->",            // huge comment
      "<textarea" + Repeat("x", 1 << 16),               // huge RAWTEXT/RCDATA tag
      std::string("<p>\xff\xfe\xfd</p>", 8),            // malformed UTF-8
      Repeat("<table><tr><td>", 2000),                  // deep table nesting
      "<template>" + Repeat("<template>", 500) + Repeat("</template>", 500),
      Repeat("<select><option>", 2000), // deep select nesting
      Repeat("&", 100000),              // char-ref flood
  };
  return battery;
}

const std::vector<std::string>& CssBattery()
{
  static const std::vector<std::string> battery = {
      Repeat("@media all{", 1000) + Repeat("}", 1000),          // deep at-rule nesting
      Repeat("a>", 5000) + "b{}",                               // deep selector combinator
      "a{" + Repeat("x:y;", 100000) + "}",                      // declaration flood
      "a{width:calc(" + Repeat("1+", 10000) + "1)}",            // deep calc nesting
      "a{width:repeat(" + Repeat("9", 20) + ",1fr)}",           // repeat() overflow
      "a{--x:var(--x)}",                                        // self-referential var
      "a{" + Repeat("/*x*/", 100000) + "}",                     // comment flood
      std::string("a{content:'\xff\xfe'}", 13),                 // malformed UTF-8
      Repeat("@font-face{src:url(", 5000),                      // unclosed url(
      "a{grid-template-columns:" + Repeat("1px ", 50000) + "}", // huge track list
  };
  return battery;
}

// ---------------------------------------------------------------------------
// Suites.
// ---------------------------------------------------------------------------

TEST(FuzzRegressionTest, UrlCorpusAndBattery)
{
  RunAll(&Url, "url", UrlBattery());
}

TEST(FuzzRegressionTest, HttpCorpusAndBattery)
{
  RunAll(&Http, "http", HttpBattery());
}

TEST(FuzzRegressionTest, HtmlCorpusAndBattery)
{
  RunAll(&Html, "html", HtmlBattery());
}

TEST(FuzzRegressionTest, CssCorpusAndBattery)
{
  RunAll(&Css, "css", CssBattery());
}

// A corpus directory that silently stopped being read would hide crash
// reproducers from CI.  Pin the floor: every area must contribute seeds.
TEST(FuzzRegressionTest, CorpusDirectoriesArePopulated)
{
  for (const char* area : {"url", "http", "html", "css"}) {
    const std::filesystem::path dir = std::filesystem::path(NEKO_TEST_FUZZ_DIR) / "corpus" / area;
    ASSERT_TRUE(std::filesystem::is_directory(dir)) << dir;
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (entry.is_regular_file()) {
        ++files;
      }
    }
    EXPECT_GE(files, 5u) << dir;
  }
}

} // namespace
} // namespace neko::fuzz
