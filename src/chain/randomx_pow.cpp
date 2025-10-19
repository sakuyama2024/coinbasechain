// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "chain/randomx_pow.hpp"
#include "chain/arith_uint256.hpp"
#include "chain/sha256.hpp"
#include "chain/logging.hpp"
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace coinbasechain {
namespace crypto {

// Seed string for epoch key generation
static const char *RANDOMX_EPOCH_SEED_STRING = "CoinbaseChain/RandomX/Epoch/%d";

// Mutex for cache access
static std::mutex g_randomx_mutex;

// RAII wrappers for RandomX objects
struct RandomXCacheWrapper {
  randomx_cache *cache = nullptr;

  explicit RandomXCacheWrapper(randomx_cache *c) : cache(c) {}
  ~RandomXCacheWrapper() {
    if (cache)
      randomx_release_cache(cache);
  }
};

// Thread-local cache storage: each thread gets its own cache 
static thread_local std::map<uint32_t, std::shared_ptr<RandomXCacheWrapper>>
    t_cache_storage;

// Thread-local VM storage (each thread gets its own VM)
static thread_local std::map<uint32_t, std::shared_ptr<RandomXVMWrapper>>
    t_vm_cache;

static bool g_randomx_initialized = false;

uint32_t GetEpoch(uint32_t nTime, uint32_t nDuration) {
  return nTime / nDuration;
}

uint256 GetSeedHash(uint32_t nEpoch) {
  char buffer[128];
  snprintf(buffer, sizeof(buffer), RANDOMX_EPOCH_SEED_STRING, nEpoch);
  std::string s(buffer);

  uint256 h1, h2;
  CSHA256()
      .Write((const unsigned char *)s.data(), s.size())
      .Finalize(h1.begin());
  CSHA256().Write(h1.begin(), 32).Finalize(h2.begin());
  return h2;
}

// Get or create thread-local VM for an epoch
// Each thread gets its own VM instance for thread safety
std::shared_ptr<RandomXVMWrapper> GetCachedVM(uint32_t nEpoch) {
  if (!g_randomx_initialized) {
    throw std::runtime_error("RandomX not initialized");
  }

  // Check if this thread already has a VM for this epoch
  auto it = t_vm_cache.find(nEpoch);
  if (it != t_vm_cache.end()) {
    return it->second;
  }

  uint256 seedHash = GetSeedHash(nEpoch);
  randomx_flags flags = randomx_get_flags();
  // Disable JIT and use secure mode (interpreter only)
  // flags = static_cast<randomx_flags>(flags & ~RANDOMX_FLAG_JIT);
  // flags = static_cast<randomx_flags>(flags | RANDOMX_FLAG_SECURE);

  // Get or create thread-local cache (each thread has isolated cache)
  std::shared_ptr<RandomXCacheWrapper> myCache;
  auto cache_it = t_cache_storage.find(nEpoch);
  if (cache_it != t_cache_storage.end()) {
    myCache = cache_it->second;
  } else {
    randomx_cache *pCache = randomx_alloc_cache(flags);
    if (!pCache) {
      throw std::runtime_error("Failed to allocate RandomX cache");
    }
    randomx_init_cache(pCache, seedHash.data(), seedHash.size());
    myCache = std::make_shared<RandomXCacheWrapper>(pCache);
    t_cache_storage[nEpoch] = myCache;

    LOG_CRYPTO_INFO("Created thread-local RandomX cache for epoch {}", nEpoch);
  }

  // Create thread-local VM (no lock needed, each thread has its own cache and
  // VM)
  randomx_vm *myVM = randomx_create_vm(flags, myCache->cache, nullptr);
  if (!myVM) {
    throw std::runtime_error("Failed to create RandomX VM");
  }

  auto vmWrapper = std::make_shared<RandomXVMWrapper>(myVM, myCache);
  t_vm_cache[nEpoch] = vmWrapper;

  LOG_CRYPTO_INFO("Created thread-local RandomX VM for epoch {} (interpreter mode, isolated cache)", nEpoch);

  return vmWrapper;
}

uint256 GetRandomXCommitment(const CBlockHeader &block, uint256 *inHash) {
  uint256 rx_hash = inHash == nullptr ? block.hashRandomX : *inHash;

  // Create copy of header with hashRandomX set to null
  CBlockHeader rx_blockHeader(block);
  rx_blockHeader.hashRandomX.SetNull();

  // Calculate commitment
  char rx_cm[RANDOMX_HASH_SIZE];
  randomx_calculate_commitment(&rx_blockHeader, sizeof(rx_blockHeader),
                               rx_hash.data(), rx_cm);

  return uint256(std::vector<unsigned char>(rx_cm, rx_cm + sizeof(rx_cm)));
}

void InitRandomX(int vmCacheSize) {
  std::lock_guard<std::mutex> lock(g_randomx_mutex);

  if (g_randomx_initialized) {
    return;
  }

  g_randomx_initialized = true;

  LOG_CRYPTO_INFO("RandomX initialized with thread-local caches and VMs "
                  "(isolated per thread)");
}

void ShutdownRandomX() {
  std::lock_guard<std::mutex> lock(g_randomx_mutex);

  if (!g_randomx_initialized) {
    return;
  }

  g_randomx_initialized = false;

  LOG_CRYPTO_INFO("RandomX shutdown complete (thread-local caches and VMs "
                  "cleaned up automatically)");
}

randomx_vm *CreateVMForEpoch(uint32_t nEpoch) {
  if (!g_randomx_initialized) {
    throw std::runtime_error("RandomX not initialized");
  }

  uint256 seedHash = GetSeedHash(nEpoch);
  randomx_flags flags = randomx_get_flags();
  // Disable JIT and use secure mode (interpreter only)
  flags = static_cast<randomx_flags>(flags & ~RANDOMX_FLAG_JIT);
  flags = static_cast<randomx_flags>(flags | RANDOMX_FLAG_SECURE);

  // Get or create thread-local cache (each thread has isolated cache)
  std::shared_ptr<RandomXCacheWrapper> myCache;
  auto cache_it = t_cache_storage.find(nEpoch);
  if (cache_it != t_cache_storage.end()) {
    myCache = cache_it->second;
  } else {
    randomx_cache *pCache = randomx_alloc_cache(flags);
    if (!pCache) {
      throw std::runtime_error("Failed to allocate RandomX cache");
    }
    randomx_init_cache(pCache, seedHash.data(), seedHash.size());
    myCache = std::make_shared<RandomXCacheWrapper>(pCache);
    t_cache_storage[nEpoch] = myCache;

    LOG_CRYPTO_INFO("Created thread-local RandomX cache for epoch {} (for "
                    "parallel verification)",
                    nEpoch);
  }

  // Create a new VM from the thread-local cache
  randomx_vm *vm = randomx_create_vm(flags, myCache->cache, nullptr);
  if (!vm) {
    throw std::runtime_error(
        "Failed to create RandomX VM for parallel verification");
  }

  return vm;
}

} // namespace crypto
} // namespace coinbasechain
