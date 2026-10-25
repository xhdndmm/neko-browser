// neko::browser::RunPageScripts — external script loading.
//
// ADR 0022: script bodies are fetched in parallel (what a browser's preload
// scanner does) while execution stays in document order.  Fetching each body at
// execution time serialised a page's scripts: on bilibili.com that was seven
// back-to-back round trips per load.

#include "neko/base/thread_pool.h"
#include "neko/browser/page_scripts.h"
#include "neko/network/http.h"
#include "neko/renderer/page.h"

#include <atomic>
#include <chrono>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace neko::browser {
namespace {

// A fetcher that answers every script with `document.title += '<letter>'`,
// records how many fetches ran at once, and blocks until every expected fetch
// has started.  The wait is bounded, so a serial engine fails the assertion
// instead of hanging the test.
class ScriptFetcherRecorder
{
public:
  struct Reply
  {
    bool ok = true;
    std::string body; // when ok
  };

  explicit ScriptFetcherRecorder(int expected_fetches) : expected_(expected_fetches) {}

  // Pre-register what to answer for a path; unknown paths get a letter derived
  // from the file name.
  void SetReply(const std::string& path, Reply reply)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    replies_[path] = std::move(reply);
  }

  base::Result<network::HttpResponse> Fetch(const url::Url& url)
  {
    const int concurrent = ++in_flight_;
    int highest = max_concurrent_.load();
    while (highest < concurrent && !max_concurrent_.compare_exchange_weak(highest, concurrent)) {
    }
    Reply answer;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      urls_.push_back(url.Serialize());
      const auto entry = replies_.find(url.path());
      answer = entry != replies_.end() ? entry->second : DefaultReply(url.path());
    }
    // Wait until every expected fetch has started, so that a serial engine is
    // distinguishable from a parallel one.  Bounded: a serial engine fails the
    // assertion instead of hanging the test.
    ++started_;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (started_.load() < expected_ && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(1ms);
    }
    --in_flight_;
    if (!answer.ok) {
      return base::Err(base::Error::Network("simulated failure"));
    }
    network::HttpResponse response;
    response.status_code = 200;
    response.body = answer.body;
    return base::Ok(std::move(response));
  }

  int max_concurrent() const
  {
    return max_concurrent_.load();
  }
  std::size_t fetch_count() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return urls_.size();
  }
  std::vector<std::string> urls() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return urls_;
  }

private:
  static Reply DefaultReply(const std::string& path)
  {
    // "/a.js" -> document.title += 'A'
    Reply reply;
    char letter = '?';
    for (const char c : path) {
      if (c >= 'a' && c <= 'z') {
        letter = static_cast<char>(c - 'a' + 'A');
        break;
      }
    }
    reply.body = std::string("document.title += '") + letter + "';";
    return reply;
  }

  int expected_ = 0;
  std::atomic<int> in_flight_{0};
  std::atomic<int> max_concurrent_{0};
  std::atomic<int> started_{0};
  mutable std::mutex mutex_;
  std::vector<std::string> urls_;
  std::map<std::string, Reply> replies_;
};

std::string ThreeScriptPage()
{
  return "<!DOCTYPE html><html><head><title></title>"
         "<script src=\"/a.js\"></script>"
         "<script src=\"/b.js\"></script>"
         "<script src=\"/c.js\"></script>"
         "</head><body></body></html>";
}

std::shared_ptr<javascript::DomBinder> RunScripts(renderer::Page& page,
                                                  const std::string& html,
                                                  ScriptFetcherRecorder& recorder,
                                                  base::ThreadPool& pool,
                                                  javascript::ScriptEngine::ConsoleSink sink = {},
                                                  ScriptTimings* timings = nullptr)
{
  if (!page.LoadHtml(html).has_value()) {
    return nullptr;
  }
  return RunPageScripts(
      page,
      "https://example.test/",
      [&recorder](const url::Url& url, const RequestContext&) { return recorder.Fetch(url); },
      std::move(sink),
      pool,
      {},
      nullptr,
      timings);
}

TEST(PageScriptsTest, FetchesExternalScriptsInParallelAndExecutesInDocumentOrder)
{
  renderer::Page page;
  ScriptFetcherRecorder recorder(3);
  base::ThreadPool pool(8);

  const std::shared_ptr<javascript::DomBinder> binder =
      RunScripts(page, ThreeScriptPage(), recorder, pool);
  ASSERT_NE(binder, nullptr);

  // All three bodies were in flight together: fetching them is not serialised.
  EXPECT_EQ(recorder.max_concurrent(), 3);
  // ... while execution stayed in document order.
  EXPECT_EQ(page.document()->Title(), "ABC");
  // Relative src attributes resolve against the document URL.
  const std::vector<std::string> urls = recorder.urls();
  EXPECT_THAT(urls, ::testing::Each(::testing::StartsWith("https://example.test/")));
}

