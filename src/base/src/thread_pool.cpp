// neko::base::ThreadPool — a small fixed-size worker pool for CPU-bound work
// that must not block the owning thread.

#include "neko/base/thread_pool.h"

#include <thread>
#include <utility>

namespace neko::base {

std::size_t ThreadPool::HardwareConcurrency()
{
  const unsigned int n = std::thread::hardware_concurrency();
  return n > 0 ? static_cast<std::size_t>(n) : 1U;
}

ThreadPool::ThreadPool(std::size_t num_threads)
{
  if (num_threads == 0) {
    num_threads = 1;
  }
  workers_.reserve(num_threads);
  for (std::size_t i = 0; i < num_threads; ++i) {
    workers_.emplace_back([this] { WorkerLoop(); });
  }
}

ThreadPool::~ThreadPool()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  cv_.notify_all();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

void ThreadPool::Post(std::function<void()> task)
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.push(std::move(task));
  }
  cv_.notify_one();
}

std::size_t ThreadPool::PendingCount() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return tasks_.size();
}

void ThreadPool::WaitIdle()
{
  std::unique_lock<std::mutex> lock(mutex_);
  idle_cv_.wait(lock, [this] { return tasks_.empty() && active_ == 0; });
}

void ThreadPool::WorkerLoop()
{
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      if (stopping_ && tasks_.empty()) {
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop();
      ++active_;
    }
    // Run outside the lock: tasks may post new tasks or block.
    task();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --active_;
      if (tasks_.empty() && active_ == 0) {
        idle_cv_.notify_all();
      }
    }
  }
}

} // namespace neko::base
