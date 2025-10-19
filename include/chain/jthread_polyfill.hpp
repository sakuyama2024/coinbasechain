#ifndef COINBASECHAIN_JTHREAD_POLYFILL_HPP
#define COINBASECHAIN_JTHREAD_POLYFILL_HPP

#include <atomic>
#include <memory>
#include <thread>

// Check if std::jthread is available
#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
// Use native std::jthread
namespace coinbasechain {
using jthread = std::jthread;
using stop_token = std::stop_token;
using stop_source = std::stop_source;
} // namespace coinbasechain
#else
// Polyfill for std::stop_token, std::stop_source, and std::jthread
namespace coinbasechain {

// Simple stop_source implementation
class stop_source {
public:
  stop_source() : stopped_(std::make_shared<std::atomic<bool>>(false)) {}

  void request_stop() noexcept {
    if (stopped_) {
      stopped_->store(true, std::memory_order_release);
    }
  }

  [[nodiscard]] bool stop_requested() const noexcept {
    return stopped_ && stopped_->load(std::memory_order_acquire);
  }

private:
  friend class stop_token;
  friend class jthread;
  std::shared_ptr<std::atomic<bool>> stopped_;
};

// Simple stop_token implementation
class stop_token {
public:
  stop_token() = default;

  explicit stop_token(std::shared_ptr<std::atomic<bool>> stopped)
      : stopped_(std::move(stopped)) {}

  // Copyable
  stop_token(const stop_token &) = default;
  stop_token &operator=(const stop_token &) = default;

  // Movable
  stop_token(stop_token &&) noexcept = default;
  stop_token &operator=(stop_token &&) noexcept = default;

  [[nodiscard]] bool stop_requested() const noexcept {
    return stopped_ && stopped_->load(std::memory_order_acquire);
  }

private:
  friend class stop_source;
  std::shared_ptr<std::atomic<bool>> stopped_;
};

// Simple jthread implementation
class jthread {
public:
  jthread() noexcept = default;

  template <typename F, typename... Args>
  explicit jthread(F &&f, Args &&...args) {
    stop_src_ = std::make_shared<stop_source>();

    // Always wrap in a lambda that provides stop_token
    thread_ = std::thread([f = std::forward<F>(f), stop_src = stop_src_,
                           ... args = std::forward<Args>(args)]() mutable {
      stop_token token(stop_src->stopped_);
      // Try to call with stop_token first, otherwise without
      if constexpr (std::is_invocable_v<F, stop_token, Args...>) {
        f(token, std::forward<Args>(args)...);
      } else {
        f(std::forward<Args>(args)...);
      }
    });
  }

  jthread(jthread &&) noexcept = default;
  jthread &operator=(jthread &&) noexcept = default;

  jthread(const jthread &) = delete;
  jthread &operator=(const jthread &) = delete;

  ~jthread() {
    if (joinable()) {
      request_stop();
      join();
    }
  }

  void join() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  bool joinable() const noexcept { return thread_.joinable(); }

  void request_stop() noexcept {
    if (stop_src_) {
      stop_src_->request_stop();
    }
  }

  [[nodiscard]] stop_token get_stop_token() const noexcept {
    return stop_src_ ? stop_token(stop_src_->stopped_) : stop_token{};
  }

  [[nodiscard]] stop_source &get_stop_source() noexcept {
    if (!stop_src_) {
      stop_src_ = std::make_shared<stop_source>();
    }
    return *stop_src_;
  }

  [[nodiscard]] std::thread::id get_id() const noexcept {
    return thread_.get_id();
  }

private:
  std::shared_ptr<stop_source> stop_src_;
  std::thread thread_;
};
} // namespace coinbasechain
#endif

#endif // COINBASECHAIN_JTHREAD_POLYFILL_HPP
