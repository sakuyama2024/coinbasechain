// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_MINING_MINER_HPP
#define COINBASECHAIN_MINING_MINER_HPP

#include "primitives/block.h"
#include "chain/chainparams.hpp"
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
}

namespace validation {
    class ChainstateManager;
}

namespace mining {

/**
 * Block template - header ready for mining
 */
struct BlockTemplate {
    CBlockHeader header;          // Block header to mine
    uint32_t nBits;              // Difficulty target
    int nHeight;                 // Block height
    uint256 hashPrevBlock;       // Previous block hash
};

/**
 * CPU Miner - Single-threaded RandomX mining for regtest
 *
 * Simplified single-threaded implementation:
 * - Single mining thread for simplicity
 * - Atomic operations for safe state access from RPC
 * - Designed for regtest/testing only
 */
class CPUMiner {
public:
    /**
     * Constructor
     * @param params Chain parameters
     * @param chainstate Chainstate manager for processing found blocks
     */
    CPUMiner(const chain::ChainParams& params, validation::ChainstateManager& chainstate);
    ~CPUMiner();

    /**
     * Start mining (single-threaded)
     * @param num_threads Ignored - always uses 1 thread for regtest
     * @return true if started successfully
     */
    bool Start(int num_threads = 1);

    /**
     * Stop mining
     */
    void Stop();

    /**
     * Check if currently mining
     */
    bool IsMining() const { return mining_.load(); }

    /**
     * Get current hashrate (hashes per second)
     */
    double GetHashrate() const;

    /**
     * Get total hashes computed
     */
    uint64_t GetTotalHashes() const { return total_hashes_.load(); }

    /**
     * Get number of blocks found
     */
    int GetBlocksFound() const { return blocks_found_.load(); }

private:
    /**
     * Mining worker function (runs in single thread)
     */
    void MiningWorker();

    /**
     * Create block template from current chain tip
     * @return Block template ready for mining
     */
    BlockTemplate CreateBlockTemplate();

    /**
     * Check if we should regenerate block template
     * (e.g., chain tip changed)
     */
    bool ShouldRegenerateTemplate();

private:
    // Chain params
    const chain::ChainParams& params_;
    validation::ChainstateManager& chainstate_;

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
