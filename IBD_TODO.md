# Initial Block Download (IBD) - Missing Implementation

## Problem
Node1 connects to Node0 (which has 50 blocks) but doesn't sync. Node1 remains at genesis.

## Root Cause
We only send GETHEADERS in response to INV messages. We don't proactively request headers after handshake completes.

## Unicity's Approach (from net_processing.cpp)

### 1. Handshake Completion Tracking
**File**: `net_processing.cpp:3658`
```cpp
// In ProcessMessage for VERACK:
pfrom.fSuccessfullyConnected = true;
```

### 2. Block Outgoing Messages Until Handshake Complete
**File**: `net_processing.cpp:5557`
```cpp
// In SendMessages:
if (!pto->fSuccessfullyConnected || pto->fDisconnect)
    return true;  // Don't send anything until handshake complete
```

### 3. Initial Headers Sync (Proactive GETHEADERS)
**File**: `net_processing.cpp:5609-5634`
```cpp
// In SendMessages, after handshake is complete:
// Only actively request headers from a single peer, unless we're close to today
if ((nSyncStarted == 0 && sync_blocks_and_headers_from_peer) ||
    m_chainman.m_best_header->Time() > NodeClock::now() - 24h) {

    const CBlockIndex* pindexStart = m_chainman.m_best_header;
    if (pindexStart->pprev)
        pindexStart = pindexStart->pprev;

    if (MaybeSendGetHeaders(*pto, GetLocator(pindexStart), *peer)) {
        LogPrint(BCLog::NET, "initial getheaders (%d) to peer=%d\n",
                 pindexStart->nHeight, pto->GetId());

        state.fSyncStarted = true;  // Mark this peer as our sync peer
        peer->m_headers_sync_timeout = current_time + HEADERS_DOWNLOAD_TIMEOUT_BASE + ...;
        nSyncStarted++;
    }
}
```

### 4. Limit to One Sync Peer
**Purpose**: Bandwidth efficiency during IBD
**Mechanism**: `nSyncStarted` counter, only one peer gets initial getheaders

## What We Need To Implement

### Option A: Add SendMessages-style Loop
1. Add `successfully_connected` flag to Peer
2. Create periodic `SendMessages()` function in NetworkManager
3. Check handshake completion and send initial GETHEADERS
4. Track which peer we're syncing from (`sync_started` flag)

### Option B: Use Callback on VERACK
1. Add `PeerReadyHandler` callback to Peer class
2. Call it in `handle_verack()` when peer becomes READY
3. NetworkManager receives callback and sends GETHEADERS
4. Simpler but less like Unicity

### Option C: Hybrid Approach (Recommended)
1. Add `successfully_connected` bool to Peer (matches Unicity)
2. Set it in `handle_verack()`
3. Add periodic sync check in NetworkManager (lightweight)
4. On first READY outbound peer, send GETHEADERS

## Current State vs Unicity

| Feature | Our Implementation | Unicity |
|---------|-------------------|---------|
| Track handshake complete | ✅ PeerState::READY | ✅ fSuccessfullyConnected |
| Block messages until ready | ❌ No | ✅ Yes (line 5557) |
| Proactive GETHEADERS | ❌ Only on INV | ✅ After handshake (line 5622) |
| Limit sync peers | ❌ No | ✅ nSyncStarted counter |
| Sync timeout | ❌ No | ✅ m_headers_sync_timeout |

## Files To Modify

1. **include/network/peer.hpp**
   - Add `successfully_connected()` getter
   - Add `successfully_connected_` member variable

2. **src/network/peer.cpp**
   - Set `successfully_connected_ = true` in `handle_verack()`

3. **include/network/network_manager.hpp**
   - Add `sync_peer_id_` to track which peer we're syncing from
   - Add periodic sync check method

4. **src/network/network_manager.cpp**
   - Implement initial GETHEADERS logic
   - Check for READY outbound peers without sync started
   - Send GETHEADERS to initiate IBD

## Next Steps

1. Choose approach (recommend Option C)
2. Implement successfully_connected tracking
3. Add initial sync logic to NetworkManager
4. Test with p2p_ibd.py test
