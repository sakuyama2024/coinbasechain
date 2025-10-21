// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_UTIL_TIME_HPP
#define COINBASECHAIN_UTIL_TIME_HPP

#include <atomic>
#include <chrono>
#include <cstdint>

namespace coinbasechain {
namespace util {

/**
 * Mockable time system for testing
 *
 * Inspired by Bitcoin Core's time mocking system, this allows tests to
 * control time passage without waiting for real time to elapse.
 *
 * Usage:
 * - Production code calls GetTime() or GetSteadyTime() instead of direct system
 * calls
 * - Tests call SetMockTime() to control the current time
 * - When mock time is set, all time functions return the mocked value
 * - When mock time is 0 (default), time functions return real system time
 */

/**
 * Get current time as Unix timestamp (seconds since epoch)
 * Returns mock time if set, otherwise returns real system time
 */
int64_t GetTime();

/**
 * Get current time as steady clock time point
 * Returns mock time if set, otherwise returns real steady clock time
 *
 * Note: When mock time is active, steady clock is simulated using the mock
 * value
 */
std::chrono::steady_clock::time_point GetSteadyTime();


/**
 * Set mock time for testing
 *
 * @param time Unix timestamp in seconds (0 to disable mocking)
 *
 * When mock time is set to a non-zero value:
 * - All GetTime*() functions return values based on the mock time
 * - Time does not advance automatically - tests must call SetMockTime() again
 *
 * Set to 0 to return to real system time
 */
void SetMockTime(int64_t time);

/**
 * Get current mock time setting
 * Returns 0 if mock time is disabled (using real time)
 */
int64_t GetMockTime();

/**
 * RAII helper to set mock time and restore it when scope exits
 * Useful for tests that need to temporarily set mock time
 */
class MockTimeScope {
public:
  explicit MockTimeScope(int64_t time) : previous_time_(GetMockTime()) {
    SetMockTime(time);
  }

  ~MockTimeScope() { SetMockTime(previous_time_); }

private:
  int64_t previous_time_;
};

} // namespace util
} // namespace coinbasechain

#endif // COINBASECHAIN_UTIL_TIME_HPP