TEST(PageScriptsTest, FetchesOneScriptTaggedSharedUrlOnceAndRunsItTwice)
{
  renderer::Page page;
  ScriptFetcherRecorder recorder(2); // a.js and b.js: two unique resources
  base::ThreadPool pool(8);

  const std::string html = "<!DOCTYPE html><html><head><title></title>"
                           "<script src=\"/a.js\"></script>"
                           "<script src=\"/b.js\"></script>"
                           "<script src=\"/a.js\"></script>"
                           "</head><body></body></html>";
  const std::shared_ptr<javascript::DomBinder> binder = RunScripts(page, html, recorder, pool);
  ASSERT_NE(binder, nullptr);

  EXPECT_EQ(recorder.fetch_count(), 2u);
  EXPECT_EQ(page.document()->Title(), "ABA");
}

TEST(PageScriptsTest, FailedFetchIsReportedAndLaterScriptsStillRun)
{
  renderer::Page page;
  ScriptFetcherRecorder recorder(2);
  ScriptFetcherRecorder::Reply failure;
  failure.ok = false;
  recorder.SetReply("/a.js", failure);
  base::ThreadPool pool(8);

  std::vector<std::string> errors;
  const std::string html = "<!DOCTYPE html><html><head><title></title>"
                           "<script src=\"/a.js\"></script>"
                           "<script src=\"/b.js\"></script>"
                           "</head><body></body></html>";
  const std::shared_ptr<javascript::DomBinder> binder = RunScripts(
      page, html, recorder, pool, [&errors](std::string_view level, std::string_view text) {
        errors.push_back(std::string(level) + ": " + std::string(text));
      });
  ASSERT_NE(binder, nullptr);

  EXPECT_EQ(page.document()->Title(), "B"); // b.js still ran
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_NE(errors.front().find("script: fetch failed for https://example.test/a.js"),
            std::string::npos)
      << errors.front();
}

// The pump tracker is the smoothness signal --perf-report publishes, so it must
// actually observe the turns: a page with a deferred callback produces at least
// one turn, and the count matches the turns the function reported.
TEST(PageScriptsTest, PumpTrackerRecordsEveryTurn)
{
  renderer::Page page;
  ScriptFetcherRecorder recorder(0);
  base::ThreadPool pool(4);
  const std::string html =
      "<!DOCTYPE html><html><head><title></title></head><body>"
      "<script>setTimeout(function () { document.title = 'late'; }, 0);</script>"
      "</body></html>";
  const std::shared_ptr<javascript::DomBinder> binder = RunScripts(page, html, recorder, pool);
  ASSERT_NE(binder, nullptr);

  PumpStats turns;
  const int iterations = PumpScriptTimersUntilQuiet(*binder, kDefaultPumpIterations, &turns);
  EXPECT_EQ(page.document()->Title(), "late");
  EXPECT_GT(iterations, 0);
  // The return value counts turns that ran work; the tracker also sees the final
  // turn that found nothing due (it is still a turn, and it is the one that
  // proves the page settled), so it is exactly one higher.
  EXPECT_EQ(turns.turns.count, static_cast<std::size_t>(iterations) + 1);
  EXPECT_EQ(turns.turns.samples_ms.size(), turns.turns.count);
  EXPECT_GE(turns.turns.longest_ms, 0);
}

// The scripts phase is split into segments because they need different fixes:
// evaluation (QuickJS), module loading, lifecycle handlers, and the cascade the
// engine re-runs afterwards.  Times cannot be gated (TESTING.md), so the counts
// are what a test asserts.
// Segment COUNTS are asserted, not durations: a `>= 0` on a value that is only
// ever accumulated is tautological, and TESTING.md forbids gating on times.
TEST(PageScriptsTest, RecordsScriptPhaseSegments)
{
  renderer::Page page;
  ScriptFetcherRecorder recorder(1);
  base::ThreadPool pool(4);
  ScriptTimings timings;
  const std::string html = "<!DOCTYPE html><html><head><title></title>"
                           "<script src=\"/a.js\"></script>"
                           "<script>document.title += '!';</script>"
                           "</head><body></body></html>";
  const std::shared_ptr<javascript::DomBinder> binder =
      RunScripts(page, html, recorder, pool, {}, &timings);
  ASSERT_NE(binder, nullptr);

  EXPECT_EQ(timings.fetches, 1u);          // the external body was waited for
  EXPECT_EQ(timings.evaluations, 2u);      // external + inline
  EXPECT_EQ(timings.event_dispatches, 2u); // DOMContentLoaded and load
  EXPECT_EQ(timings.restyles, 1u);         // the cascade after scripts
  EXPECT_EQ(timings.modules, 0u);          // classic scripts only
  // /a.js appends 'A' (see ScriptFetcherRecorder); the inline script adds '!'.
  EXPECT_EQ(page.document()->Title(), "A!");

  // The out-parameter is optional: the same page produces the same DOM without
  // it (this is the only meaningful thing the absent-timings path can assert).
  renderer::Page unmeasured;
  const std::shared_ptr<javascript::DomBinder> plain = RunScripts(unmeasured, html, recorder, pool);
  ASSERT_NE(plain, nullptr);
  EXPECT_EQ(unmeasured.document()->Title(), "A!");
}

