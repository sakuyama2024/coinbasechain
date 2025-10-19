# Duplicate Header Penalty Fix

## Issue
The `InvalidateBlock - Basic invalidation with reorg` test fails because nodes disconnect when reconnecting after a chain fork. The failure occurs because:

1. Nodes connect, sync blockA, blockB, blockC
2. Nodes disconnect
3. Node2 builds new fork: blockA → blockD → blockE → blockF (longer chain)
4. Nodes reconnect
5. **Node2 sends headers including blockA → Node1 penalizes with 100 points for "duplicate" → instant disconnect**
6. Reorg never happens

## Root Cause
In `src/chain/validation/chainstate_manager.cpp:AcceptBlockHeader()`, when a header already exists:
```cpp
// Check for duplicate
const chain::CBlockIndex* pindex = LookupBlockIndex(hash);
if (pindex) {
    // Header already exists
    if (!pindex->IsValid()) {
        state.Invalid("duplicate", "header already exists and is invalid");  // ❌ PENALTY
        return nullptr;
    }
    state.Invalid("duplicate", "header already exists");  // ❌ PENALTY - THIS IS THE PROBLEM
    return const_cast<chain::CBlockIndex*>(pindex);
}
```

This causes `network_manager.cpp` to apply a 100-point penalty:
```cpp
if (reason == "duplicate" || ...) {
    peer_manager_->ReportInvalidHeader(peer_id, reason);  // 100 points!
}
```

## Bitcoin Core Solution
Bitcoin Core handles this **completely differently** in `src/net_processing.cpp:2970-2981`:

```cpp
// If the headers we received are already in memory and an ancestor of
// m_best_header or our tip, skip anti-DoS checks. These headers will not
// use any more memory (and we are not leaking information that could be
// used to fingerprint us).
const CBlockIndex *last_received_header{nullptr};
{
    LOCK(cs_main);
    last_received_header = m_chainman.m_blockman.LookupBlockIndex(headers.back().GetHash());
    if (IsAncestorOfBestHeaderOrTip(last_received_header)) {
        already_validated_work = true;  // ✅ SKIP DOS CHECKS
    }
}
```

And in `src/validation.cpp:4102-4116`:
```cpp
// Check for duplicate
uint256 hash = block.GetHash();
BlockMap::iterator miSelf{m_blockman.m_block_index.find(hash)};
if (hash != GetConsensus().hashGenesisBlock) {
    if (miSelf != m_blockman.m_block_index.end()) {
        CBlockIndex* pindex = &(miSelf->second);
        if (ppindex)
            *ppindex = pindex;
        if (pindex->nStatus & BLOCK_FAILED_MASK) {
            LogPrint(BCLog::VALIDATION, "%s: block %s is marked invalid\n", __func__, hash.ToString());
            return state.Invalid(BlockValidationResult::BLOCK_CACHED_INVALID, "duplicate");
        }
        return true;  // ✅ SUCCESS - NO PENALTY
    }
}
```

## Recommended Fix

### Option 1: Network Manager Level (Preferred)
In `src/network/network_manager.cpp:handle_headers()`, skip penalty for known ancestors:

```cpp
if (!pindex) {
    const std::string &reason = state.GetRejectReason();

    // Duplicate header - check if it's already in our chain
    if (reason == "duplicate") {
        // Bitcoin Core approach: If header is a known ancestor, skip penalty
        const chain::CBlockIndex* existing = chainstate_manager_.LookupBlockIndex(header.GetHash());
        if (existing && existing->IsValid()) {
            // Header already accepted and valid - not an attack, just redundant
            LOG_INFO("Header from peer {} is duplicate but valid (ancestor of our chain): {}",
                     peer_id, header.GetHash().ToString().substr(0, 16));
            continue;  // ✅ NO PENALTY
        }
        // If we get here, it's a duplicate of an INVALID header - that's an attack
        LOG_ERROR("Peer {} sent duplicate invalid header: {}", peer_id, reason);
        peer_manager_->ReportInvalidHeader(peer_id, reason);
        // ... disconnect logic
        return false;
    }

    // Other rejection reasons...
}
```

### Option 2: Validation Level
In `src/chain/validation/chainstate_manager.cpp:AcceptBlockHeader()`, don't set rejection for valid duplicates:

```cpp
const chain::CBlockIndex* pindex = LookupBlockIndex(hash);
if (pindex) {
    // Header already exists
    if (!pindex->IsValid()) {
        state.Invalid("duplicate-invalid", "header already exists and is invalid");
        return nullptr;
    }
    // Valid duplicate - return success WITHOUT setting state as invalid
    // state.Invalid("duplicate", "header already exists");  // ❌ REMOVE THIS
    return const_cast<chain::CBlockIndex*>(pindex);  // ✅ SUCCESS
}
```

## Impact
- **Fixes**: `InvalidateBlock - Basic invalidation with reorg` test
- **Prevents**: False positive bans when:
  - Nodes reconnect after temporary disconnection
  - Peer announces headers we already have
  - Testing scenarios with multiple connections/disconnections
- **Security**: Still catches actual attacks (invalid headers marked as duplicate)

## Test Status
- Before fix: 303/304 tests passing (99.7%)
- After fix: Should be 304/304 (100%)

## References
- Bitcoin Core: `src/net_processing.cpp:2970-2981` (anti-DoS skip for known ancestors)
- Bitcoin Core: `src/validation.cpp:4102-4116` (duplicate returns success, not error)
- Test: `test/network/invalidateblock_functional_tests.cpp:29` (exposes the issue)
