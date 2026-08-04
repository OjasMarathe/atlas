#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace atlas {

// Fixed-size worker pool over a shared task queue.
//
// Why a pool at all: thread creation costs on the order of tens of microseconds and a stack
// allocation, so spawning one per unit of work loses to the work itself once tasks are short.
// A pool pays that once and amortizes it, and — the part that actually matters in a server —
// it *bounds* concurrency. An unbounded thread-per-task server degrades under load instead of
// queueing, which is the difference between slow and down.
//
// Note on scope: gRPC's synchronous server has its own thread pool, so Atlas does not use this
// one to serve RPCs — that would be two schedulers competing. It is used where we own the
// parallelism: fanning indexing work out across shards, and driving N concurrent clients in the
// load test. See docs/concepts/thread-pool.md.
class ThreadPool {
 public:
  explicit ThreadPool(std::size_t threads = 0) {
    if (threads == 0) {
      const unsigned hardware = std::thread::hardware_concurrency();
      threads = hardware == 0 ? 4 : hardware;
    }
    workers_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
      workers_.emplace_back([this] { Run(); });
    }
  }

  ~ThreadPool() { Shutdown(); }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Queues `fn(args...)` and hands back a future for its result. The future is how exceptions
  // get back to the caller too: a task that throws on a worker thread would otherwise call
  // std::terminate, since an exception escaping a thread's entry function is fatal.
  template <typename Fn, typename... Args>
  auto Submit(Fn&& fn, Args&&... args) -> std::future<std::invoke_result_t<Fn, Args...>> {
    using Result = std::invoke_result_t<Fn, Args...>;

    auto task = std::make_shared<std::packaged_task<Result()>>(
        [fn = std::forward<Fn>(fn),
         tuple = std::make_tuple(std::forward<Args>(args)...)]() mutable -> Result {
          return std::apply(std::move(fn), std::move(tuple));
        });
    std::future<Result> future = task->get_future();

    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) throw std::runtime_error("ThreadPool::Submit after shutdown");
      tasks_.emplace([task] { (*task)(); });
    }
    work_available_.notify_one();
    return future;
  }

  // Blocks until every queued and running task has finished. Does not stop the pool.
  void WaitIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return tasks_.empty() && active_ == 0; });
  }

  // Lets running tasks finish, drains the queue, then joins. Idempotent.
  void Shutdown() {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) return;
      stopping_ = true;
    }
    work_available_.notify_all();
    for (std::thread& worker : workers_) {
      if (worker.joinable()) worker.join();
    }
    workers_.clear();
  }

  std::size_t size() const { return workers_.size(); }

  std::size_t pending() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
  }

 private:
  void Run() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        work_available_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        // Drain before exiting: a queued task holds a promise, and dropping it would break
        // every caller still waiting on the matching future.
        if (tasks_.empty()) return;
        task = std::move(tasks_.front());
        tasks_.pop();
        ++active_;
      }
      task();
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        --active_;
        if (tasks_.empty() && active_ == 0) idle_.notify_all();
      }
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable work_available_;
  std::condition_variable idle_;
  std::queue<std::function<void()>> tasks_;
  std::vector<std::thread> workers_;
  std::size_t active_ = 0;
  bool stopping_ = false;
};

}  // namespace atlas
