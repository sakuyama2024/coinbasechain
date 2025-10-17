# RandomX Implementation Comparison: Unicity vs CoinbaseChain

## Key Differences Found

### 1. **Commitment Calculation** ⚠️ CRITICAL

**Unicity** (pow.cpp:617-619):
```cpp
uint256 GetRandomXCommitment(const CBlockHeader& block, uint256 *inHash) {
    uint256 rx_hash = inHash==nullptr ? block.hashRandomX : *inHash;
    CBlockHeader rx_blockHeader(block);
    rx_blockHeader.hashRandomX.SetNull();   // set to null when hashing
    char rx_cm[RANDOMX_HASH_SIZE];
    randomx_calculate_commitment(&rx_blockHeader, sizeof(rx_blockHeader), rx_hash.data(), rx_cm);
    return uint256(std::vector<unsigned char>(rx_cm, rx_cm + sizeof(rx_cm)));
}
```

**CoinbaseChain** (randomx_pow.cpp:237-249):
```cpp
uint256 GetRandomXCommitment(const CBlockHeader& block, uint256* inHash) {
    uint256 rx_hash;
    if (inHash == nullptr) {
        // Calculate RandomX hash
        int32_t nEpoch = GetEpoch(block.nTime, DEFAULT_RANDOMX_EPOCH_DURATION);
        auto vmRef = GetVM(nEpoch);
        // ... calculate rx_hash ...
    } else {
        rx_hash = *inHash;
    }

    // Double SHA256: commitment = SHA256(SHA256(rx_hash || block_header))
    CBlockHeader tmp(block);
    CSHA256 hasher1, hasher2;
    hasher1.Write(rx_hash.data(), 32);
    hasher1.Write((const uint8_t*)&tmp, sizeof(tmp));
    // ... finalize ...
}
```

### ❌ **PROBLEM**: Unicity uses `randomx_calculate_commitment()` NOT double SHA256!

We are using a **completely different commitment algorithm** than Unicity.

---

## 2. **Hashing Process**

### Unicity Approach:
1. Set `hashRandomX` to null in temporary header
2. Call `randomx_calculate_hash(vm, &tmp, sizeof(tmp), rx_hash)`
3. Call `randomx_calculate_commitment(&rx_blockHeader, sizeof(rx_blockHeader), rx_hash.data(), rx_cm)`

### CoinbaseChain Approach:
1. Set `hashRandomX` to null in temporary header  ✓
2. Call `randomx_calculate_hash(vm, &tmp, sizeof(tmp), rx_hash)`  ✓
3. Calculate commitment = SHA256(SHA256(rx_hash || block_header))  ❌ WRONG

---

## 3. **RandomX Custom Function**

Unicity uses a custom RandomX function: `randomx_calculate_commitment()`

This is likely defined in their **modified RandomX fork** at:
https://github.com/unicitynetwork/RandomX

We need to check if this function exists in their fork.

---

## 4. **Verification Modes**

### Unicity has 3 modes (pow.h):
```cpp
enum POWVerifyMode {
    POW_VERIFY_COMMITMENT_ONLY = 0,  // Fast - only check commitment
    POW_VERIFY_FULL,                 // Full - check hash AND commitment
    POW_VERIFY_MINING                // Mining - calculate both
};
```

### CoinbaseChain has 3 modes:
```cpp
enum class POWVerifyMode {
    FULL = 0,
    COMMITMENT_ONLY,
    MINING
};
```

✓ Same concept, different naming

---

## 5. **LRU Cache Implementation**

### Unicity (pow.cpp:62-69):
```cpp
using LRURandomXCacheRef = std::shared_ptr<
    boost::compute::detail::lru_cache<int32_t, RandomXCacheRef>>;

static LRURandomXCacheRef cache_rx_cache;
static LRURandomXVMRef cache_rx_vm_light;
static LRURandomXVMRef cache_rx_vm_fast;
static LRURandomXDatasetRef cache_rx_dataset;
```

Uses **Boost.Compute LRU cache** (external dependency)

### CoinbaseChain:
```cpp
template<typename K, typename V>
class LRUCache {
    size_t capacity_;
    mutable std::mutex mutex_;
    std::map<K, V> cache_;
    std::list<K> access_order_;
};
```

Custom implementation (no external dependencies)

✓ Both are LRU, ours is simpler

---

## 6. **Fast vs Light Mode**

### Unicity:
- Has **separate caches** for light mode and fast mode VMs
- Light mode: 256 MB (cache only)
- Fast mode: 2+ GB (full dataset)
- Creates fast mode VM in **background thread** after IBD finishes
- Checks `g_isIBDFinished` flag to enable fast mode

### CoinbaseChain:
- Single cache for all VMs
- Has fast mode flag but doesn't use dataset
- No background thread creation

⚠️ We're missing the fast mode optimization

---

## 7. **Seed Hash Calculation**

### Unicity (pow.cpp:480-493):
```cpp
static const char *RANDOMX_EPOCH_SEED_STRING = "Scash/RandomX/Epoch/%d";

uint256 GetSeedHash(uint32_t nEpoch) {
    std::string s = strprintf(RANDOMX_EPOCH_SEED_STRING, nEpoch);
    uint256 h1, h2;
    CSHA256().Write((const unsigned char*)s.data(), s.size()).Finalize(h1.begin());
    CSHA256().Write(h1.begin(), 32).Finalize(h2.begin());
    return h2;  // SHA256d of "Scash/RandomX/Epoch/%d"
}
```

### CoinbaseChain (randomx_pow.cpp:120-127):
```cpp
uint256 GetSeedHash(uint32_t nEpoch) {
    std::string seed_str = "coinbasechain_epoch_" + std::to_string(nEpoch);
    uint256 seed_hash;
    CSHA256()
        .Write((const uint8_t*)seed_str.data(), seed_str.size())
        .Finalize(seed_hash.data());
    return seed_hash;  // Single SHA256
}
```

⚠️ **Differences**:
1. Different string format
2. We use **single SHA256**, Unicity uses **double SHA256** (SHA256d)

---

## Critical Issues to Fix

### 🔴 **HIGHEST PRIORITY**

1. **Find and use `randomx_calculate_commitment()`**
   - Check Unicity's RandomX fork for this function
   - This is NOT standard RandomX - it's a custom addition
   - Our double SHA256 approach is completely wrong

2. **Fix GetSeedHash to use SHA256d**
   - Change from single SHA256 to double SHA256
   - Consider using "CoinbaseChain/RandomX/Epoch/%d" format

### 🟡 **MEDIUM PRIORITY**

3. **Implement Fast Mode properly**
   - Create separate light/fast VM caches
   - Use randomx dataset for fast mode
   - Background thread creation after sync

4. **Review hashRandomX field usage**
   - Unicity stores RandomX hash IN the block header
   - We have this field but need to verify it's used correctly

### 🟢 **LOW PRIORITY**

5. **Add configuration options**
   - `-randomxvmcachesize` flag
   - `-randomxfastmode` flag
   - Check `g_isIBDFinished` for optimizations

---

## Next Steps

1. ✅ Clone Unicity's RandomX fork
2. ❌ Find `randomx_calculate_commitment()` implementation
3. ❌ Update our code to use correct commitment algorithm
4. ❌ Update seed hash to SHA256d
5. ❌ Test against Unicity's genesis block

## Impact on Genesis Miner

⚠️ **The genesis miner is currently generating INVALID blocks!**

The commitment calculation is wrong, so any block we mine won't be accepted by Unicity-compatible nodes.

We MUST fix the commitment algorithm before mining any real genesis blocks.
