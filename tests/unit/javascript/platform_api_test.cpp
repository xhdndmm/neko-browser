// Platform Web API binding tests: TextEncoder/TextDecoder, atob/btoa,
// queueMicrotask, structuredClone, reportError and CSS (escape/supports).
// They exercise the real engine subsystems (UTF-8 strings, the WHATWG encoding
// decoders, the microtask pump and the console sink), not stubs.

#include "neko/dom/query.h"
#include "neko/html/parser.h"
#include "neko/javascript/dom_binding.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace neko::javascript {
namespace {

class PlatformApiTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    document_ =
        html::Parser(R"(<!doctype html><html><body><p id="text">hi</p></body></html>)").Parse();
    PageApis apis;
    apis.location_href = []() { return "https://www.example.com/"; };
    binder_ = std::make_unique<DomBinder>(*document_, apis);
    binder_->SetConsoleSink([this](std::string_view level, std::string_view text) {
      console_.push_back(std::string(level) + ": " + std::string(text));
    });
  }

  std::string EvalString(const std::string& code)
  {
    auto result = binder_->Evaluate(code);
    if (!result.has_value()) {
      return "<error: " + result.error().message() + ">";
    }
    auto text = result.value().ToString();
    return text.has_value() ? text.value() : "<tostring-error>";
  }

  double EvalNumber(const std::string& code)
  {
    auto result = binder_->Evaluate(code);
    if (!result.has_value()) {
      return -1e9;
    }
    auto number = result.value().ToNumber();
    return number.has_value() ? number.value() : -1e9;
  }

  bool EvalBool(const std::string& code)
  {
    auto result = binder_->Evaluate(code);
    if (!result.has_value()) {
      return false;
    }
    auto boolean = result.value().ToBoolean();
    return boolean.has_value() && boolean.value();
  }

  std::unique_ptr<dom::Document> document_;
  std::unique_ptr<DomBinder> binder_;
  std::vector<std::string> console_;
};

TEST_F(PlatformApiTest, TextEncoderProducesUtf8)
{
  EXPECT_EQ(EvalString("new TextEncoder().encoding"), "utf-8");
  // "é" is two UTF-8 bytes, "中" is three: the view reports them as bytes.
  EXPECT_EQ(EvalNumber("new TextEncoder().encode('é').length"), 2.0);
  EXPECT_EQ(EvalNumber("new TextEncoder().encode('中').length"), 3.0);
  EXPECT_EQ(EvalNumber("new TextEncoder().encode('é')[0]"), 0xc3);
  EXPECT_EQ(EvalNumber("new TextEncoder().encode('é')[1]"), 0xa9);
  EXPECT_EQ(EvalNumber("new TextEncoder().encode('abc').length"), 3.0);
  EXPECT_EQ(EvalNumber("new TextEncoder().encode('').length"), 0.0);
  // encodeInto writes into a caller-provided array and reports the progress.
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var dest = new Uint8Array(4);"
                       "  var result = new TextEncoder().encodeInto('abcdef', dest);"
                       "  return result.written === 4 && result.read === 4"
                       "      && dest[0] === 97 && dest[3] === 100;"
                       "})()"));
}

TEST_F(PlatformApiTest, TextDecoderDecodesWithTheEngineDecoders)
{
  EXPECT_EQ(EvalString("new TextDecoder().decode(new Uint8Array([104, 105]))"), "hi");
  EXPECT_EQ(EvalString("new TextDecoder('utf-8').decode(new Uint8Array([]))"), "");
  // A non-UTF-8 label uses the engine's WHATWG decoder for that encoding.
  // The canonical name is reported (browsers map gbk onto gb18030).
  EXPECT_EQ(EvalString("new TextDecoder('gbk').encoding"), "gb18030");
  EXPECT_EQ(EvalString("new TextDecoder('gbk').decode(new Uint8Array([196, 227]))"), "你");
  EXPECT_EQ(EvalString("new TextDecoder('iso-8859-2').encoding"), "iso-8859-2");
  // An ArrayBuffer (not just a view) is accepted.
  EXPECT_EQ(EvalString("(function() {"
                       "  var buffer = new Uint8Array([111, 107]).buffer;"
                       "  return new TextDecoder().decode(buffer);"
                       "})()"),
            "ok");
  // Unknown labels throw a RangeError, like browsers.
  EXPECT_TRUE(EvalBool("(function(){ try { new TextDecoder('no-such-encoding'); }"
                       " catch (e) { return e.name === 'RangeError'; } return false; })()"));
}

