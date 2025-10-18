#ifndef COINBASECHAIN_THREADPOOL_HPP
#define COINBASECHAIN_THREADPOOL_HPP

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

namespace coinbasechain {
namespace util {

/**
 * Simple thread pool for parallel task execution
 *
 * Usage:
 *   ThreadPool pool(4);  // 4 worker threads
 *   auto future = pool.enqueue([](){ return 42; });
 *   int result = future.get();
 */
class ThreadPool {
public:
  /**
   * Constructor - create pool with specified number of threads
   * If num_threads == 0, uses hardware concurrency
   */
  explicit ThreadPool(size_t num_threads = 0);

  /**
   * Destructor - waits for all tasks to complete
   */
  ~ThreadPool();

  /**
   * Enqueue a task for execution
   * Returns a future that will contain the result
   */
  template <class F, class... Args>
  auto enqueue(F &&f, Args &&...args)
      -> std::future<typename std::invoke_result<F, Args...>::type>;

  /**
   * Get number of worker threads
   */
  size_t size() const { return workers_.size(); }

private:
  // Worker threads
  std::vector<std::thread> workers_;

  // Task queue
  std::queue<std::function<void()>> tasks_;

  // Synchronization
  std::mutex queue_mutex_;
  std::condition_variable condition_;
  bool stop_;
};

// Implementation of enqueue (must be in header for template)
template <class F, class... Args>
auto ThreadPool::enqueue(F &&f, Args &&...args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {
  using return_type = typename std::invoke_result<F, Args...>::type;

  auto task = std::make_shared<std::packaged_task<return_type()>>(
      std::bind(std::forward<F>(f), std::forward<Args>(args)...));

  std::future<return_type> res = task->get_future();
  {
    std::unique_lock<std::mutex> lock(queue_mutex_);

    // Don't allow enqueueing after stopping the pool
    if (stop_)
      throw std::runtime_error("enqueue on stopped ThreadPool");

    tasks_.emplace([task]() { (*task)(); });
  }
  condition_.notify_one();
  return res;
}

} // namespace util
} // namespace coinbasechain

#endif // COINBASECHAIN_THREADPOOL_HPP
