// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license
// Based on Bitcoin Core's timedata implementation

#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace coinbasechain {
namespace util {

// Maximum time adjustment from network peers (±70 minutes)
static constexpr int64_t DEFAULT_MAX_TIME_ADJUSTMENT = 70 * 60; // seconds

/**
 * Median filter over a stream of values.
 * Returns the median of the last N numbers.
 *
 * This is used to track time offsets from network peers and calculate
 * a median offset to adjust local system time.
 */
template <typename T> class CMedianFilter {
private:
  std::vector<T> vValues;
  std::vector<T> vSorted;
  unsigned int nSize;

public:
  CMedianFilter(unsigned int _size, T initial_value) : nSize(_size) {
    vValues.reserve(_size);
    vValues.push_back(initial_value);
    vSorted = vValues;
  }

  void input(T value) {
    if (vValues.size() == nSize) {
      vValues.erase(vValues.begin());
    }
    vValues.push_back(value);

    vSorted.resize(vValues.size());
    std::copy(vValues.begin(), vValues.end(), vSorted.begin());
    std::sort(vSorted.begin(), vSorted.end());
  }

  T median() const {
    int vSortedSize = vSorted.size();
    assert(vSortedSize > 0);
    if (vSortedSize & 1) // Odd number of elements
    {
      return vSorted[vSortedSize / 2];
    } else // Even number of elements
    {
      return (vSorted[vSortedSize / 2 - 1] + vSorted[vSortedSize / 2]) / 2;
    }
  }

  int size() const { return vValues.size(); }

  const std::vector<T>& sorted() const { return vSorted; }
};

/**
 * Get the current time offset from network peers (in seconds).
 * This offset is added to system time to get network-adjusted time.
 *
 * @return Time offset in seconds (can be negative)
 */
int64_t GetTimeOffset();

/**
 * Add a time sample from a network peer.
 *
 * When we receive a version message from a peer, they send us their timestamp.
 * We calculate the offset: peer_time - our_time, and feed it into a median
 * filter. Once we have at least 5 samples, we use the median offset (capped to
 * ±70 minutes).
 *
 * @param peer_addr String representation of peer address (for deduplication)
 * @param nOffsetSample Time offset sample: peer_time - system_time (in seconds)
 */
void AddTimeData(const std::string &peer_addr, int64_t nOffsetSample);

/**
 * Reset time data state (for testing only).
 */
void TestOnlyResetTimeData();

} // namespace util
} // namespace coinbasechain


