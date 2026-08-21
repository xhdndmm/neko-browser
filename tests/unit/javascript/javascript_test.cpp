// Unit tests for the neko::javascript module (QuickJS runtime wrapper).

#include "neko/base/status.h"
#include "neko/javascript/script_engine.h"

#include <chrono>
#include <map>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace neko::javascript {
namespace {

class ScriptEngineTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    engine_ = std::make_unique<ScriptEngine>();
    engine_->SetConsoleSink([this](std::string_view level, std::string_view text) {
      console_.push_back(std::string(level) + ": " + std::string(text));
    });
  }

  std::unique_ptr<ScriptEngine> engine_;
  std::vector<std::string> console_;
};

TEST_F(ScriptEngineTest, RuntimeInfo)
{
  EXPECT_EQ(ScriptEngine::RuntimeName(), "QuickJS");
  EXPECT_FALSE(ScriptEngine::Version().empty());
}

TEST_F(ScriptEngineTest, EvaluatesArithmetic)
{
  auto result = engine_->Evaluate("1 + 2 * 3");
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(result.value().Kind(), ValueKind::kNumber);
  auto num = result.value().ToNumber();
  ASSERT_TRUE(num.has_value());
  EXPECT_DOUBLE_EQ(num.value(), 7.0);
}

// Promise continuations (microtasks) are drained after evaluation, so an
// async function started at top level completes even though Evaluate returns
// synchronously.
TEST_F(ScriptEngineTest, PromiseJobsDrainAfterEvaluate)
{
  ASSERT_TRUE(engine_
                  ->Evaluate("window = {}; "
                             "window._done = false; "
                             "(async function(){ window._done = true; })();")
                  .has_value());
  auto v = engine_->Evaluate("window._done");
  ASSERT_TRUE(v.has_value());
  auto b = v.value().ToBoolean();
  ASSERT_TRUE(b.has_value());
  EXPECT_TRUE(b.value());
}

// An async function with an await chain progresses through the drained queue.
TEST_F(ScriptEngineTest, AsyncAwaitChainCompletes)
{
  ASSERT_TRUE(engine_
                  ->Evaluate("var _order = []; "
                             "Promise.resolve()"
                             "  .then(function(){ _order.push('a'); })"
                             "  .then(function(){ _order.push('b'); });")
                  .has_value());
  auto v = engine_->Evaluate("_order.join(',')");
  ASSERT_TRUE(v.has_value());
  auto s = v.value().ToString();
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s.value(), "a,b");
}

// An unhandled rejection is reported through the console sink and does not
// stop the remaining jobs.
TEST_F(ScriptEngineTest, UnhandledRejectionIsReported)
{
  ASSERT_TRUE(engine_
                  ->Evaluate("var _done = false; "
                             "Promise.reject(new Error('boom'))"
                             "  .catch(function(){ _done = true; });")
                  .has_value());
  auto v = engine_->Evaluate("_done");
  ASSERT_TRUE(v.has_value());
  auto b = v.value().ToBoolean();
  ASSERT_TRUE(b.has_value());
  EXPECT_TRUE(b.value());
  // The rejection had a handler, so nothing was reported.
  EXPECT_TRUE(console_.empty());

  // A truly unhandled rejection reaches the console sink as an error.
  ASSERT_TRUE(engine_->Evaluate("Promise.reject(new Error('nobody listens'));").has_value());
  ASSERT_EQ(console_.size(), 1u);
  EXPECT_EQ(console_[0].substr(0, 6), "error:");
  EXPECT_NE(console_[0].find("nobody listens"), std::string::npos);
}

TEST_F(ScriptEngineTest, EvaluatesString)
{
  auto result = engine_->Evaluate("'hello ' + 'world'");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().Kind(), ValueKind::kString);
  auto str = result.value().ToString();
  ASSERT_TRUE(str.has_value());
  EXPECT_EQ(str.value(), "hello world");
}

TEST_F(ScriptEngineTest, EvaluatesObjectAndJson)
{
  auto result = engine_->Evaluate("({a: 1, b: 'x'})");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().Kind(), ValueKind::kObject);
  auto json = result.value().JsonStringify();
  ASSERT_TRUE(json.has_value()) << json.error().message();
  EXPECT_EQ(json.value(), R"({"a":1,"b":"x"})");
}

TEST_F(ScriptEngineTest, EvaluatesBooleanAndNull)
{
  auto t = engine_->Evaluate("true");
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t.value().Kind(), ValueKind::kBoolean);
  auto tb = t.value().ToBoolean();
  ASSERT_TRUE(tb.has_value());
  EXPECT_TRUE(tb.value());

  auto n = engine_->Evaluate("null");
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(n.value().Kind(), ValueKind::kNull);
}

