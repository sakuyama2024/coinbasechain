# cs_main Implementation Plan

## Summary

Adding cs_main locking to match Unicity's thread safety model exactly.

## Changes Required

### 1. ChainstateManager - Add m_chainstate_mutex ✅ NEXT

**File**: `include/validation/chainstate_manager.hpp`

Add private member:
```cpp
/**
 * The ChainState Mutex
 * A lock that must be held when modifying this ChainState - held in ActivateBestChain() and
 * InvalidateBlock()
 */
RecursiveMutex m_chainstate_mutex;
```

### 2. Split ActivateBestChain into Step Function

**File**: `src/validation/chainstate_manager.cpp`

Create `ActivateBestChainStep()` that:
- Takes `pindexMostWork` parameter
- Is called by `ActivateBestChain()` in batches of 32 blocks
- Returns after each batch to release cs_main
- Has signature matching Unicity:
  ```cpp
  bool ActivateBestChainStep(chain::CBlockIndex* pindexMostWork, bool& fInvalidFound);
  ```

### 3. Refactor ActivateBestChain

**Pattern** (from Unicity validation.cpp:969-1077):
```cpp
bool ActivateBestChain(chain::CBlockIndex* pindexMostWork) {
    AssertLockNotHeld(::cs_main);
    AssertLockNotHeld(m_chainstate_mutex);

    LOCK(m_chainstate_mutex);  // Prevent concurrent ActivateBestChain

    chain::CBlockIndex* pindexNewTip = nullptr;
    do {
        {
            LOCK(cs_main);  // Acquire for one batch

            if (!pindexMostWork) {
                pindexMostWork = FindMostWorkChain();
            }

            if (!pindexMostWork || pindexMostWork == GetTip()) {
                break;
            }

            bool fInvalidFound = false;
            if (!ActivateBestChainStep(pindexMostWork, fInvalidFound)) {
                return false;
            }

            if (fInvalidFound) {
                pindexMostWork = nullptr;  // Wipe cache
            }

            pindexNewTip = GetTip();

        } // cs_main released here

        // Notify outside of cs_main
        // ...

    } while (pindexNewTip != pindexMostWork);

    return true;
}
```

### 4. ActivateBestChainStep Implementation

**Batching logic** (from Unicity validation.cpp:875-916):
```cpp
while (fContinue && nHeight != pindexMostWork->nHeight) {
    int nTargetHeight = std::min(nHeight + 32, pindexMostWork->nHeight);
    vpindexToConnect.clear();
    vpindexToConnect.reserve(nTargetHeight - nHeight);

    // Build list of up to 32 blocks to connect
    chain::CBlockIndex* pindexIter = pindexMostWork->GetAncestor(nTargetHeight);
    while (pindexIter && pindexIter->nHeight != nHeight) {
        vpindexToConnect.push_back(pindexIter);
        pindexIter = pindexIter->pprev;
    }

    // Connect blocks (in reverse order)
    for (auto it = vpindexToConnect.rbegin(); it != vpindexToConnect.rend(); ++it) {
        if (!ConnectTip(*it)) {
            // error handling
        }

        PruneBlockIndexCandidates();  // Prune after EACH block

        // Early return if we've improved
        if (!pindexOldTip || GetTip()->nChainWork > pindexOldTip->nChainWork) {
            fContinue = false;
            break;
        }
    }
}
```

### 5. Add Thread Safety Annotations

**File**: `include/validation/chainstate_manager.hpp`

```cpp
// Include cs_main header
#include "validation/cs_main.hpp"

// Annotate methods:
chain::CBlockIndex* AcceptBlockHeader(...) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
const chain::CBlockIndex* GetTip() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
chain::CBlockIndex* FindMostWorkChain() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
void PruneBlockIndexCandidates() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
bool ConnectTip(chain::CBlockIndex* pindexNew) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
bool DisconnectTip() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
void TryAddBlockIndexCandidate(chain::CBlockIndex* pindex) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

// ActivateBestChain must NOT hold cs_main when called
bool ActivateBestChain(chain::CBlockIndex* pindexMostWork = nullptr) LOCKS_EXCLUDED(::cs_main, m_chainstate_mutex);

// Helper - holds cs_main
bool ActivateBestChainStep(chain::CBlockIndex* pindexMostWork, bool& fInvalidFound) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
```

### 6. BlockManager Annotations

