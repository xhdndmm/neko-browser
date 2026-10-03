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
      [&recorder](const url::Url& url) { return recorder.Fetch(url); },
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

  LatencyTracker turns;
  const int iterations = PumpScriptTimersUntilQuiet(*binder, kDefaultPumpIterations, &turns);
  EXPECT_EQ(page.document()->Title(), "late");
  EXPECT_GT(iterations, 0);
  // The return value counts turns that ran work; the tracker also sees the final
  // turn that found nothing due (it is still a turn, and it is the one that
  // proves the page settled), so it is exactly one higher.
  EXPECT_EQ(turns.count, static_cast<std::size_t>(iterations) + 1);
  EXPECT_EQ(turns.samples_ms.size(), turns.count);
  EXPECT_GE(turns.longest_ms, 0);
}

// The scripts phase is split into segments because they need different fixes:
// evaluation (QuickJS), module loading, lifecycle handlers, and the cascade the
// engine re-runs afterwards.  Times cannot be gated (TESTING.md), so the counts
// are what a test asserts.
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

  EXPECT_EQ(timings.fetches, 1u);     // the external body was waited for
  EXPECT_EQ(timings.evaluations, 2u); // external + inline
  EXPECT_EQ(timings.events, 1u);      // one DOMContentLoaded/load dispatch pair
  EXPECT_EQ(timings.restyles, 1u);    // the cascade after scripts
  EXPECT_GE(timings.fetch_ms, 0);
  EXPECT_GE(timings.exec_ms, 0);
  EXPECT_GE(timings.restyle_ms, 0);
}

// Without an out-param nothing is measured and behaviour is unchanged.
TEST(PageScriptsTest, RecordsNothingWhenTimingsAreNotRequested)
{
  renderer::Page page;
  ScriptFetcherRecorder recorder(0);
  base::ThreadPool pool(4);
  const std::shared_ptr<javascript::DomBinder> binder =
      RunScripts(page, ThreeScriptPage(), recorder, pool);
  ASSERT_NE(binder, nullptr);
  EXPECT_EQ(page.document()->Title(), "ABC");
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