// The module segment is ~85% of the script phase on a real site, so it must be
// covered: a module script must land in |modules| and in neither of the classic
// segments, which is exactly the boundary the split claims.
TEST(PageScriptsTest, ModuleScriptsAreNotCountedAsClassicScripts)
{
  renderer::Page page;
  ScriptFetcherRecorder recorder(1); // the module entry fetch
  base::ThreadPool pool(4);
  ScriptTimings timings;
  const std::string html = "<!DOCTYPE html><html><head><title></title>"
                           "<script type=\"module\" src=\"/m.js\"></script>"
                           "</head><body></body></html>";
  recorder.SetReply("/m.js", ScriptFetcherRecorder::Reply{true, "document.title = 'module';"});
  const std::shared_ptr<javascript::DomBinder> binder =
      RunScripts(page, html, recorder, pool, {}, &timings);
  ASSERT_NE(binder, nullptr);

  EXPECT_EQ(timings.modules, 1u);
  EXPECT_EQ(timings.evaluations, 0u);      // not a classic script
  EXPECT_EQ(timings.fetches, 0u);          // the entry fetch belongs to the module segment
  EXPECT_EQ(timings.sync_fetch_calls, 1u); // ...and is counted as a module fetch
  EXPECT_EQ(timings.event_dispatches, 2u); // still two lifecycle dispatches
  EXPECT_EQ(page.document()->Title(), "module");
  // The module segment is published as fetch + eval and must add up: the split is
  // what separates "waiting for sources" from "compiling and running them".
  EXPECT_EQ(timings.modules_ms, timings.modules_wait_ms + timings.modules_eval_ms);
}

// A module's static import is fetched by the module loader *inside* evaluation, so
// it lands in the same segment: it must raise the fetch count without inflating
// the module count (which counts <script type="module"> entries).
TEST(PageScriptsTest, ModuleImportsCountAsModuleFetches)
{
  renderer::Page page;
  ScriptFetcherRecorder recorder(2); // /m.js plus its import
  base::ThreadPool pool(4);
  ScriptTimings timings;
  recorder.SetReply("/m.js",
                    ScriptFetcherRecorder::Reply{true,
                                                 "import './dep.js';"
                                                 "document.title += 'M';"});
  recorder.SetReply("/dep.js", ScriptFetcherRecorder::Reply{true, "document.title += 'D';"});
  const std::string html = "<!DOCTYPE html><html><head><title></title>"
                           "<script type=\"module\" src=\"/m.js\"></script>"
                           "</head><body></body></html>";
  const std::shared_ptr<javascript::DomBinder> binder =
      RunScripts(page, html, recorder, pool, {}, &timings);
  ASSERT_NE(binder, nullptr);

  EXPECT_EQ(timings.modules, 1u);          // one <script type=module> entry
  EXPECT_EQ(timings.sync_fetch_calls, 2u); // the entry plus the imported module
  EXPECT_EQ(timings.modules_ms, timings.modules_wait_ms + timings.modules_eval_ms);
  EXPECT_EQ(page.document()->Title(), "DM"); // the import ran before the importer
}

// Pins the ScriptTimings -> LoadPhases mapping: five assignments of the same
// shape, so a swapped pair would otherwise go unnoticed.
TEST(PageScriptsTest, ScriptTimingsMapToDistinctPhases)
{
  ScriptTimings timings;
  timings.fetch_ms = 11;
  timings.exec_ms = 22;
  timings.modules_ms = 33;
  timings.modules_wait_ms = 34;
  timings.modules_eval_ms = 35;
  timings.events_ms = 44;
  timings.restyle_ms = 55;

  LoadPhases phases;
  ApplyScriptTimings(timings, phases);
  EXPECT_EQ(phases.scripts_fetch, 11);
  EXPECT_EQ(phases.scripts_exec, 22);
  EXPECT_EQ(phases.scripts_modules, 33);
  EXPECT_EQ(phases.scripts_modules_wait, 34);
  EXPECT_EQ(phases.scripts_modules_eval, 35);
  EXPECT_EQ(phases.scripts_events, 44);
  EXPECT_EQ(phases.scripts_restyle, 55);
  // The total is measured around RunPageScripts, not derived here.
  EXPECT_EQ(phases.scripts, 0);
}