**File**: `include/chain/block_manager.hpp`

```cpp
const chain::CBlockIndex* LookupBlockIndex(const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
chain::CBlockIndex* AddToBlockIndex(const CBlockHeader& header) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
```

### 7. Update Call Sites

**header_sync.cpp** - ProcessHeaders:
```cpp
bool HeaderSync::ProcessHeaders(...) {
    // NO lock here - called from network thread

    for (const auto& header : headers) {
        validation::ValidationState state;

        // AcceptBlockHeader now requires cs_main internally
        chain::CBlockIndex* pindex;
        {
            LOCK(::cs_main);
            pindex = chainstate_manager_.AcceptBlockHeader(header, state, true);
            if (pindex) {
                chainstate_manager_.TryAddBlockIndexCandidate(pindex);
            }
        }

        if (!pindex) {
            return false;
        }
    }

    // ActivateBestChain acquires its own locks
    bool activate_result = chainstate_manager_.ActivateBestChain(nullptr);
    return activate_result;
}
```

**network_manager.cpp** - handle_getheaders_message:
```cpp
bool NetworkManager::handle_getheaders_message(...) {
    const chain::CBlockIndex* fork_point = nullptr;
    const chain::CBlockIndex* tip = nullptr;

    {
        LOCK(::cs_main);

        for (const auto& hash_array : msg->block_locator_hashes) {
            uint256 hash;
            std::memcpy(hash.data(), hash_array.data(), 32);

            const chain::CBlockIndex* pindex = chainstate_manager_.GetBlockManager().LookupBlockIndex(hash);
            if (pindex && chainstate_manager_.GetBlockManager().ActiveChain().Contains(pindex)) {
                fork_point = pindex;
                break;
            }
        }

        if (!fork_point) {
            // Find genesis
        }

        tip = chainstate_manager_.GetBlockManager().GetTip();
    }

    // Build response outside lock
    // ...
}
```

### 8. Testing Strategy

1. Build and verify compilation
2. Run feature_suspicious_reorg.py (single peer)
3. Create stress test with 4 concurrent peers
4. Verify no crashes, no data races
5. Check that suspicious reorg detection still works

### 9. Expected Benefits

- **Thread safety**: No more race conditions
- **Correctness**: Matches Unicity's proven model
- **Responsiveness**: cs_main released every 32 blocks
- **Future-proof**: Ready for RPC, mempool, full validation

### 10. Risks

- **Performance**: More locking overhead (minimal - recursive mutex is fast)
- **Deadlocks**: Must follow lock order (cs_main before m_chainstate_mutex never)
- **Complexity**: More code to maintain

### 11. Files to Modify

1. ✅ `include/util/threadsafety.hpp` - Thread safety annotations
2. ✅ `include/util/sync.hpp` - LOCK macros, RecursiveMutex
3. ✅ `include/util/macros.hpp` - UNIQUE_NAME
4. ✅ `include/validation/cs_main.hpp` - Declare cs_main
5. ✅ `src/validation/cs_main.cpp` - Define cs_main
6. ⏳ `include/validation/chainstate_manager.hpp` - Add m_chainstate_mutex, annotations
7. ⏳ `src/validation/chainstate_manager.cpp` - Implement new locking pattern
8. ⏳ `src/sync/header_sync.cpp` - Update call sites
9. ⏳ `src/network/network_manager.cpp` - Update call sites
10. ⏳ `include/chain/block_manager.hpp` - Add annotations
11. ⏳ `CMakeLists.txt` - ✅ Already added cs_main.cpp

## Implementation Order

1. ✅ Create threading infrastructure (sync.hpp, threadsafety.hpp, macros.hpp)
2. ✅ Declare cs_main
3. **NEXT**: Add m_chainstate_mutex to ChainstateManager
4. Add thread safety annotations to all validation methods
5. Implement ActivateBestChainStep with batching
6. Refactor ActivateBestChain with lock acquire/release pattern
7. Update all call sites to use LOCK(cs_main) appropriately
8. Build and fix compilation errors
9. Test with feature_suspicious_reorg.py
10. Create and run multi-peer stress test

## Current Status

- Threading infrastructure: ✅ DONE
- cs_main declaration: ✅ DONE
- Next task: Add m_chainstate_mutex and annotations

This is a large change (300+ lines modified). Recommend implementing in phases with testing after each phase.