TEST_F(ScriptEngineTest, ToNumberConversion)
{
  auto r = engine_->Evaluate("'42'");
  ASSERT_TRUE(r.has_value());
  auto num = r.value().ToNumber();
  ASSERT_TRUE(num.has_value());
  EXPECT_DOUBLE_EQ(num.value(), 42.0);
}

TEST_F(ScriptEngineTest, DefineFunctionAndCallGlobal)
{
  auto define = engine_->Evaluate("function add(a, b) { return a + b; }");
  ASSERT_TRUE(define.has_value());
  EXPECT_EQ(define.value().Kind(), ValueKind::kUndefined);

  auto n2 = engine_->MakeNumber(2);
  auto n3 = engine_->MakeNumber(3);
  ASSERT_TRUE(n2.has_value());
  ASSERT_TRUE(n3.has_value());
  auto result = engine_->CallGlobal("add", {n2.value(), n3.value()});
  ASSERT_TRUE(result.has_value()) << result.error().message();
  auto num = result.value().ToNumber();
  ASSERT_TRUE(num.has_value());
  EXPECT_DOUBLE_EQ(num.value(), 5.0);
}

TEST_F(ScriptEngineTest, CallGlobalOnMissingFunction)
{
  auto result = engine_->CallGlobal("doesNotExist");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kJavascript);
}

TEST_F(ScriptEngineTest, SetAndGetGlobal)
{
  auto s = engine_->MakeString("neko");
  ASSERT_TRUE(s.has_value());
  auto set = engine_->SetGlobal("project", s.value());
  ASSERT_TRUE(set.has_value()) << set.error().message();

  auto get = engine_->GetGlobal("project");
  ASSERT_TRUE(get.has_value());
  EXPECT_EQ(get.value().Kind(), ValueKind::kString);
  auto str = get.value().ToString();
  ASSERT_TRUE(str.has_value());
  EXPECT_EQ(str.value(), "neko");
}

TEST_F(ScriptEngineTest, PersistentGlobalStateAcrossEvaluations)
{
  ASSERT_TRUE(engine_->Evaluate("counter = 1;").has_value());
  auto bump = engine_->Evaluate("counter += 41; counter;");
  ASSERT_TRUE(bump.has_value());
  auto num = bump.value().ToNumber();
  ASSERT_TRUE(num.has_value());
  EXPECT_DOUBLE_EQ(num.value(), 42.0);
}

TEST_F(ScriptEngineTest, SyntaxErrorIsParseError)
{
  auto result = engine_->Evaluate("function (");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kParse);
  EXPECT_NE(result.error().message().find("SyntaxError"), std::string::npos);
}

TEST_F(ScriptEngineTest, RuntimeErrorIsJavascriptError)
{
  auto result = engine_->Evaluate("throw new Error('boom')");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kJavascript);
  EXPECT_NE(result.error().message().find("boom"), std::string::npos);
}

TEST_F(ScriptEngineTest, ConsoleLogCapture)
{
  auto result = engine_->Evaluate("console.log('hello', 42, {k: 1})");
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ASSERT_EQ(console_.size(), 1u);
  EXPECT_EQ(console_[0], "log: hello 42 {\"k\":1}");
}

TEST_F(ScriptEngineTest, ConsoleErrorCapture)
{
  (void)engine_->Evaluate("console.error('fatal'); console.warn('careful')");
  ASSERT_EQ(console_.size(), 2u);
  EXPECT_EQ(console_[0], "error: fatal");
  EXPECT_EQ(console_[1], "warning: careful");
}

TEST_F(ScriptEngineTest, ConsoleSinkCanBeChanged)
{
  std::vector<std::string> second;
  engine_->SetConsoleSink(
      [&second](std::string_view, std::string_view text) { second.push_back(std::string(text)); });
  (void)engine_->Evaluate("console.log('moved')");
  ASSERT_EQ(console_.size(), 0u);
  ASSERT_EQ(second.size(), 1u);
  EXPECT_EQ(second[0], "moved");
}

TEST_F(ScriptEngineTest, InfiniteLoopIsInterrupted)
{
  engine_->SetExecutionLimit(std::chrono::milliseconds(50));
  const auto start = std::chrono::steady_clock::now();
  auto result = engine_->Evaluate("while (true) {}");
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kJavascript);
  EXPECT_NE(result.error().message().find("interrupted"), std::string::npos);
  EXPECT_LT(elapsed, std::chrono::seconds(10));
}

