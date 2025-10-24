# Bitcoin Core Deviations - Block Relay Issues

**Date:** 2025-10-24
**Status:** ACTIVE ISSUES - Requires fixes
**Severity:** CRITICAL (bandwidth waste, network inefficiency)

## Overview

This document tracks identified deviations from Bitcoin Core's block relay behavior in CoinbaseChain. These issues cause unnecessary bandwidth usage and inefficient network propagation.

---

## Issue #1: Block Relay During IBD (CRITICAL)

### Severity: CRITICAL
**Impact:** Massive bandwidth waste during initial sync

### Problem Description
Nodes relay every block they receive during Initial Block Download (IBD) to all connected peers, even though these blocks are already widely available on the network.

### Observed Behavior
```
[2025-10-24 20:40:02.786] [network] [info] Relaying block 28e7b599... to 6 peers
[2025-10-24 20:40:02.786] [network] [info] Relaying block 6fba4447... to 6 peers
[2025-10-24 20:40:02.786] [network] [info] Relaying block 8233346e... to 6 peers
... (continues for all 81 blocks during sync)
```

### Bitcoin Core Behavior
Bitcoin Core **does not relay blocks during IBD**. It only relays blocks after the node has caught up with the network.

**Reference:** Bitcoin Core's `PeerManagerImpl::ProcessMessage()` checks `IsInitialBlockDownload()` before relaying blocks.

### Code Location
**File:** `src/application.cpp`
**Lines:** 84-89

```cpp
// Subscribe to block notifications to relay new blocks to peers
block_sub_ = Notifications().SubscribeBlockConnected(
    [this](const CBlockHeader &block, const chain::CBlockIndex *pindex) {
      if (pindex && network_manager_) {
        network_manager_->relay_block(pindex->GetBlockHash());  // ❌ NO IBD CHECK
      }
    });
```

### Correct Implementation
```cpp
block_sub_ = Notifications().SubscribeBlockConnected(
    [this](const CBlockHeader &block, const chain::CBlockIndex *pindex) {
      // Only relay blocks if not in IBD (Bitcoin Core behavior)
      if (pindex && network_manager_ && !chainstate_manager_->IsInitialBlockDownload()) {
        network_manager_->relay_block(pindex->GetBlockHash());
      }
    });
```

### Impact Analysis
- **Bandwidth waste:** Syncing 100 blocks with 6 peers = 600 unnecessary block relays
- **Network congestion:** All syncing nodes spam the network
- **Peer reputation:** Peers may consider this misbehavior

### Fix Complexity: TRIVIAL
Add single `!IsInitialBlockDownload()` check.

---

## Issue #2: Reorg Block Relay (CONFIRMED)

### Severity: HIGH
**Impact:** Bandwidth waste during chain reorganizations

### Problem Description
During a chain reorganization, the node relays ALL blocks that get reconnected, not just the new tip. This means a 100-block reorg causes 100 block relays.

### Observed Behavior
```
[2025-10-24 20:40:02.786] [network] [info] Found fork point at height 0 (hash=cb608755c4b2bee0)
[2025-10-24 20:40:02.786] [network] [info] Relaying block 28e7b5990785a95a to 6 peers  (block 1)
[2025-10-24 20:40:02.786] [network] [info] Relaying block 6fba4447376d0a20 to 6 peers  (block 2)
[2025-10-24 20:40:02.786] [network] [info] Relaying block 823346e676a14bb3 to 6 peers  (block 3)
... (continues for all reconnected blocks)
```

### Bitcoin Core Behavior
Bitcoin Core only relays **recently connected blocks** (typically just the new tip). Old blocks that get reconnected during a reorg are NOT relayed because peers already know about them.

**Reference:** Bitcoin Core's `CConnman::ForEachNode()` only sends INV for blocks that are "new" to the network.

### Root Cause Analysis

**File:** `src/chain/chainstate_manager.cpp`
**Line:** 505

```cpp
// Inside ConnectTip() - called for EVERY block during reorg
Notifications().NotifyBlockConnected(header, pindexNew);
```

