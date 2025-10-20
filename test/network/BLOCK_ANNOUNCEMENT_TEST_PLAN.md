# Per-Peer Block Announcement Protocol - Comprehensive Test Plan

## Overview
This document outlines the test plan for validating the per-peer block announcement architecture implemented in network_manager.cpp and peer.hpp. The implementation follows Bitcoin Core's per-peer announcement queue pattern.

## Architecture Summary

### Key Components
1. **Per-Peer Queue** (`peer.hpp:169-172`): Each peer has `blocks_for_inv_relay_` vector
2. **Queuing Functions**:
   - `announce_tip_to_peers()`: Adds tip to ALL peers' queues periodically (every 30s)
   - `announce_tip_to_peer()`: Adds tip to SINGLE peer when they become READY
   - Both use per-peer deduplication
3. **Flushing** (`flush_block_announcements()`): Sends queued INV messages
4. **Immediate Relay** (`relay_block()`): Broadcasts INV immediately (bypasses queue)

### Current Test Coverage
- ✅ INV message security (MAX_INV_SIZE enforcement)
- ✅ Block propagation during sync (IBD tests)
- ✅ Network partition healing
- ✅ Chain reorgs and invalidation
- ❌ Per-peer announcement queue behavior (MISSING)
- ❌ Announcement flushing mechanism (MISSING)
- ❌ Per-peer deduplication (MISSING)

## Test Plan

### HIGH PRIORITY Tests (Must Have)

#### HIGH-1: Per-Peer Queue Isolation
**Purpose**: Verify blocks are added to individual peer queues, not global broadcast

**Test Scenario**:
```
- Setup: 3 nodes (node1, node2, node3)
- node1 mines a block
- node2 and node3 connect to node1
- Call announce_tip_to_peers() on node1
- Verify: node2's queue has block, node3's queue has block
- Verify: Queues are independent (modifying node2's queue doesn't affect node3)
```

**Expected Behavior**:
- Each peer has independent queue
- Block appears in both peers' queues
- Mutex protects per-peer queue access

**Location**: `test/network/block_announcement_tests.cpp`

---

#### HIGH-2: Per-Peer Deduplication
**Purpose**: Verify same block not queued twice for same peer

**Test Scenario**:
```
- Setup: 2 nodes (node1, node2)
- node1 mines blockA
- node2 connects to node1
- Call announce_tip_to_peer(node2) -> blockA added to node2's queue
- Call announce_tip_to_peer(node2) again -> blockA NOT added (duplicate)
- Verify: node2's queue has exactly 1 copy of blockA
```

**Expected Behavior**:
- Duplicate blocks filtered at queue level
- std::find() check in announce_tip_to_peer() prevents duplicates
- Queue size remains 1 after multiple announcements

**Edge Cases**:
- Duplicate announcement before flush
- Duplicate announcement after flush (should be allowed)

---

#### HIGH-3: Flush Block Announcements
**Purpose**: Verify flush_block_announcements() sends INV messages and clears queue

**Test Scenario**:
```
- Setup: 2 nodes (node1, node2) connected
- node1 mines blockA
- Add blockA to node2's announcement queue (via announce_tip_to_peer)
- Call flush_block_announcements()
- Verify: node2 receives INV message with blockA
- Verify: node1's internal queue for node2 is now empty
```

**Expected Behavior**:
- INV message sent with all queued blocks
- Queue cleared after successful flush
- Only READY peers receive flushes

**Test Implementation**:
- Capture network messages during flush
- Parse INV message to verify block hash
- Check peer's queue is empty after flush

---

#### HIGH-4: Announce Tip to New Peer on READY
**Purpose**: Verify new peer receives tip when transitioning to READY state

**Test Scenario**:
```
- Setup: 2 nodes (node1, node2)
- node1 mines 5 blocks (height=5)
- node2 connects to node1
- Wait for handshake (VERSION/VERACK)
- When node2 becomes READY, verify announce_tip_to_peer() called
- Verify: node2's queue contains tip hash
- Verify: Subsequent flush sends INV to node2
```

