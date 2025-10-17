# RandomX Integration Plan

## What is RandomX?

RandomX is a CPU-optimized proof-of-work algorithm designed to be ASIC-resistant. It uses:
- Random code execution (randomly generated programs)
- Memory-hard algorithm
- Epoch-based key changes (every ~2 days)

## How Unicity Uses RandomX

From `pow.cpp` analysis:

### 1. **Epoch System**
- Epochs are calculated from timestamp: `epoch = timestamp / duration`
- Each epoch has a seed hash: `SHA256d("Scash/RandomX/Epoch/N")`
- Default epoch duration: typically 2 days (172800 seconds)

### 2. **Two-Step PoW Verification**
```cpp
hashRandomX = RandomX(blockHeader_without_hashRandomX_field)
commitment = RandomXCommitment(blockHeader, hashRandomX)
valid = commitment <= target
```

### 3. **VM Caching**
- Light mode: VM with cache only (~256MB)
- Fast mode: VM with full dataset (~2GB) - faster but more memory
- LRU cache for multiple epochs (default: 2 epochs cached)

### 4. **Three Verification Modes**
- `POW_VERIFY_COMMITMENT_ONLY`: Just check commitment (fast)
- `POW_VERIFY_FULL`: Verify both hash and commitment (full validation)
- `POW_VERIFY_MINING`: Calculate hash and commitment (for miners)

## Integration Approach for CoinbaseChain

### Phase 1: Add RandomX Library

Use CMake FetchContent to download RandomX v1.2.1:

```cmake
include(FetchContent)

FetchContent_Declare(
  randomx
  GIT_REPOSITORY https://github.com/tevador/RandomX.git
  GIT_TAG        v1.2.1
)

FetchContent_MakeAvailable(randomx)
```

### Phase 2: Create RandomX Wrapper

Simplified version of Unicity's implementation:

**Files to create:**
- `include/crypto/randomx_wrapper.hpp` - C++ wrapper for RandomX
- `src/crypto/randomx_wrapper.cpp` - Implementation

**Key functions:**
```cpp
// Calculate epoch from timestamp
uint32_t GetEpoch(uint32_t nTime, uint32_t nDuration);

// Get seed hash for epoch
uint256 GetSeedHash(uint32_t nEpoch);

// Calculate RandomX hash of block header
uint256 CalculateRandomXHash(const CBlockHeader& block);

// Calculate commitment (hash of header + randomx_hash)
uint256 GetRandomXCommitment(const CBlockHeader& block);

// Check if RandomX PoW is valid
bool CheckProofOfWorkRandomX(const CBlockHeader& block, uint32_t nBits, uint32_t nEpochDuration);
```

### Phase 3: Update CBlockHeader

Already has `hashRandomX` field - this stores the RandomX hash.

### Phase 4: Update PoW Validation

Modify `HeaderSync::CheckProofOfWork()` to:
1. Check if RandomX is enabled for this chain
2. If yes, use `CheckProofOfWorkRandomX()`
3. If no, use legacy SHA256d PoW

### Phase 5: Add ChainParams Fields

Add to `ConsensusParams`:
```cpp
bool fPowRandomX{false};              // Enable RandomX PoW
uint32_t nRandomXEpochDuration{172800}; // 2 days in seconds
int64_t nRandomXActivationHeight{0};  // Height when RandomX activates
```

## Simplifications vs Unicity

1. **No legacy SHA256d support**: CoinbaseChain starts with RandomX from genesis
2. **Simpler caching**: Start with light mode only, add fast mode later
3. **No background VM creation**: Create synchronously initially
4. **Single LRU cache**: Cache only 2 epochs (current and previous)

## Memory Requirements

- **Light mode**: ~256MB per epoch × 2 epochs = ~512MB
- **Fast mode** (future): ~2GB per epoch × 2 epochs = ~4GB

## Performance

- **Light mode**: ~2000 hashes/second (CPU dependent)
- **Fast mode**: ~20,000 hashes/second (with full dataset)

For headers-only chain:
- Initial sync: Only verify commitments (fast)
- Full validation: Can be done later or on-demand
- Mining: Would use mining mode to generate hash + commitment

## Implementation Order

1. ✅ Research Unicity's RandomX integration (current)
2. Add RandomX library via CMake FetchContent
3. Create basic RandomX wrapper (epoch, seed, hash calculation)
4. Update PoW validation to use RandomX
5. Add tests for RandomX hashing
6. Update ChainParams with RandomX settings

## Notes

- RandomX is deterministic: same input → same output
- Epoch changes require new VM initialization (~1 second for light mode)
- VMs can be cached and reused within same epoch
- For RegTest, we can disable RandomX or use very short epochs