**File:** `src/application.cpp`
**Lines:** 84-89

The `BlockConnected` notification fires for **every** block that `ConnectTip()` processes, including old blocks during reorgs.

### Architectural Issue

The current design has a fundamental problem:

1. `ActivateBestChain()` processes a chain reorganization
2. For each block that needs to be connected, it calls `ConnectTip()`
3. `ConnectTip()` fires `NotifyBlockConnected()` for EVERY block
4. Application's subscriber relays EVERY block

### Correct Design (Bitcoin Core approach)

Bitcoin Core tracks:
- **Block Source:** Which peer sent us the block
- **Block Time:** When we first learned about the block
- **Recently Added:** Only relay blocks added in the last few seconds

**Relay Rules:**
- Only relay blocks **first learned about recently** (< 10 seconds)
- Never relay blocks that are old (from disk or deep reorgs)
- Never relay blocks back to the peer that sent them

### Fix Options

**Option A: Add timestamp tracking**
Track when we first learned about each block and only relay recent ones.

**Option B: Simplify subscription**
Only relay blocks in specific scenarios:
- Locally mined blocks (always relay)
- Blocks received from peers during normal sync (only relay if not IBD)
- Skip relay during reorgs

**Option C: Separate notifications**
Create distinct notifications:
- `BlockMinedLocally` → always relay
- `BlockReceivedFromPeer` → relay if not IBD
- `BlockReconnectedDuringReorg` → never relay

### Fix Complexity: MEDIUM
Requires architectural decision on notification design.

---

## Issue #3: No Block Relay Deduplication

### Severity: MEDIUM
**Impact:** Inefficient bandwidth usage

