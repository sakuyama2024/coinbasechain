// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license
// Based on Bitcoin Core's timedata implementation

#include "chain/timedata.hpp"
#include "chain/logging.hpp"
#include <ctime>
#include <mutex>
#include <set>

namespace coinbasechain {
namespace util {

// Maximum number of time samples to track from peers
static constexpr size_t MAX_TIME_SAMPLES = 200;

// Global state (thread-safe via mutex)
static std::mutex g_timeoffset_mutex;
static int64_t nTimeOffset = 0; // Current network time offset in seconds
static std::set<std::string> g_sources; // Track unique peer addresses
static CMedianFilter<int64_t> g_time_offsets{MAX_TIME_SAMPLES, 0};
static bool g_warning_emitted = false; // Only warn once about clock issues

/**
 * "Never go to sea with two chronometers; take one or three."
 * Our three time sources are:
 *  - System clock
 *  - Median of other nodes clocks
 *  - The user (asking the user to fix the system clock if the first two
 * disagree)
 */
int64_t GetTimeOffset() {
  std::lock_guard<std::mutex> lock(g_timeoffset_mutex);
  return nTimeOffset;
}

void AddTimeData(const std::string &peer_addr, int64_t nOffsetSample) {
  std::lock_guard<std::mutex> lock(g_timeoffset_mutex);

  LOG_CHAIN_TRACE("AddTimeData: peer={} offset={:+d}s sources={}/{}",
                  peer_addr, nOffsetSample, g_sources.size(), MAX_TIME_SAMPLES);

  // Ignore duplicates (only accept one sample per peer)
  if (g_sources.size() == MAX_TIME_SAMPLES) {
    LOG_CHAIN_TRACE("AddTimeData: Ignoring (max samples reached)");
    return;
  }
  if (!g_sources.insert(peer_addr).second) {
    LOG_CHAIN_TRACE("AddTimeData: Ignoring (duplicate peer)");
    return;
  }

  // Add data to median filter
  g_time_offsets.input(nOffsetSample);
  LOG_CHAIN_TRACE("Added time data from peer {}: offset={:+d}s ({:+d} minutes), "
            "total samples={}",
            peer_addr, nOffsetSample, nOffsetSample / 60,
            g_time_offsets.size());

  // There is a known issue here (from Bitcoin Core issue #4521):
  //
  // - The structure g_time_offsets contains up to 200 elements, after which
  // any new element added to it will not increase its size, replacing the
  // oldest element.
  //
  // - The condition to update nTimeOffset includes checking whether the
  // number of elements in g_time_offsets is odd, which will never happen after
  // there are 200 elements.
  //
  // But in this case the 'bug' is protective against some attacks, and may
  // actually explain why we've never seen attacks which manipulate the
  // clock offset.
  //
  // So we should hold off on fixing this and clean it up as part of
  // a timing cleanup that strengthens it in a number of other ways.
  //
  // Require at least 5 samples and an odd number of samples to update offset
  if (g_time_offsets.size() >= 5 && g_time_offsets.size() % 2 == 1) {
    int64_t nMedian = g_time_offsets.median();
    std::vector<int64_t> vSorted = g_time_offsets.sorted();

    LOG_CHAIN_TRACE("AddTimeData: Evaluating median offset: median={:+d}s samples={}",
                    nMedian, g_time_offsets.size());

    // Only let other nodes change our time by so much (default ±70 minutes)
    // This protects against eclipse attacks where attacker controls all our
    // peers
    int64_t max_adjustment = DEFAULT_MAX_TIME_ADJUSTMENT;

    if (nMedian >= -max_adjustment && nMedian <= max_adjustment) {
      int64_t oldOffset = nTimeOffset;
      nTimeOffset = nMedian;
      LOG_CHAIN_TRACE("AddTimeData: Time offset adjusted: {:+d}s -> {:+d}s",
                      oldOffset, nTimeOffset);
      LOG_CHAIN_TRACE("Network time offset updated: {:+d}s ({:+d} minutes) based on "
               "{} samples",
               nTimeOffset, nTimeOffset / 60, g_time_offsets.size());
    } else {
      // Median offset exceeds max adjustment - don't adjust time
      LOG_CHAIN_TRACE("AddTimeData: Median {:+d}s exceeds max adjustment ±{:+d}s, rejecting",
                      nMedian, max_adjustment);
      nTimeOffset = 0;

      if (!g_warning_emitted) {
        // If nobody has a time different than ours but within 5 minutes of
        // ours, give a warning
        bool fMatch = false;
        for (const int64_t nOffset : vSorted) {
          if (nOffset != 0 && nOffset > -5 * 60 && nOffset < 5 * 60) {
            fMatch = true;
            break;
          }
        }

        if (!fMatch) {
          g_warning_emitted = true;
          LOG_CHAIN_ERROR(
              "WARNING: Please check that your computer's date and time are "
              "correct! "
              "If your clock is wrong, Coinbase Chain will not work properly.");
          LOG_CHAIN_ERROR("Your clock differs from network time by more than {:+d} "
                    "minutes (max adjustment). "
                    "Median network offset: {:+d}s",
                    max_adjustment / 60, nMedian);
        }
      }
    }

    // Debug logging of all time samples
    if (g_time_offsets.size() >= 5) {
      std::string log_message = "Time data samples: ";
      for (const int64_t n : vSorted) {
        log_message += std::to_string(n) + "s  ";
      }
      log_message += "| median offset = " + std::to_string(nTimeOffset) +
                     "s (" + std::to_string(nTimeOffset / 60) + " minutes)";
      LOG_CHAIN_TRACE("{}", log_message);
    }
  }
}

void TestOnlyResetTimeData() {
  std::lock_guard<std::mutex> lock(g_timeoffset_mutex);
  nTimeOffset = 0;
  g_sources.clear();
  g_time_offsets = CMedianFilter<int64_t>{MAX_TIME_SAMPLES, 0};
  g_warning_emitted = false;
}

} // namespace util
} // namespace coinbasechain