**Expected Behavior**:
- announce_tip_to_peer() called when peer state transitions to READY
- Tip added to new peer's announcement queue
- New peer discovers network tip via announcement

**Critical Path**:
- This is how new peers learn about current tip
- Ensures new peers aren't left behind

---

#### HIGH-5: Disconnect Before Flush
**Purpose**: Verify disconnect before flush doesn't crash and properly cleans up

**Test Scenario**:
```
- Setup: 2 nodes (node1, node2) connected
- node1 mines blockA
- Add blockA to node2's announcement queue
- node2 disconnects (before flush)
- Call flush_block_announcements()
- Verify: No crash, no memory leaks
- Verify: node2's queue is cleaned up
```

**Expected Behavior**:
- flush_block_announcements() skips disconnected peers
- No dangling pointers or memory leaks
- Graceful handling of peer disconnect

**Safety Checks**:
- `if (!peer || !peer->is_connected())` check in flush
- Queue is part of Peer object (destroyed with peer)

---

### MEDIUM PRIORITY Tests (Should Have)

#### MED-1: Multiple Blocks Batched in Single INV
**Purpose**: Verify multiple blocks can be announced together in one INV message

**Test Scenario**:
```
- Setup: 2 nodes (node1, node2) connected
- node1 mines 5 blocks rapidly
- Add all 5 blocks to node2's announcement queue
- Call flush_block_announcements()
- Verify: Single INV message with 5 inventory items
```

**Expected Behavior**:
- All queued blocks batched into one INV message
- Efficient: 1 network message instead of 5
- Respects MAX_INV_SIZE (50000 items)

---

#### MED-2: Multi-Peer Propagation (3-5 Nodes)
**Purpose**: Verify block propagates correctly to 3+ peers

**Test Scenario**:
```
- Setup: 5 nodes (node1 with 4 peers)
- node1 mines blockA
- Call announce_tip_to_peers()
- Flush announcements
- Verify: Each of 4 peers receives exactly ONE INV for blockA
- Verify: No peer receives duplicate INV
```

**Expected Behavior**:
- Block announced to all connected READY peers
- Each peer receives exactly one announcement
- Per-peer deduplication prevents duplicates

---

#### MED-3: Periodic Re-Announcement
**Purpose**: Verify periodic re-announcements via announce_tip_to_peers()

**Test Scenario**:
```
- Setup: 2 nodes (node1, node2) connected
- node1 mines blockA
- First announcement: blockA added to node2's queue
- Flush (node2 receives INV)
- Time passes (30+ seconds in simulation)
- announce_tip_to_peers() called again (simulating periodic maintenance)
- Verify: blockA added to node2's queue again (allowed after flush)
- Verify: node2 receives second INV
```

**Expected Behavior**:
- Periodic re-announcements help with partition healing
- Re-announcement allowed AFTER flush (queue was cleared)
- Helps ensure network convergence

**Use Case**:
- Peer missed original announcement due to network issues
- Network partition healed, need to re-sync tip

---

#### MED-4: Rapid Sequential Blocks (10+ Blocks)
**Purpose**: Verify queue behavior under load with rapid block production

**Test Scenario**:
```
- Setup: 2 nodes (node1, node2) connected
- node1 mines 20 blocks in rapid succession
- Add all 20 blocks to node2's announcement queue
- Flush announcements
- Verify: All 20 blocks announced correctly
- Verify: No memory issues, no duplicates
```

**Expected Behavior**:
- Queue handles large number of pending blocks
- All blocks announced in order
- No performance degradation

---

#### MED-5: Mixed Peer States
**Purpose**: Verify only READY peers receive announcements

**Test Scenario**:
```
- Setup: 3 nodes
  - node1: source
  - node2: READY state (handshake complete)
  - node3: VERSION_SENT state (handshake incomplete)
- node1 mines blockA
- Call announce_tip_to_peers()
- Flush announcements
- Verify: node2 receives INV (READY)
- Verify: node3 does NOT receive INV (not READY)
```

**Expected Behavior**:
- Only READY peers receive block announcements
- Peers in handshake stages are skipped
- State check: `peer->state() == PeerState::READY`

---

### LOW PRIORITY Tests (Nice to Have)