### Problem Description
The `RelayBlock()` function sends blocks to ALL connected peers without tracking:
1. Which peer sent us the block (don't send back to source)
2. Which peers already know about the block
3. Per-peer inventory deduplication

### Code Location
**File:** `src/network/block_relay_manager.cpp`
**Lines:** 119-148

```cpp
void BlockRelayManager::RelayBlock(const uint256 &block_hash) {
  // Create INV message with the new block
  auto inv_msg = std::make_unique<message::InvMessage>();

  // ... setup message ...

  // Send to all connected peers
  auto all_peers = peer_manager_.get_all_peers();
  for (const auto &peer : all_peers) {
    if (peer && peer->is_connected() && peer->state() == PeerState::READY) {
      peer->send_message(std::move(msg_copy));  // ❌ NO DEDUPLICATION
    }
  }
}
```

### Bitcoin Core Behavior

Bitcoin Core maintains:

**Per-Block Tracking:**
```cpp
std::map<uint256, NodeId> mapBlockSource;  // Which peer sent us this block
```

**Per-Peer Tracking:**
```cpp
struct Peer {
    std::set<uint256> setInventoryTxToSend;  // Pending INVs to send
    std::set<uint256> setInventoryKnown;     // What peer already knows
};
```

**Relay Logic:**
```cpp
if (peer_id != mapBlockSource[block_hash]) {  // Don't send back to source
    if (!setInventoryKnown.count(block_hash)) {  // Peer doesn't know
        setInventoryTxToSend.insert(block_hash);  // Queue for sending
    }
}
```

### Impact Analysis
- Sends blocks back to the peer that announced them
- Sends duplicate INVs if block is relayed multiple times
- Wastes bandwidth on peers that already have the block

### Fix Complexity: HIGH
Requires adding per-peer and per-block state tracking.

---

## Issue #4: Mining During IBD (POTENTIAL)

### Severity: LOW
**Impact:** Wastes CPU cycles, potential chain instability

### Problem Description
Need to verify if the CPU miner checks IBD before generating blocks.

### Bitcoin Core Behavior
Bitcoin Core's miner does NOT generate blocks during IBD because:
- The chain is not caught up yet
- Blocks would be stale by the time the node syncs
- Wastes resources

**Reference:** `generateBlocks()` in Bitcoin Core checks `IsInitialBlockDownload()`.

### Investigation Required
**File:** `src/mining/cpu_miner.cpp`

Check if `CPUMiner::MineBlock()` or the mining loop checks:
```cpp
if (chainstate_manager_.IsInitialBlockDownload()) {
    // Don't mine during IBD
    return;
}
```

### Fix Complexity: TRIVIAL (if issue exists)

---

## Issue #5: No Block Source Tracking

### Severity: LOW
**Impact:** Cannot implement proper relay deduplication

### Problem Description
The codebase does not track which peer sent us each block. This information is needed for:
- Not relaying blocks back to the source
- Debugging sync issues
- Peer scoring/reputation

### Bitcoin Core Behavior
```cpp
// When receiving a block from peer
mapBlockSource[block_hash] = peer_id;

// When relaying
for (auto& peer : peers) {
    if (peer.id != mapBlockSource[block_hash]) {  // Don't send back
        peer.RelayBlock(block_hash);
    }
}
```

### Fix Complexity: MEDIUM
Requires adding state tracking in BlockManager or HeaderSyncManager.

---

## Priority Fix Order

### Phase 1: Critical Fixes (ASAP)
1. **Issue #1: Add IBD check to block relay** ⚠️ TRIVIAL FIX
   - Single line change
   - Immediate bandwidth savings
   - No architectural changes needed

### Phase 2: Important Fixes (Next Release)
2. **Issue #2: Fix reorg block relay** 📋 MEDIUM FIX
   - Requires design decision on notification architecture
   - Consider implementing timestamp-based relay logic
   - May need to refactor notification system

### Phase 3: Optimization (Future)
3. **Issue #3: Add block relay deduplication** 🔧 HIGH COMPLEXITY
   - Implement per-peer inventory tracking
   - Add block source tracking
   - Requires significant state management

4. **Issue #4: Verify mining during IBD** 🔍 INVESTIGATION
   - Check if issue exists
   - Add check if needed

5. **Issue #5: Add block source tracking** 📊 MEDIUM COMPLEXITY
   - Enables Issue #3 fix
   - Improves debugging capabilities

---

## Testing Plan

### After Fixing Issue #1 (IBD Check)
**Test:** Sync a fresh node from genesis
**Expected:** No "Relaying block" logs during initial sync
**Verification:** Check bandwidth usage vs. current behavior

### After Fixing Issue #2 (Reorg Relay)
**Test:** Create a 100-block chain, trigger reorg from genesis
**Expected:** Only relay the new tip, not all 100 blocks
**Verification:** Count INV messages sent

### After Fixing Issue #3 (Deduplication)
**Test:** Have 3 peers, receive block from peer A
**Expected:** Relay to peers B and C only, not back to A
**Verification:** Check peer message logs

---

## Bandwidth Impact Analysis

### Current Behavior (Syncing 1000 blocks)
```
Peers:           6
Blocks synced:   1000
Relays sent:     1000 × 6 = 6000 INV messages
Average INV:     ~100 bytes
Total waste:     600 KB of unnecessary INVs
```

### After Fix (Issue #1)
```
Blocks relayed:  0 during IBD
Bandwidth saved: 600 KB per sync
```

### Network-wide Impact
With 100 syncing nodes:
```
Current:  100 × 6000 = 600,000 unnecessary INVs
After:    0 INVs during sync
Savings:  ~60 MB network-wide per sync cycle
```

---

## References

### Bitcoin Core Source
- `net_processing.cpp`: PeerManagerImpl::ProcessMessage()
- `validation.cpp`: ActivateBestChain(), BlockConnected signals
- `net.h`: CNode inventory tracking structures

### CoinbaseChain Code
- `src/application.cpp:84-89`: Block relay subscription
- `src/chain/chainstate_manager.cpp:505`: BlockConnected notification
- `src/network/block_relay_manager.cpp:119-148`: RelayBlock implementation
- `include/chain/chainstate_manager.hpp:74`: IsInitialBlockDownload()

---

## Change Log

### 2025-10-24
- Initial document created
- Identified 5 distinct issues
- Prioritized fixes
- Added bandwidth impact analysis