// Without an out-param nothing is measured and behaviour is unchanged.
// The budget must not strand callbacks: PumpScriptTimersUntilQuiet keeps turning
// while a turn reports work, so a deferred batch is drained over several turns and
// the page still settles.
TEST(PageScriptsTest, RafBudgetIsDrainedByTheQuietPump)
{
  renderer::Page page;
  ScriptFetcherRecorder recorder(0);
  base::ThreadPool pool(4);
  const std::string html = "<!DOCTYPE html><html><head><title></title></head><body>"
                           "<script>for (var i = 0; i < 5; i++) {"
                           "  requestAnimationFrame(function(){ document.title += 'r'; }); }"
                           "</script></body></html>";
  const std::shared_ptr<javascript::DomBinder> binder = RunScripts(page, html, recorder, pool);
  ASSERT_NE(binder, nullptr);
  binder->SetRafPerTurnLimit(2);

  PumpStats stats;
  const int iterations = PumpScriptTimersUntilQuiet(*binder, kDefaultPumpIterations, &stats);
  EXPECT_EQ(page.document()->Title(), "rrrrr"); // every callback ran
  EXPECT_EQ(stats.breakdown.raf, 5u);
  // Five callbacks at two per turn need at least three turns of rAF work.
  EXPECT_GE(iterations, 3);
}

// A page that keeps rescheduling a timer must not block the settle pass.  Each
// turn may sleep up to the quiet budget, so the iteration cap alone does not
// bound wall-clock time: baidu.com's repeating timer made headless
// --screenshot / --dump-dom run for minutes.
TEST(PageScriptsTest, PumpStopsAtItsTotalBudget)
{
  renderer::Page page;
  ScriptFetcherRecorder recorder(0);
  base::ThreadPool pool(4);
  const std::string html = "<!DOCTYPE html><html><body><script>"
                           "function tick() { setTimeout(tick, 200); }"
                           "tick();"
                           "</script></body></html>";
  const std::shared_ptr<javascript::DomBinder> binder = RunScripts(page, html, recorder, pool);
  ASSERT_NE(binder, nullptr);

  const auto start = std::chrono::steady_clock::now();
  const int iterations =
      PumpScriptTimersUntilQuiet(*binder, 1000, nullptr, std::chrono::milliseconds(300));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  // The chain is still pending, so the pass ended on the budget, not on quiet.
  EXPECT_TRUE(binder->NextTimerDeadline().has_value());
  EXPECT_GE(iterations, 1);
  EXPECT_LT(iterations, 100);
  EXPECT_LT(elapsed, 1s);
}

// The binder outlives RunPageScripts, so apis.fetch is called again *after* the
// load -- that is how a page's own fetch()/XHR works.  Anything that callback
// touches must therefore be shared-owned: it used to capture a stack local by
// reference, and these post-load calls wrote into a dead frame (undefined
// behaviour, caught by ASan rather than by a failing assertion).
TEST(PageScriptsTest, PageCanFetchAfterTheLoad)
{
  renderer::Page page;
  ScriptFetcherRecorder recorder(0);
  base::ThreadPool pool(4);
  const std::shared_ptr<javascript::DomBinder> binder =
      RunScripts(page, ThreeScriptPage(), recorder, pool);
  ASSERT_NE(binder, nullptr);

  // The scripts phase is over; the page now issues its own request.
  const std::size_t before = recorder.fetch_count();
  const auto fetched = binder->Evaluate("fetch('/late.json').then(function(){ return 1; })");
  ASSERT_TRUE(fetched.has_value()) << fetched.error().message();
  EXPECT_EQ(recorder.fetch_count(), before + 1);
}

TEST(PageScriptsTest, InlineScriptsNeedNoFetch)
{
  renderer::Page page;
  ScriptFetcherRecorder recorder(0);
  base::ThreadPool pool(8);

  const std::string html = "<!DOCTYPE html><html><head><title></title>"
                           "<script>document.title = 'inline';</script>"
                           "</head><body></body></html>";
  const std::shared_ptr<javascript::DomBinder> binder = RunScripts(page, html, recorder, pool);
  ASSERT_NE(binder, nullptr);

  EXPECT_EQ(recorder.fetch_count(), 0u);
  EXPECT_EQ(page.document()->Title(), "inline");
}

} // namespace
} // namespace neko::browser