TEST_F(PlatformApiTest, AtobAndBtoaRoundTrip)
{
  EXPECT_EQ(EvalString("btoa('hello')"), "aGVsbG8=");
  EXPECT_EQ(EvalString("btoa('')"), "");
  EXPECT_EQ(EvalString("atob('aGVsbG8=')"), "hello");
  EXPECT_EQ(EvalString("atob(btoa('a\\u00ffb'))"), "a\u00ffb");
  // Padding is optional and whitespace is skipped, as in browsers.
  EXPECT_EQ(EvalString("atob('aGVsbG8')"), "hello");
  EXPECT_EQ(EvalString("atob('aGVs bG8=')"), "hello");
  // Invalid input throws; non-Latin-1 text cannot be encoded.
  EXPECT_TRUE(EvalBool("(function(){ try { atob('!!!!'); }"
                       " catch (e) { return e.name === 'TypeError'; } return false; })()"));
  EXPECT_TRUE(EvalBool("(function(){ try { btoa('中'); }"
                       " catch (e) { return e.name === 'TypeError'; } return false; })()"));
}

TEST_F(PlatformApiTest, QueueMicrotaskRunsAfterTheCurrentScript)
{
  // The pump drains the job queue at the end of Evaluate(), so the synchronous
  // part of the same evaluation runs first and the callback afterwards.
  EXPECT_EQ(EvalString("var order = [];"
                       "queueMicrotask(function () { order.push('micro'); });"
                       "order.push('sync');"
                       "order.join(',')"),
            "sync");
  EXPECT_EQ(EvalString("order.join(',')"), "sync,micro");

  // A throwing microtask is reported to the console instead of breaking the
  // pump: a following microtask still runs.
  console_.clear();
  EXPECT_EQ(EvalString("var after = [];"
                       "queueMicrotask(function () { throw new Error('boom'); });"
                       "queueMicrotask(function () { after.push('still here'); });"
                       "''"),
            "");
  EXPECT_EQ(EvalString("after.join(',')"), "still here");
  ASSERT_FALSE(console_.empty());
  EXPECT_NE(console_.back().find("boom"), std::string::npos);
}

TEST_F(PlatformApiTest, StructuredCloneDeepCopiesValues)
{
  // Nested objects/arrays are independent copies.
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var original = { a: 1, nested: { list: [1, 2, 3] }, s: 'x' };"
                       "  var copy = structuredClone(original);"
                       "  copy.nested.list.push(4);"
                       "  copy.a = 2;"
                       "  return original.a === 1 && original.nested.list.length === 3"
                       "      && copy.a === 2 && copy.nested.list.length === 4;"
                       "})()"));
  // Cycles are preserved (the clone is a graph, not an infinite walk).
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var node = { name: 'n' };"
                       "  node.self = node;"
                       "  var copy = structuredClone(node);"
                       "  return copy !== node && copy.self === copy && copy.name === 'n';"
                       "})()"));
  // Dates clone by value; typed arrays clone into the same class.
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var date = new Date(1234567890);"
                       "  var cloned_date = structuredClone(date);"
                       "  var bytes = new Uint8Array([1, 2, 3]);"
                       "  var cloned_bytes = structuredClone(bytes);"
                       "  cloned_bytes[0] = 9;"
                       "  return cloned_date instanceof Date"
                       "      && cloned_date.getTime() === date.getTime()"
                       "      && cloned_bytes instanceof Uint8Array"
                       "      && cloned_bytes.length === 3 && cloned_bytes[1] === 2"
                       "      && bytes[0] === 1;"
                       "})()"));
  // Non-cloneable values throw a DataCloneError.
  EXPECT_TRUE(EvalBool("(function(){ try { structuredClone(function () {}); }"
                       " catch (e) { return e.name === 'DataCloneError'; } return false; })()"));
  EXPECT_TRUE(EvalBool("(function(){ try { structuredClone(new Map()); }"
                       " catch (e) { return e.name === 'DataCloneError'; } return false; })()"));
}

