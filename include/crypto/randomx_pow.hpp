// Copyright (c) 2024 Coinbase Chain
// Based on Bitcoin Consensus implementation
// Distributed under the MIT software license

#ifndef COINBASECHAIN_CRYPTO_RANDOMX_POW_HPP
#define COINBASECHAIN_CRYPTO_RANDOMX_POW_HPP

#include "primitives/block.h"
#include <randomx.h>
#include <cstdint>
#include <memory>
#include <mutex>

namespace coinbasechain {
namespace crypto {

// Forward declarations
class ChainParams;
namespace chain { struct ConsensusParams; }

/**
 * RandomX Proof-of-Work Implementation
 *
 * Based on SCASH's implementation with VM caching for performance.
 * RandomX VMs are expensive to create (~1 second for light mode), so we
 * cache them per epoch.
 */

/** POW verification modes */
enum class POWVerifyMode {
    FULL = 0,              // Verify both RandomX hash and commitment
    COMMITMENT_ONLY,       // Only verify commitment (faster, for header sync)
    MINING                 // Calculate hash and commitment (for miners)
};

/** Forward declarations for internal structures */
struct RandomXCacheWrapper;

/**
 * RandomX VM Wrapper - Manages VM lifecycle and thread-safety
 *
 * VMs are cached and shared between threads for efficiency.
 * The hashing_mutex MUST be locked when calling randomx_calculate_hash.
 */
struct RandomXVMWrapper {
    randomx_vm* vm = nullptr;
    std::shared_ptr<RandomXCacheWrapper> cache;
    mutable std::mutex hashing_mutex;  // Protects concurrent hashing on shared VM

    RandomXVMWrapper(randomx_vm* v, std::shared_ptr<RandomXCacheWrapper> c)
        : vm(v), cache(c) {}

    ~RandomXVMWrapper() {
        if (vm) {
            randomx_destroy_vm(vm);
            cache = nullptr;
        }
    }
};

/** Faster RandomX computation but requires more memory */
static constexpr bool DEFAULT_RANDOMX_FAST_MODE = false;

/** Number of epochs to cache. There is one VM per epoch. Minimum is 1. */
static constexpr int DEFAULT_RANDOMX_VM_CACHE_SIZE = 2;

/**
 * Calculate epoch from timestamp
 * Epoch = timestamp / duration (in seconds)
 */
uint32_t GetEpoch(uint32_t nTime, uint32_t nDuration);

/**
 * TODO check Alpha
 * Calculate RandomX key (seed hash) for a given epoch
 * Uses SHA256d("CoinbaseChain/RandomX/Epoch/N")
 */
uint256 GetSeedHash(uint32_t nEpoch);

/**
 * Calculate RandomX commitment from block header
 * Commitment = RandomX_Commitment(block_header_without_hashRandomX, hashRandomX)
 *
 * @param block Block header
 * @param inHash Optional pre-computed RandomX hash (if nullptr, uses block.hashRandomX)
 * @return Commitment hash
 */
uint256 GetRandomXCommitment(const CBlockHeader& block, uint256* inHash = nullptr);

/**
 * Initialize RandomX subsystem
 * Should be called once at startup
 * TODO
 *
 * @param vmCacheSize Number of epochs to cache (default: 2)
 * @param fastMode Use fast mode after IBD (default: false)
 */
void InitRandomX(int vmCacheSize = DEFAULT_RANDOMX_VM_CACHE_SIZE,
                 bool fastMode = DEFAULT_RANDOMX_FAST_MODE);

/**
 * Shutdown RandomX subsystem
 * Releases all VMs and caches
 */
void ShutdownRandomX();

/**
 * Create a RandomX VM for an epoch (for parallel verification)
 * Multiple VMs can be created from the same cache for parallel execution
 *
 * @param nEpoch Epoch number
 * @return RandomX VM pointer (caller must destroy with randomx_destroy_vm)
 */
randomx_vm* CreateVMForEpoch(uint32_t nEpoch);

/**
 * Get cached RandomX VM for an epoch (for efficient use)
 * Returns a shared VM from the cache. Multiple callers may get the same VM.
 * IMPORTANT: Must lock vmRef->hashing_mutex before calling randomx_calculate_hash
 *
 * @param nEpoch Epoch number
 * @return Shared pointer to VM wrapper (contains VM and hashing mutex)
 */
std::shared_ptr<RandomXVMWrapper> GetCachedVM(uint32_t nEpoch);

} // namespace crypto
} // namespace coinbasechain

#endif // COINBASECHAIN_CRYPTO_RANDOMX_POW_HPP
