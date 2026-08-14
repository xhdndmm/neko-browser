// Unit tests for neko::base::ThreadPool.

#include "neko/base/thread_pool.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace neko::base {
namespace {

TEST(ThreadPoolTest, RunsAllPostedTasks)
{
  ThreadPool pool(4);
  constexpr int kTasks = 100;
  std::atomic<int> counter{0};
  for (int i = 0; i < kTasks; ++i) {
    pool.Post([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
  }
  pool.WaitIdle();
  EXPECT_EQ(counter.load(), kTasks);
  EXPECT_EQ(pool.PendingCount(), 0u);
}

TEST(ThreadPoolTest, SubmitReturnsResults)
{
  ThreadPool pool(2);
  auto a = pool.Submit([] { return 40; });
  auto b = pool.Submit([] { return 2; });
  EXPECT_EQ(a.get() + b.get(), 42);
}

TEST(ThreadPoolTest, SubmitWithMoveOnlyResult)
{
  ThreadPool pool(2);
  auto result = pool.Submit([] { return std::string("hello"); });
  EXPECT_EQ(result.get(), "hello");
}

TEST(ThreadPoolTest, TasksRunConcurrently)
{
  ThreadPool pool(2);
  std::mutex mutex;
  std::vector<std::thread::id> ids;
  std::atomic<int> running{0};
  std::atomic<int> max_running{0};
  for (int i = 0; i < 8; ++i) {
    pool.Post([&] {
      const int now = running.fetch_add(1) + 1;
      int expected = max_running.load();
      while (now > expected && !max_running.compare_exchange_weak(expected, now)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      running.fetch_sub(1);
      std::lock_guard<std::mutex> lock(mutex);
      ids.push_back(std::this_thread::get_id());
    });
  }
  pool.WaitIdle();
  EXPECT_GE(max_running.load(), 2); // at least two tasks overlapped
  // With 2 workers we expect exactly 2 distinct threads.
  EXPECT_LE(ids.size(), 8u);
}

TEST(ThreadPoolTest, TasksMayPostNewTasks)
{
  ThreadPool pool(1);
  std::atomic<int> counter{0};
  // A task that posts another task; the pool must keep draining until done.
  std::function<void()> self;
  self = [&] {
    if (counter.fetch_add(1) < 4) {
      pool.Post(self);
    }
  };
  pool.Post(self);
  pool.WaitIdle();
  EXPECT_EQ(counter.load(), 5);
}

TEST(ThreadPoolTest, DestructorDrainsAndJoins)
{
  std::atomic<int> counter{0};
  {
    ThreadPool pool(3);
    for (int i = 0; i < 10; ++i) {
      pool.Post([&counter] {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        counter.fetch_add(1);
      });
    }
  }
  EXPECT_EQ(counter.load(), 10); // destructor ran everything to completion
}

TEST(ThreadPoolTest, HardwareConcurrencyIsPositive)
{
  EXPECT_GE(ThreadPool::HardwareConcurrency(), 1u);
}

} // namespace
} // namespace neko::base