TEST_F(PlatformApiTest, ReportErrorLogsAndFiresTheErrorEvent)
{
  console_.clear();
  EXPECT_EQ(EvalString("window.__errors = 0;"
                       "window.addEventListener('error', function () { window.__errors++; });"
                       "reportError(new Error('reported failure'));"
                       "String(window.__errors)"),
            "1");
  ASSERT_FALSE(console_.empty());
  bool logged = false;
  for (const std::string& line : console_) {
    if (line.find("reported failure") != std::string::npos) {
      logged = true;
    }
  }
  EXPECT_TRUE(logged) << "console did not receive the reported error";
}

TEST_F(PlatformApiTest, CssEscapeAndSupports)
{
  EXPECT_EQ(EvalString("CSS.escape('a b')"), "a\\ b");
  EXPECT_EQ(EvalString("CSS.escape('simple-id')"), "simple-id");
  // A leading digit is escaped with its code point (CSSOM serialization).
  EXPECT_EQ(EvalString("CSS.escape('1abc')"), "\\31 abc");
  EXPECT_EQ(EvalString("CSS.escape('')"), "");
  EXPECT_TRUE(EvalBool("CSS.supports('display', 'flex')"));
  EXPECT_TRUE(EvalBool("CSS.supports('width', '10px')"));
  EXPECT_TRUE(EvalBool("CSS.supports('color', '#ff0000')"));
  // The single-string form is not implemented: it reports false, never a guess.
  EXPECT_FALSE(EvalBool("CSS.supports('display: flex')"));
  // A value the engine's own parser rejects is not "supported".
  EXPECT_FALSE(EvalBool("CSS.supports('width', '}')"));
}

// DOM-side platform additions: getAttributeNames (document order) and
// scrollIntoView (a real scroll request through the browser layer).
TEST(PlatformDomApiTest, GetAttributeNamesAndScrollIntoView)
{
  std::unique_ptr<dom::Document> document =
      html::Parser("<html><body><p id=p data-x=1 hidden>text</p></body></html>").Parse();
  std::vector<std::pair<double, double>> scroll_requests;
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com/"; };
  apis.scroll_to = [&scroll_requests](double x, double y) { scroll_requests.emplace_back(x, y); };
  apis.element_geometry = [](const dom::Element& element) -> std::optional<ElementGeometry> {
    const auto id = element.GetAttribute("id");
    if (!id.has_value() || id.value() != "p") {
      return std::nullopt;
    }
    ElementGeometry geometry;
    geometry.x = 5;
    geometry.y = 120;
    geometry.width = 100;
    geometry.height = 20;
    return geometry;
  };
  DomBinder binder(*document, apis);

  const auto evaluate = [&binder](const std::string& code) { return binder.Evaluate(code); };
  auto names = evaluate("document.getElementById('p').getAttributeNames().join(',')");
  ASSERT_TRUE(names.has_value()) << names.error().message();
  auto joined = names.value().ToString();
  ASSERT_TRUE(joined.has_value());
  EXPECT_EQ(joined.value(), "id,data-x,hidden");

  auto scrolled = evaluate("document.getElementById('p').scrollIntoView()");
  ASSERT_TRUE(scrolled.has_value()) << scrolled.error().message();
  ASSERT_EQ(scroll_requests.size(), 1u);
  EXPECT_DOUBLE_EQ(scroll_requests[0].first, 0.0);
  EXPECT_DOUBLE_EQ(scroll_requests[0].second, 120.0);

  // An element without a laid-out box does not request a scroll.
  auto missing = evaluate("document.body.scrollIntoView(); 0");
  EXPECT_TRUE(missing.has_value());
  EXPECT_EQ(scroll_requests.size(), 1u);
}

} // namespace
} // namespace neko::javascript
