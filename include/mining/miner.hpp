// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_MINING_MINER_HPP
#define COINBASECHAIN_MINING_MINER_HPP

#include "chain/chainparams.hpp"
#include "primitives/block.h"
#include "uint.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace coinbasechain {

// Forward declarations
namespace chain {
class BlockManager;
class CBlockIndex;
} // namespace chain

namespace validation {
class ChainstateManager;
}

namespace mining {

// Block template - header ready for mining
struct BlockTemplate {
  CBlockHeader header;   // Block header to mine
  uint32_t nBits;        // Difficulty target
  int nHeight;           // Block height
  uint256 hashPrevBlock; // Previous block hash
};

// CPU Miner - Single-threaded RandomX mining for regtest
// Atomics for safe RPC access, designed for regtest/testing only
class CPUMiner {
public:
  CPUMiner(const chain::ChainParams &params,
           validation::ChainstateManager &chainstate);
  ~CPUMiner();

  // Start mining (num_threads ignored, always uses 1 thread for regtest)
  bool Start(int num_threads = 1);
  void Stop();

  bool IsMining() const { return mining_.load(); }
  double GetHashrate() const;
  uint64_t GetTotalHashes() const { return total_hashes_.load(); }
  int GetBlocksFound() const { return blocks_found_.load(); }

private:
  void MiningWorker();
  BlockTemplate CreateBlockTemplate();
  bool ShouldRegenerateTemplate(); // e.g., chain tip changed

private:
  // Chain params
  const chain::ChainParams &params_;
  validation::ChainstateManager &chainstate_;

  // Mining state (atomics for RPC thread safety)
  std::atomic<bool> mining_{false};
  std::atomic<uint64_t> total_hashes_{0};
  std::atomic<int> blocks_found_{0};

  // Current template
  BlockTemplate current_template_;
  uint256 template_prev_hash_;
  std::chrono::steady_clock::time_point start_time_;

  // Mining thread
  std::thread worker_;
};

} // namespace mining
} // namespace coinbasechain

#endif // COINBASECHAIN_MINING_MINER_HPP