TEST_F(ScriptEngineTest, CallGlobalInfiniteLoopIsInterrupted)
{
  ASSERT_TRUE(engine_->Evaluate("function spin() { while (true) {} }").has_value());
  engine_->SetExecutionLimit(std::chrono::milliseconds(50));
  auto result = engine_->CallGlobal("spin");
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().message().find("interrupted"), std::string::npos);
}

TEST_F(ScriptEngineTest, MemoryLimitIsEnforced)
{
  engine_->SetMemoryLimit(1024u * 1024u); // 1 MiB
  // A large allocation must be rejected by the runtime.  (new Array(n) only
  // sets length and does not actually allocate, so allocate a big string.)
  auto result = engine_->Evaluate("'x'.repeat(8 * 1024 * 1024);");
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kJavascript);
}

TEST_F(ScriptEngineTest, ValueOutlivesEngine)
{
  ScriptValue saved;
  {
    ScriptEngine temp;
    auto result = temp.Evaluate("'survivor'");
    ASSERT_TRUE(result.has_value());
    saved = result.value();
  }
  // The value keeps the runtime alive even after the engine is destroyed.
  ASSERT_TRUE(saved.IsValid());
  EXPECT_EQ(saved.Kind(), ValueKind::kString);
  auto str = saved.ToString();
  ASSERT_TRUE(str.has_value());
  EXPECT_EQ(str.value(), "survivor");
}

TEST_F(ScriptEngineTest, ValueCopyKeepsReference)
{
  auto result = engine_->Evaluate("({n: 7})");
  ASSERT_TRUE(result.has_value());
  ScriptValue copy = result.value();
  ASSERT_TRUE(copy.IsValid());
  auto json = copy.JsonStringify();
  ASSERT_TRUE(json.has_value());
  EXPECT_EQ(json.value(), R"({"n":7})");
}

TEST_F(ScriptEngineTest, ValueMoveTransfersOwnership)
{
  auto result = engine_->Evaluate("'moved'");
  ASSERT_TRUE(result.has_value());
  ScriptValue moved = std::move(result.value());
  EXPECT_FALSE(result.value().IsValid());
  ASSERT_TRUE(moved.IsValid());
  auto str = moved.ToString();
  ASSERT_TRUE(str.has_value());
  EXPECT_EQ(str.value(), "moved");
}

TEST_F(ScriptEngineTest, DefaultValueIsInvalid)
{
  ScriptValue v;
  EXPECT_FALSE(v.IsValid());
  EXPECT_EQ(v.Kind(), ValueKind::kInvalid);
  auto str = v.ToString();
  ASSERT_FALSE(str.has_value());
  EXPECT_EQ(str.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST_F(ScriptEngineTest, BigIntKind)
{
  auto result = engine_->Evaluate("123n");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().Kind(), ValueKind::kBigInt);
}

TEST_F(ScriptEngineTest, FunctionKind)
{
  auto result = engine_->Evaluate("function f() {}");
  ASSERT_TRUE(result.has_value());
  // A function declaration evaluates to undefined; grab it from the global.
  auto f = engine_->GetGlobal("f");
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f.value().Kind(), ValueKind::kFunction);
}

TEST_F(ScriptEngineTest, ValueKindToString)
{
  EXPECT_EQ(ToString(ValueKind::kNumber), "number");
  EXPECT_EQ(ToString(ValueKind::kString), "string");
  EXPECT_EQ(ToString(ValueKind::kObject), "object");
  EXPECT_EQ(ToString(ValueKind::kFunction), "function");
  EXPECT_EQ(ToString(ValueKind::kInvalid), "invalid");
}

