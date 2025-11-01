// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "util/time.hpp"
#include <atomic>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace coinbasechain {
namespace util {

// Static storage for mock time
// 0 means mock time is disabled (use real time)
static std::atomic<int64_t> g_mock_time{0};

// Store the steady clock reference point when mock time is first set
// This allows us to simulate steady_clock behavior with mock time
static std::atomic<int64_t> g_mock_steady_reference{0};
static std::chrono::steady_clock::time_point g_real_steady_reference;
static std::atomic<bool> g_steady_initialized{false};

int64_t GetTime() {
  int64_t mock = g_mock_time.load(std::memory_order_relaxed);
  if (mock != 0) {
    return mock;
  }

  // Return real system time
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::chrono::steady_clock::time_point GetSteadyTime() {
  int64_t mock = g_mock_time.load(std::memory_order_relaxed);
  if (mock != 0) {
    // When mock time is active, simulate steady clock
    // Maintain the property that steady_clock advances monotonically

    if (!g_steady_initialized.load(std::memory_order_acquire)) {
      // First time using mock steady time - set reference point
      bool expected = false;
      if (g_steady_initialized.compare_exchange_strong(expected, true)) {
        g_real_steady_reference = std::chrono::steady_clock::now();
        g_mock_steady_reference.store(mock, std::memory_order_release);
      }
    }

    // Calculate offset from reference point
    int64_t mock_ref = g_mock_steady_reference.load(std::memory_order_relaxed);
    int64_t seconds_offset = mock - mock_ref;

    // Add offset to real reference point
    return g_real_steady_reference + std::chrono::seconds(seconds_offset);
  }

  // Return real steady clock time
  return std::chrono::steady_clock::now();
}

void SetMockTime(int64_t time) {
  g_mock_time.store(time, std::memory_order_relaxed);

  // Reset steady clock reference when mock time is changed
  if (time != 0) {
    g_steady_initialized.store(false, std::memory_order_release);
  }
}

int64_t GetMockTime() { return g_mock_time.load(std::memory_order_relaxed); }

std::string FormatTime(uint32_t unix_time) {
  if (unix_time == 0) {
    return "1970-01-01 00:00:00 UTC";
  }

  std::time_t t = static_cast<std::time_t>(unix_time);
  std::tm* tm_utc = std::gmtime(&t);

  if (!tm_utc) {
    return "invalid";
  }

  std::ostringstream oss;
  oss << std::put_time(tm_utc, "%Y-%m-%d %H:%M:%S UTC");
  return oss.str();
}

} // namespace util
} // namespace coinbasechain
