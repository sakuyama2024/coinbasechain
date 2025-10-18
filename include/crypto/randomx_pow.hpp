// Copyright (c) 2024 Coinbase Chain
// Based on Bitcoin Consensus implementation
// Distributed under the MIT software license

#ifndef COINBASECHAIN_CRYPTO_RANDOMX_POW_HPP
#define COINBASECHAIN_CRYPTO_RANDOMX_POW_HPP

#include "primitives/block.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <randomx.h>

namespace coinbasechain {
namespace crypto {

// Forward declarations
class ChainParams;
namespace chain {
struct ConsensusParams;
}

// RandomX Proof-of-Work Implementation (based on SCASH with VM caching)
// VMs are expensive to create (~1s light mode), cached per epoch

// POW verification modes
enum class POWVerifyMode {
  FULL = 0,        // Verify both RandomX hash and commitment
  COMMITMENT_ONLY, // Only verify commitment (faster, for header sync)
  MINING           // Calculate hash and commitment (for miners)
};

struct RandomXCacheWrapper;

// RandomX VM Wrapper - Manages VM lifecycle and thread-safety
// VMs are cached/shared between threads - MUST lock hashing_mutex when calling
// randomx_calculate_hash
struct RandomXVMWrapper {
  randomx_vm *vm = nullptr;
  std::shared_ptr<RandomXCacheWrapper> cache;
  mutable std::mutex hashing_mutex; // Protects concurrent hashing on shared VM

  RandomXVMWrapper(randomx_vm *v, std::shared_ptr<RandomXCacheWrapper> c)
      : vm(v), cache(c) {}

  ~RandomXVMWrapper() {
    if (vm) {
      randomx_destroy_vm(vm);
      cache = nullptr;
    }
  }
};

// Faster RandomX computation but requires more memory
static constexpr bool DEFAULT_RANDOMX_FAST_MODE = false;

// Number of epochs to cache (one VM per epoch, minimum 1)
static constexpr int DEFAULT_RANDOMX_VM_CACHE_SIZE = 2;

// Calculate epoch from timestamp: epoch = timestamp / duration (seconds)
uint32_t GetEpoch(uint32_t nTime, uint32_t nDuration);

// TODO check Alpha
// Calculate RandomX key (seed hash) for epoch:
// SHA256d("CoinbaseChain/RandomX/Epoch/N")
uint256 GetSeedHash(uint32_t nEpoch);

// Calculate RandomX commitment from block header
// inHash: optional pre-computed hash (nullptr = use block.hashRandomX)
uint256 GetRandomXCommitment(const CBlockHeader &block,
                             uint256 *inHash = nullptr);

// Initialize RandomX subsystem (call once at startup) TODO
void InitRandomX(int vmCacheSize = DEFAULT_RANDOMX_VM_CACHE_SIZE,
                 bool fastMode = DEFAULT_RANDOMX_FAST_MODE);

// Shutdown RandomX subsystem (releases all VMs and caches)
void ShutdownRandomX();

// Create RandomX VM for epoch (for parallel verification)
// Caller must destroy with randomx_destroy_vm
randomx_vm *CreateVMForEpoch(uint32_t nEpoch);

// Get cached RandomX VM for epoch (shared, multiple callers may get same VM)
// IMPORTANT: Must lock vmRef->hashing_mutex before calling
// randomx_calculate_hash
std::shared_ptr<RandomXVMWrapper> GetCachedVM(uint32_t nEpoch);

} // namespace crypto
} // namespace coinbasechain

#endif // COINBASECHAIN_CRYPTO_RANDOMX_POW_HPP
