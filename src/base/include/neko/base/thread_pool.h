#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace neko::base {

// A fixed-size pool of worker threads executing queued tasks concurrently.
//
// This is the project's threading primitive for CPU-bound work that must not
// block the owning thread (e.g. decoding images, parsing subresources).
//
// Threading contract:
//   * Post/Submit may be called from any thread at any time.
//   * Tasks must not rely on ordering guarantees (FIFO within the queue is
//     best-effort; completion order is unspecified).
//   * The destructor stops the pool: it drains the queue (running every
//     queued task to completion) and joins the workers.
//   * Tasks must be self-contained; the pool does not manage task lifetimes
//     beyond the callable itself.
class ThreadPool
{
public:
  // A sensible default: one worker per hardware thread.
  static std::size_t HardwareConcurrency();

  explicit ThreadPool(std::size_t num_threads = HardwareConcurrency());
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Enqueues a task (fire-and-forget).  Safe from any thread.
  void Post(std::function<void()> task);

  // Enqueues a task and returns a future holding its result.  Safe from any
  // thread.  R must be a movable/copyable return type.
  template <typename F, typename R = std::invoke_result_t<F>> std::future<R> Submit(F&& fn);

  // Number of worker threads.
  std::size_t thread_count() const
  {
    return workers_.size();
  }

  // Number of tasks queued but not yet started (may be stale by the time it
  // is read).
  std::size_t PendingCount() const;

  // Blocks the calling thread until every task posted so far has completed.
  // Does not prevent new posts; a task posted while waiting may keep the wait
  // alive.
  void WaitIdle();

private:
  void WorkerLoop();

  std::vector<std::thread> workers_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> tasks_;
  std::size_t active_ = 0;
  std::condition_variable idle_cv_;
  bool stopping_ = false;
};

template <typename F, typename R> std::future<R> ThreadPool::Submit(F&& fn)
{
  auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
  std::future<R> result = task->get_future();
  Post([task] { (*task)(); });
  return result;
}

} // namespace neko::base