#### LOW-1: Immediate Relay vs Queued Announcement
**Purpose**: Verify relay_block() bypasses queue system

**Test Scenario**:
```
- Compare relay_block() (immediate) vs announce_tip_to_peers() (queued)
- Verify immediate relay doesn't go through queue
- Verify both mechanisms can coexist
```

---

#### LOW-2: Thread Safety
**Purpose**: Verify block_inv_mutex_ protects concurrent access

**Test Scenario**:
```
- Concurrent announce_tip_to_peer() calls from multiple threads
- Verify no data corruption
- Verify mutex properly protects queue
```

---

#### LOW-3: Memory Leak on Disconnect
**Purpose**: Verify no memory leaks when peer disconnects with full queue

**Test Scenario**:
```
- Add 100 blocks to peer's queue
- Disconnect peer
- Run under Valgrind/ASAN
- Verify no memory leaks
```

---

## Test Implementation Strategy

### Phase 1: Infrastructure (Task 1)
Create `test/network/block_announcement_tests.cpp` with:
- Test fixture for announcement testing
- Helper functions to inspect peer queues
- Network message capture utilities
- SimulatedNetwork with zero latency for deterministic testing

### Phase 2: HIGH Priority Tests (Tasks 2-6)
Implement 5 critical tests covering:
- Per-peer queue isolation
- Deduplication
- Flushing mechanism
- New peer announcement
- Disconnect safety

### Phase 3: MEDIUM Priority Tests (Tasks 7-11)
Implement 5 important tests covering:
- Batch announcements
- Multi-peer scenarios
- Periodic re-announcements
- Load testing
- State handling

### Phase 4: Validation (Tasks 12-13)
- Build all tests
- Run full test suite
- Ensure 100% pass rate
- Verify no regressions in existing tests

## Success Criteria

### Coverage Goals
- **Per-Peer Queue**: 100% coverage of queue operations
- **Announcement Functions**: 100% coverage of announce_tip_* and flush_*
- **Edge Cases**: All disconnect/state-transition scenarios covered
- **Integration**: Multi-node scenarios (3-5 nodes) tested

### Quality Metrics
- All HIGH priority tests must pass (5/5)
- At least 80% of MEDIUM priority tests passing (4/5+)
- Zero regressions in existing 318 tests
- Zero memory leaks (ASAN clean)
- Zero data races (TSAN clean, if applicable)

## Test Tags

Recommended Catch2 tags for organization:
- `[block_announcement]` - All announcement tests
- `[per_peer_queue]` - Queue-specific tests
- `[announcement_flush]` - Flush mechanism tests
- `[functional][network]` - Integration tests
- `[quick]` - Fast-running tests (&lt;1s)
- `[network]` - All network tests

## References

### Production Code Files
- `include/network/peer.hpp:169-172` - Per-peer queue definition
- `include/network/network_manager.hpp:76-82` - Announcement function declarations
- `src/network/network_manager.cpp:1362-1488` - Announcement implementations

### Existing Test Files
- `test/network/invalidateblock_functional_tests.cpp` - Block propagation patterns
- `test/network/sync_ibd_tests.cpp` - Sync testing patterns
- `test/network/simulated_node.cpp` - Test infrastructure

## Estimated Timeline

- **Phase 1** (Infrastructure): 2-3 hours
- **Phase 2** (HIGH tests): 4-6 hours
- **Phase 3** (MEDIUM tests): 3-5 hours
- **Phase 4** (Validation): 1-2 hours

**Total**: 10-16 hours of focused development

## Risk Assessment

### LOW RISK
- Infrastructure setup (well-established patterns exist)
- HIGH priority tests (clear requirements, isolated tests)

### MEDIUM RISK
- Multi-peer tests (coordination complexity)
- Thread safety tests (potential flakiness)

### MITIGATION
- Use SimulatedNetwork with zero latency for determinism
- Follow existing test patterns from invalidateblock tests
- Extensive logging for debugging test failures
- Incremental development (one test at a time)

---

**Document Version**: 1.0
**Created**: 2025-10-20
**Last Updated**: 2025-10-20
**Status**: Ready for Implementation