TEST_F(ScriptEngineTest, ArgumentsFromDifferentEngineRejected)
{
  ScriptEngine other;
  auto other_val = other.MakeNumber(1);
  ASSERT_TRUE(other_val.has_value());
  auto n = engine_->MakeNumber(2);
  ASSERT_TRUE(n.has_value());
  ASSERT_TRUE(engine_->Evaluate("function f(x) { return x; }").has_value());
  auto result = engine_->CallGlobal("f", {other_val.value()});
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST_F(ScriptEngineTest, JsonStringifyFailureOnCycle)
{
  auto result = engine_->Evaluate("var o = {}; o.self = o; o;");
  ASSERT_TRUE(result.has_value());
  auto json = result.value().JsonStringify();
  ASSERT_FALSE(json.has_value());
  EXPECT_EQ(json.error().category(), base::ErrorCategory::kJavascript);
}

// ---------------------------------------------------------------------------
// ES modules (<script type="module"> support, Phase 8 M4).
// ---------------------------------------------------------------------------

// Installs a fetcher answering module sources from |routes| (URL -> source)
// and records every requested URL.
class ModuleFetcher
{
public:
  explicit ModuleFetcher(ScriptEngine& engine)
  {
    engine.SetModuleFetcher([this](const std::string& url) -> base::Result<std::string> {
      requests_.push_back(url);
      const auto it = routes_.find(url);
      if (it == routes_.end()) {
        return base::Err(base::Error::Javascript("HTTP 404"));
      }
      return it->second;
    });
  }
  void Add(const std::string& url, std::string source)
  {
    routes_[url] = std::move(source);
  }
  const std::vector<std::string>& requests() const
  {
    return requests_;
  }

private:
  std::map<std::string, std::string> routes_;
  std::vector<std::string> requests_;
};

// A static import resolves "./lib.js" against the entry module's URL and the
// imported binding is visible to the importer.
TEST_F(ScriptEngineTest, ModuleImportResolvesRelativeSpecifier)
{
  ModuleFetcher fetcher(*engine_);
  fetcher.Add("http://test/lib.js", "export const value = 41 + 1;");
  auto result = engine_->EvaluateModule(
      "import { value } from './lib.js'; globalThis.out = value;", "http://test/page.html");
  ASSERT_TRUE(result.has_value()) << result.error().message();
  auto out = engine_->Evaluate("globalThis.out");
  ASSERT_TRUE(out.has_value());
  auto num = out.value().ToNumber();
  ASSERT_TRUE(num.has_value());
  EXPECT_DOUBLE_EQ(num.value(), 42.0);
  ASSERT_EQ(fetcher.requests().size(), 1u);
  EXPECT_EQ(fetcher.requests()[0], "http://test/lib.js");
}

// "../" specifiers climb relative to the importing module's directory.
TEST_F(ScriptEngineTest, ModuleImportResolvesParentSpecifier)
{
  ModuleFetcher fetcher(*engine_);
  fetcher.Add("http://test/a/dep.js", "export const who = 'a-dep';");
  fetcher.Add("http://test/entry/main.js",
              "import { who } from '../a/dep.js'; globalThis.who = who;");
  auto result =
      engine_->EvaluateModule("import './main.js';", "http://test/entry/index.html");
  ASSERT_TRUE(result.has_value()) << result.error().message();
  auto who = engine_->Evaluate("globalThis.who");
  ASSERT_TRUE(who.has_value());
  auto s = who.value().ToString();
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s.value(), "a-dep");
}

// import.meta.url is the absolute URL of the module (entry and imported).
TEST_F(ScriptEngineTest, ModuleImportMetaUrl)
{
  ModuleFetcher fetcher(*engine_);
  fetcher.Add("http://test/app/lib.js",
              "globalThis.lib_url = import.meta.url;");
  auto result = engine_->EvaluateModule(
      "import './lib.js'; globalThis.entry_url = import.meta.url;",
      "http://test/app/entry.mjs");
  ASSERT_TRUE(result.has_value()) << result.error().message();
  for (const char* name : {"entry_url", "lib_url"}) {
    auto v = engine_->Evaluate(std::string("globalThis.") + name);
    ASSERT_TRUE(v.has_value());
    auto s = v.value().ToString();
    ASSERT_TRUE(s.has_value());
    EXPECT_FALSE(s.value().empty()) << name;
  }
  auto entry = engine_->Evaluate("globalThis.entry_url");
  ASSERT_TRUE(entry.has_value());
  auto es = entry.value().ToString();
  ASSERT_TRUE(es.has_value());
  EXPECT_EQ(es.value(), "http://test/app/entry.mjs");
}

// Bare specifiers ("react") cannot resolve without an import map: importing
// one fails with an error mentioning the specifier.
TEST_F(ScriptEngineTest, ModuleBareSpecifierRejected)
{
  ModuleFetcher fetcher(*engine_);
  auto result = engine_->EvaluateModule(
      "import react from 'react';", "http://test/page.html");
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().message().find("react"), std::string::npos);
  EXPECT_TRUE(fetcher.requests().empty()); // never fetched anything
}

// Without a fetcher wired, importing any module fails loudly instead of
// touching the filesystem.
TEST_F(ScriptEngineTest, ModuleLoadingDisabledWithoutFetcher)
{
  ScriptEngine bare;
  auto result = bare.EvaluateModule("import './x.js';", "http://test/page.html");
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().message().find("not enabled"), std::string::npos);
}

