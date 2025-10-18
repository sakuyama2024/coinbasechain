// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "crypto/randomx_pow.hpp"
#include "arith_uint256.h"
#include "crypto/sha256.h"
#include "util/logging.hpp"
#include <cstring>
#include <list>
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

// Simple LRU cache implementation
template <typename K, typename V> class LRUCache {
public:
  explicit LRUCache(size_t capacity) : capacity_(capacity) {}

  bool contains(const K &key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.find(key) != cache_.end();
  }

  V get(const K &key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(key);
    if (it == cache_.end()) {
      throw std::runtime_error("Key not found in cache");
    }
    // Move to front (most recently used)
    access_order_.remove(key);
    access_order_.push_front(key);
    return it->second;
  }

  void insert(const K &key, V value) {
    std::lock_guard<std::mutex> lock(mutex_);

    // If key exists, update it
    auto it = cache_.find(key);
    if (it != cache_.end()) {
      access_order_.remove(key);
      access_order_.push_front(key);
      it->second = value;
      return;
    }

    // Evict oldest if at capacity
    if (cache_.size() >= capacity_ && capacity_ > 0) {
      K oldest = access_order_.back();
      access_order_.pop_back();
      cache_.erase(oldest);
    }

    // Insert new entry
    cache_[key] = value;
    access_order_.push_front(key);
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    access_order_.clear();
  }

private:
  size_t capacity_;
  mutable std::mutex mutex_;
  std::map<K, V> cache_;
  std::list<K> access_order_;
};

// Global caches
static std::unique_ptr<LRUCache<uint32_t, std::shared_ptr<RandomXCacheWrapper>>>
    g_cache_rx_cache;
static std::unique_ptr<LRUCache<uint32_t, std::shared_ptr<RandomXVMWrapper>>>
    g_cache_rx_vm_light;

static bool g_randomx_initialized = false;

uint32_t GetEpoch(uint32_t nTime, uint32_t nDuration) {
  return nTime / nDuration;
}

uint256 GetSeedHash(uint32_t nEpoch) {
  char buffer[128];
  snprintf(buffer, sizeof(buffer), RANDOMX_EPOCH_SEED_STRING, nEpoch);
  std::string s(buffer);

  // SHA256d (double SHA256)
  uint256 h1, h2;
  CSHA256()
      .Write((const unsigned char *)s.data(), s.size())
      .Finalize(h1.begin());
  CSHA256().Write(h1.begin(), 32).Finalize(h2.begin());
  return h2;
}

// Get or create VM for an epoch
// VMs use RANDOMX_FLAG_SECURE (interpreter mode) for thread-safe operation with
// mutex
std::shared_ptr<RandomXVMWrapper> GetCachedVM(uint32_t nEpoch) {
  if (!g_randomx_initialized) {
    throw std::runtime_error("RandomX not initialized");
  }

  // Check if light mode VM exists
  if (g_cache_rx_vm_light->contains(nEpoch)) {
    return g_cache_rx_vm_light->get(nEpoch);
  }

  uint256 seedHash = GetSeedHash(nEpoch);

  // Use RANDOMX_FLAG_SECURE to disable JIT and enable interpreter mode
  // This makes the VM thread-safe when used with mutex protection
  randomx_flags flags = randomx_get_flags();
  flags |=
      RANDOMX_FLAG_SECURE; // Force interpreter mode (thread-safe with mutex)

  std::lock_guard<std::mutex> lock(g_randomx_mutex);

  // Double-check after acquiring lock
  if (g_cache_rx_vm_light->contains(nEpoch)) {
    return g_cache_rx_vm_light->get(nEpoch);
  }

  // Create or get cache
  std::shared_ptr<RandomXCacheWrapper> myCache;
  if (g_cache_rx_cache->contains(nEpoch)) {
    myCache = g_cache_rx_cache->get(nEpoch);
  } else {
    randomx_cache *pCache = randomx_alloc_cache(flags);
    if (!pCache) {
      throw std::runtime_error("Failed to allocate RandomX cache");
    }
    randomx_init_cache(pCache, seedHash.data(), seedHash.size());
    myCache = std::make_shared<RandomXCacheWrapper>(pCache);
    g_cache_rx_cache->insert(nEpoch, myCache);

    LOG_CRYPTO_INFO("Created RandomX cache for epoch {}", nEpoch);
  }

  // Create light mode VM with SECURE flag (interpreter mode)
  randomx_vm *myVM = randomx_create_vm(flags, myCache->cache, nullptr);
  if (!myVM) {
    throw std::runtime_error("Failed to create RandomX VM");
  }

  auto vmWrapper = std::make_shared<RandomXVMWrapper>(myVM, myCache);
  g_cache_rx_vm_light->insert(nEpoch, vmWrapper);

  LOG_CRYPTO_INFO("Created RandomX VM for epoch {} (secure interpreter mode)",
                  nEpoch);

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

void InitRandomX(int vmCacheSize, bool fastMode) {
  std::lock_guard<std::mutex> lock(g_randomx_mutex);

  if (g_randomx_initialized) {
    return;
  }

  g_cache_rx_cache = std::make_unique<
      LRUCache<uint32_t, std::shared_ptr<RandomXCacheWrapper>>>(vmCacheSize);
  g_cache_rx_vm_light =
      std::make_unique<LRUCache<uint32_t, std::shared_ptr<RandomXVMWrapper>>>(
          vmCacheSize);

  g_randomx_initialized = true;

  LOG_CRYPTO_INFO("RandomX initialized (cache size: {}, secure interpreter "
                  "mode for thread safety)",
                  vmCacheSize);
}

void ShutdownRandomX() {
  std::lock_guard<std::mutex> lock(g_randomx_mutex);

  if (!g_randomx_initialized) {
    return;
  }

  g_cache_rx_vm_light->clear();
  g_cache_rx_cache->clear();

  g_cache_rx_vm_light.reset();
  g_cache_rx_cache.reset();

  g_randomx_initialized = false;

  LOG_CRYPTO_INFO("RandomX shutdown complete");
}

randomx_vm *CreateVMForEpoch(uint32_t nEpoch) {
  if (!g_randomx_initialized) {
    throw std::runtime_error("RandomX not initialized");
  }

  uint256 seedHash = GetSeedHash(nEpoch);
  randomx_flags flags = randomx_get_flags();
  flags |= RANDOMX_FLAG_SECURE; // Use secure mode for thread safety

  // Get or create cache for this epoch
  std::shared_ptr<RandomXCacheWrapper> myCache;
  {
    std::lock_guard<std::mutex> lock(g_randomx_mutex);

    if (g_cache_rx_cache->contains(nEpoch)) {
      myCache = g_cache_rx_cache->get(nEpoch);
    } else {
      randomx_cache *pCache = randomx_alloc_cache(flags);
      if (!pCache) {
        throw std::runtime_error("Failed to allocate RandomX cache");
      }
      randomx_init_cache(pCache, seedHash.data(), seedHash.size());
      myCache = std::make_shared<RandomXCacheWrapper>(pCache);
      g_cache_rx_cache->insert(nEpoch, myCache);

      LOG_CRYPTO_INFO(
          "Created RandomX cache for epoch {} (for parallel verification)",
          nEpoch);
    }
  }

  // Create a new VM from the cache (cheap operation, no lock needed)
  randomx_vm *vm = randomx_create_vm(flags, myCache->cache, nullptr);
  if (!vm) {
    throw std::runtime_error(
        "Failed to create RandomX VM for parallel verification");
  }

  return vm;
}

} // namespace crypto
} // namespace coinbasechain