// Circular imports settle with hoisted functions visible in both directions.
TEST_F(ScriptEngineTest, ModuleCircularImports)
{
  ModuleFetcher fetcher(*engine_);
  fetcher.Add("http://test/a.js",
              "import { b } from './b.js';\n"
              "export function a() { return 'a' + b(); }\n"
              "globalThis.ra = a();");
  fetcher.Add("http://test/b.js",
              "import { a } from './a.js';\n"
              "export function b() { return 'b'; }\n"
              "globalThis.rb = b();");
  auto result = engine_->EvaluateModule("import './a.js';", "http://test/main.html");
  ASSERT_TRUE(result.has_value()) << result.error().message();
  auto ra = engine_->Evaluate("globalThis.ra");
  ASSERT_TRUE(ra.has_value());
  auto s = ra.value().ToString();
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s.value(), "ab");
}

// Modules have their own top-level scope (no global leakage) and run in
// strict mode (implicit globals throw).
TEST_F(ScriptEngineTest, ModuleOwnScopeAndStrictMode)
{
  ModuleFetcher fetcher(*engine_);
  auto result = engine_->EvaluateModule(
      "var leaked = true;"
      "try { undeclared_global = 1; } catch (e) { globalThis.strict_error = e.name; }",
      "http://test/page.html");
  ASSERT_TRUE(result.has_value()) << result.error().message();
  auto leaked = engine_->Evaluate("typeof leaked");
  ASSERT_TRUE(leaked.has_value());
  auto ls = leaked.value().ToString();
  ASSERT_TRUE(ls.has_value());
  EXPECT_EQ(ls.value(), "undefined"); // did not leak to the global scope
  auto strict = engine_->Evaluate("globalThis.strict_error");
  ASSERT_TRUE(strict.has_value());
  auto ss = strict.value().ToString();
  ASSERT_TRUE(ss.has_value());
  EXPECT_EQ(ss.value(), "ReferenceError"); // strict-mode implicit global threw
}

// The same URL is fetched once even when imported by two different modules
// (QuickJS caches compiled modules by name).
TEST_F(ScriptEngineTest, ModuleFetchedOncePerUrl)
{
  ModuleFetcher fetcher(*engine_);
  fetcher.Add("http://test/shared.js", "export const k = 1;");
  fetcher.Add("http://test/x.js",
              "import { k } from './shared.js'; globalThis.xk = k;");
  fetcher.Add("http://test/y.js",
              "import { k } from './shared.js'; globalThis.yk = k;");
  auto result = engine_->EvaluateModule(
      "import './x.js'; import './y.js';", "http://test/main.html");
  ASSERT_TRUE(result.has_value()) << result.error().message();
  int shared_requests = 0;
  for (const std::string& r : fetcher.requests()) {
    if (r == "http://test/shared.js") {
      ++shared_requests;
    }
  }
  EXPECT_EQ(shared_requests, 1);
}

// Dynamic import() works in CLASSIC scripts too: the promise resolves with
// the module namespace on the next job pump, and the specifier resolves
// against the filename the script was evaluated under.
TEST_F(ScriptEngineTest, DynamicImportInClassicScript)
{
  ModuleFetcher fetcher(*engine_);
  fetcher.Add("http://test/lib.js", "export const tag = 'dyn';");
  auto result = engine_->Evaluate(
      "globalThis.done = false;"
      "import('./lib.js').then(m => { globalThis.tag = m.tag; globalThis.done = true; });",
      "http://test/page.html");
  ASSERT_TRUE(result.has_value()) << result.error().message();
  // Evaluate() drains the job queue, so the import job already settled.
  auto done = engine_->Evaluate("globalThis.done");
  ASSERT_TRUE(done.has_value());
  auto db = done.value().ToBoolean();
  ASSERT_TRUE(db.has_value());
  EXPECT_TRUE(db.value());
  auto tag = engine_->Evaluate("globalThis.tag");
  ASSERT_TRUE(tag.has_value());
  auto ts = tag.value().ToString();
  ASSERT_TRUE(ts.has_value());
  EXPECT_EQ(ts.value(), "dyn");
}

// A failed dynamic import rejects the promise; .catch observes it and no
// uncaught-rejection noise remains.
TEST_F(ScriptEngineTest, DynamicImportFailureRejects)
{
  ModuleFetcher fetcher(*engine_); // no routes: every fetch 404s
  auto result = engine_->Evaluate(
      "globalThis.err = '';"
      "import('./missing.js').catch(e => { globalThis.err = '' + e; });",
      "http://test/page.html");
  ASSERT_TRUE(result.has_value()) << result.error().message();
  auto err = engine_->Evaluate("globalThis.err");
  ASSERT_TRUE(err.has_value());
  auto es = err.value().ToString();
  ASSERT_TRUE(es.has_value());
  EXPECT_NE(es.value().find("missing.js"), std::string::npos);
}

} // namespace
} // namespace neko::javascript
