# MessageRouter Bitcoin Core Parity Action Plan

**Owner**: Mike  
**Target**: Bitcoin Core v27.0 behavior  
**Status**: In Progress  
**Date**: 2025-10-31

---

## Executive Summary

MessageRouter has **critical logic inversions** and **multiple race conditions** that prevent proper P2P network formation. This plan fixes bugs in priority order, adds Bitcoin Core parity, and establishes verification through tests.

---

## Severity Classification

### **P0 - Network Breaking (Fix Immediately)**
These bugs prevent the network from functioning correctly:

1. **GETADDR inbound/outbound inversion** (line 166)
   - Current: Only responds to inbound peers
   - Bitcoin Core: Only responds to outbound peers
   - Impact: Outbound peers can't learn addresses → network fragmentation

2. **ADDR size validation missing** (line 107)
   - Current: No cap enforcement on incoming ADDR messages
   - Bitcoin Core: Rejects messages > MAX_ADDR_SIZE (1000)
   - Impact: DoS vector - peers can send 50,000 addresses

3. **ADDR peer validation race condition** (lines 101-112)
   - Current: Feeds AddressManager before checking peer validity
   - Bitcoin Core: Validates peer first, then processes
   - Impact: Privacy leak + potential null pointer dereference

### **P1 - Correctness & Safety (Fix Before Production)**
These bugs cause incorrect behavior but don't completely break functionality:

4. **Dynamic cast without null checks** (lines 35, 44, 49, 54)
5. **handle_verack doesn't check if peer is connected** (line 65)
6. **VERACK tip announcement without chain validation** (lines 92-96)
7. **Exception safety violations** (no try/catch in handlers)
8. **Lock held during network I/O** (line 256 send_message outside lock check)

### **P2 - Performance & Robustness (Fix During Optimization)**
These impact performance under load:

9. **O(n) eviction algorithm in learned address cache** (lines 146-155)
10. **Hash collision risk in AddressKey** (FNV-1a with 250k entries)
11. **Lock contention** (single mutex for hot paths)
12. **Stale entries in recent_addrs_ ring** (no TTL pruning)

### **P3 - Observability & Maintainability**
Nice-to-haves for production monitoring:

13. **No metrics/counters** (addresses learned, suppressed, etc.)
14. **Timing attack on echo suppression** (low severity)
15. **IP-based rate limiting for Sybil attacks**

---

## Phase 1: Critical Bug Fixes (P0)

**Estimated Time**: 2-4 hours  
**Test Impact**: Requires new parity tests

### 1.1 Fix GETADDR Logic Inversion

**File**: `src/network/message_router.cpp`  
**Lines**: 160-257

**Changes**:
```cpp
// OLD (line 166):
if (!peer->is_inbound()) {
  LOG_NET_TRACE("Ignoring GETADDR from outbound peer {}", peer->id());
  return true;
}

// NEW:
// Bitcoin Core v27.0: Only respond to OUTBOUND peers (fingerprinting prevention)
if (peer->is_inbound()) {
  LOG_NET_TRACE("Ignoring GETADDR from inbound peer {} (fingerprinting protection)", 
                peer->id());
  return true;
}
```

**Verification**:
- Run existing test: `message_router_tests.cpp` (should still pass with inverted logic)
- Add new test: `test/parity/getaddr_inbound_outbound_test.cpp`

---

### 1.2 Add ADDR Size Validation

**File**: `src/network/message_router.cpp`  
**Lines**: 101-108

**Changes**:
```cpp
bool MessageRouter::handle_addr(PeerPtr peer, message::AddrMessage* msg) {
  if (!msg || !addr_manager_) {
    return false;
  }

  // NEW: Validate size against protocol limit (Bitcoin Core: Misbehaving if oversized)
  if (msg->addresses.size() > protocol::MAX_ADDR_SIZE) {
    LOG_NET_WARN("Peer {} sent oversized ADDR message ({} addrs, max {}), truncating",
                 peer ? peer->id() : -1, msg->addresses.size(), protocol::MAX_ADDR_SIZE);
    msg->addresses.resize(protocol::MAX_ADDR_SIZE);
    // TODO: Consider misbehavior scoring via PeerManager once implemented
  }

  // Continue with existing logic...
```

**Verification**:
- Add test: `test/unit/message_router_tests.cpp` - "ADDR message with excessive addresses"
- Fuzz test: Send ADDR with 50,000 addresses

---

### 1.3 Fix ADDR Peer Validation Order

**File**: `src/network/message_router.cpp`  
**Lines**: 101-157

**Changes**:
```cpp
bool MessageRouter::handle_addr(PeerPtr peer, message::AddrMessage* msg) {
  if (!msg || !addr_manager_) {
    return false;
  }

  // NEW: Validate peer FIRST (Bitcoin Core checks fSuccessfullyConnected before processing)
  if (!peer || !peer->successfully_connected()) {
    LOG_NET_TRACE("Ignoring ADDR from non-connected peer");
    return true; // Not an error, just gated
  }

  // NEW: Size validation (see 1.2)
  if (msg->addresses.size() > protocol::MAX_ADDR_SIZE) {
    // ... (as above)
  }

  // NOW safe to feed AddressManager
  addr_manager_->add_multiple(msg->addresses);

  // Continue with echo suppression logic...
```

**Verification**:
- Test: ADDR from non-handshaked peer should be ignored
- Test: ADDR from disconnected peer should be ignored

---

## Phase 2: Safety & Correctness (P1)

**Estimated Time**: 3-5 hours  
**Test Impact**: Extends existing tests

### 2.1 Add Dynamic Cast Null Checks

**File**: `src/network/message_router.cpp`  
**Lines**: 34-56

**Pattern** (apply to all dynamic_cast sites):
```cpp
if (command == protocol::commands::ADDR) {
  auto *addr_msg = dynamic_cast<message::AddrMessage *>(msg.get());
  if (!addr_msg) {
    LOG_NET_ERROR("Failed to cast ADDR message from peer={}", peer->id());
    return false; // Protocol violation
  }
  return handle_addr(peer, addr_msg);
}
```

**Affected Lines**: 35 (ADDR), 44 (INV), 49 (HEADERS), 54 (GETHEADERS)

---

### 2.2 Add Connection Check in handle_verack

**File**: `src/network/message_router.cpp`  
**Lines**: 63-99

```cpp
bool MessageRouter::handle_verack(PeerPtr peer) {
  // NEW: Verify peer is still connected
  if (!peer || !peer->is_connected()) {
    LOG_NET_TRACE("Ignoring VERACK from disconnected peer");
    return true;
  }

  // Peer has completed handshake...
  if (!peer->successfully_connected()) {
    return true;
  }
  // ... rest unchanged
```

---

### 2.3 Add Chain Validation Before Tip Announcement

**File**: `src/network/message_router.cpp`  
**Lines**: 92-96

```cpp
// Announce our tip to this peer immediately (Bitcoin Core does this with no time throttling)
// This allows peers to discover our chain and request headers if we're ahead
if (block_relay_manager_) {
  // NEW: Only announce if we have meaningful chain data
  // Avoid sync thrashing by not announcing genesis to peers ahead of us
  if (peer->start_height() == 0 || peer->start_height() <= chainstate_manager_->GetChainHeight()) {
    block_relay_manager_->AnnounceTipToPeer(peer.get());
  } else {
    LOG_NET_TRACE("Skipping tip announcement to peer {} (their height {} > ours {})",
                  peer->id(), peer->start_height(), chainstate_manager_->GetChainHeight());
  }
}
```

**Note**: Requires passing ChainstateManager to MessageRouter constructor.

---

### 2.4 Add Exception Safety Wrappers

**File**: `src/network/message_router.cpp`  
**Lines**: All handler functions

**Pattern**:
```cpp
bool MessageRouter::handle_addr(PeerPtr peer, message::AddrMessage* msg) {
  try {
    // ... existing logic
  } catch (const std::exception& e) {
    LOG_NET_ERROR("Exception in handle_addr: {} (peer={})", e.what(), peer ? peer->id() : -1);
    return false;
  }
}
```

Apply to: `handle_addr`, `handle_getaddr`, `handle_verack`

---

### 2.5 Add Connection Check Before send_message

**File**: `src/network/message_router.cpp`  
**Line**: 256

```cpp
  }  // mutex unlocked

  // NEW: Verify peer still connected before sending (TOCTOU protection)
  if (!peer->is_connected()) {
    LOG_NET_TRACE("Peer {} disconnected before GETADDR response could be sent", peer_id);
    return true; // Not an error, just too late
  }

  peer->send_message(std::move(response));
  return true;
```

---

## Phase 3: Performance Optimization (P2)

**Estimated Time**: 4-6 hours  
**Test Impact**: Benchmark tests required

### 3.1 Replace O(n) Eviction with Ordered Map

**File**: `include/network/message_router.hpp` + `src/network/message_router.cpp`  
**Lines**: 70, 146-155

**Current**:
```cpp
using LearnedMap = std::unordered_map<AddressKey, LearnedEntry, AddressKey::Hasher>;
```

**New**:
```cpp
// Ordered by last_seen_s for O(1) eviction of oldest entries
struct LearnedCache {
  std::unordered_map<AddressKey, LearnedEntry, AddressKey::Hasher> by_key;
  std::multimap<int64_t, AddressKey> by_age; // last_seen_s -> key
  
  void insert(const AddressKey& key, const LearnedEntry& entry);
  void evict_oldest();
  size_t size() const { return by_key.size(); }
};
```

**Impact**: O(n) → O(log n) eviction

---

### 3.2 Fix AddressKey Hash Collisions

**File**: `include/network/message_router.hpp`  
**Lines**: 45-62

**Current**: FNV-1a 64-bit hash (collision risk at 250k entries)

**Option A** (Zero collision risk):
```cpp
struct AddressKey {
  std::array<uint8_t,16> ip{};
  uint16_t port{0};
  
  // Use structural comparison instead of hash
  auto operator<=>(const AddressKey&) const = default;
};

// Use std::map instead of unordered_map
using LearnedMap = std::map<AddressKey, LearnedEntry>;
```

**Option B** (Keep unordered_map, use 128-bit hash):
```cpp
struct Hasher {
  size_t operator()(const AddressKey& k) const noexcept {
    // Use std::hash on the entire struct (implementation-defined but better)
    size_t h = 0;
    for (size_t i = 0; i < k.ip.size(); ++i) {
      h ^= std::hash<uint8_t>{}(k.ip[i]) << (i % sizeof(size_t));
    }
    h ^= std::hash<uint16_t>{}(k.port);
    return h;
  }
};
```

**Recommendation**: Option A (structural comparison) for correctness.

---

### 3.3 Split Lock Granularity

**File**: `include/network/message_router.hpp`  
**Lines**: 85

**Current**: Single `addr_mutex_` protects all data

**New**:
```cpp
// Separate locks for independent data structures
mutable std::mutex getaddr_mutex_;        // Protects getaddr_replied_
mutable std::mutex learned_mutex_;        // Protects learned_addrs_by_peer_
mutable std::mutex recent_mutex_;         // Protects recent_addrs_
```

**Impact**: Reduces contention on hot paths (ADDR handling vs GETADDR response)

---

### 3.4 Add TTL Pruning to recent_addrs_

**File**: `src/network/message_router.cpp`  
**Lines**: 139-143

```cpp
// Global recent ring (O(1) eviction)
// NEW: Prune stale entries before insertion
while (!recent_addrs_.empty() && 
       (now_s - recent_addrs_.front().timestamp) > RECENT_ADDRS_TTL_SEC) {
  recent_addrs_.pop_front();
}

recent_addrs_.push_back(ta);
if (recent_addrs_.size() > RECENT_ADDRS_MAX) {
  recent_addrs_.pop_front();
}
```

**Add constant**:
```cpp
static constexpr int64_t RECENT_ADDRS_TTL_SEC = 86400; // 24 hours
```

---

## Phase 4: Observability (P3)

**Estimated Time**: 2-3 hours  
**Test Impact**: None (metrics only)

### 4.1 Add Metrics Counters

**File**: New file `include/network/message_router_metrics.hpp`

```cpp
struct MessageRouterMetrics {
  std::atomic<uint64_t> addr_messages_received{0};
  std::atomic<uint64_t> addr_addresses_learned{0};
  std::atomic<uint64_t> addr_addresses_rejected{0};
  
  std::atomic<uint64_t> getaddr_requests_received{0};
  std::atomic<uint64_t> getaddr_requests_ignored_inbound{0};
  std::atomic<uint64_t> getaddr_requests_ignored_repeated{0};
  std::atomic<uint64_t> getaddr_responses_sent{0};
  std::atomic<uint64_t> getaddr_addresses_suppressed{0};
  
  void log_summary() const;
};
```

Integrate into MessageRouter and log periodically.

---

### 4.2 IP-Based Rate Limiting

**File**: `src/network/message_router.cpp`  
**Lines**: Add new state to handle_getaddr

```cpp
// Per-IP rate limiting (anti-Sybil)
struct GetAddrIPState {
  int64_t last_request_time;
  uint32_t request_count;
};

// NEW member variable:
std::unordered_map<std::string, GetAddrIPState> getaddr_ip_limiter_;
static constexpr int64_t GETADDR_IP_COOLDOWN_SEC = 3600; // 1 hour
static constexpr uint32_t GETADDR_IP_MAX_PER_HOUR = 10;
```

Check in `handle_getaddr()` before processing.

---

## Phase 5: Testing & Verification

**Estimated Time**: 4-6 hours

### 5.1 Unit Tests (Extend existing)

**File**: `test/unit/message_router_tests.cpp`

Add test cases:
- `GETADDR_inbound_ignored` (NEW)
- `GETADDR_outbound_responds` (NEW - update existing test)
- `ADDR_oversized_truncated` (NEW)
- `ADDR_peer_disconnected_ignored` (NEW)
- `dynamic_cast_failure_handling` (NEW)
- `concurrent_addr_getaddr` (NEW - multithreading)

---

### 5.2 Parity Tests (Bitcoin Core equivalence)

**New Directory**: `test/parity/`

**New File**: `test/parity/addr_relay_parity_test.cpp`

Test scenarios:
1. **getaddr_once_per_connection**: Second GETADDR from same peer gets no response
2. **getaddr_inbound_ignored**: Inbound peer GETADDR gets no response
3. **getaddr_outbound_allowed**: Outbound peer GETADDR gets response
4. **addr_oversized_rejected**: ADDR with >1000 entries is truncated
5. **addr_before_handshake_ignored**: ADDR before VERACK is ignored
6. **echo_suppression**: ADDR learned from peer A not echoed back to peer A

---

### 5.3 Fuzz Tests

**New File**: `test/fuzz/message_router_fuzz.cpp`

Targets:
- `fuzz_addr_message`: Random address lists, sizes, timestamps
- `fuzz_getaddr_timing`: Concurrent GETADDR from multiple peers
- `fuzz_disconnect_during_handling`: Disconnect during message processing

---

### 5.4 Integration Tests

**File**: `test/integration/network_integration_test.cpp`

Scenarios:
- Multi-node network formation with address relay
- Address propagation across 10+ nodes
- GETADDR behavior with mixed inbound/outbound peers
- Echo suppression verification across network

---

## Rollout Plan

### Step 1: P0 Fixes + Tests (Week 1)
1. Fix GETADDR logic (1.1)
2. Add ADDR size validation (1.2)
3. Fix ADDR peer validation (1.3)
4. Add basic parity tests (5.2)
5. **RUN FULL TEST SUITE** - ensure no regressions

### Step 2: P1 Safety (Week 2)
1. Dynamic cast checks (2.1)
2. Exception safety (2.4)
3. Connection checks (2.2, 2.5)
4. Tip announcement validation (2.3)
5. **RUN FULL TEST SUITE**

### Step 3: P2 Performance (Week 3)
1. Fix eviction algorithm (3.1)
2. Fix hash collisions (3.2)
3. Split lock granularity (3.3)
4. Add TTL pruning (3.4)
5. **BENCHMARK before/after**

### Step 4: P3 Observability (Week 4)
1. Add metrics (4.1)
2. Add IP rate limiting (4.2)
3. Integration tests (5.4)
4. **DEPLOY to testnet**

---

## Success Criteria

- [ ] All existing tests pass
- [ ] New parity tests pass (6+ scenarios)
- [ ] Fuzz tests run 1M iterations without crashes
- [ ] Integration test: 10-node network fully connected within 60s
- [ ] No data races (verify with ThreadSanitizer)
- [ ] No memory leaks (verify with AddressSanitizer)
- [ ] Performance: ADDR handling <5ms p99, GETADDR response <50ms p99
- [ ] Compatibility matrix updated: GETADDR/ADDR status = "Matches"

---

## Risk Mitigation

### Rollback Plan
- Git branch per phase: `fix/p0-critical`, `fix/p1-safety`, etc.
- Tag each phase: `v0.x.0-phase1`, `v0.x.0-phase2`, etc.
- Keep old implementation in `message_router_legacy.cpp` for 1 release

### Testing Strategy
- Run tests after EACH change (not just phase)
- Use regtest network for integration testing
- Deploy to isolated testnet before mainnet
- Monitor metrics for 7 days before declaring stable

---

## Documentation Updates

### Files to Update
1. `docs/compatibility-matrix.md` - Change GETADDR/ADDR status to "Matches"
2. `docs/PROTOCOL_SPECIFICATION.md` - Add echo suppression details
3. `README.md` - Update testing instructions
4. `CHANGELOG.md` - Document breaking changes (GETADDR behavior flip)

---

## Open Questions

1. **ChainstateManager dependency**: MessageRouter needs chain height for tip announcement validation (2.3). Add to constructor?
2. **Misbehavior scoring**: Should oversized ADDR trigger ban via PeerManager? (Currently just truncates)
3. **ADDRV2 support**: Out of scope per compatibility matrix, but consider for future?
4. **Lock-free alternatives**: Worth exploring for recent_addrs_ ring? (P2 could use lock-free queue)

---

## Approval & Sign-off

**Ready to proceed?** Phases 1-2 are **must-fix** before any production use. Phases 3-4 are optimizations.

**Estimated Total Time**: 15-20 hours (2-3 weeks part-time)

**Recommended**: Execute Phase 1 immediately (2-4 hours), then reassess based on test results.

---

## Progress Tracking

### Phase 1 (P0 Fixes)
- [ ] 1.1 Fix GETADDR logic inversion
- [ ] 1.2 Add ADDR size validation
- [ ] 1.3 Fix ADDR peer validation order
- [ ] Run existing tests
- [ ] Add new unit tests

### Phase 2 (P1 Safety)
- [ ] 2.1 Dynamic cast null checks
- [ ] 2.2 Connection check in handle_verack
- [ ] 2.3 Chain validation before tip announcement
- [ ] 2.4 Exception safety wrappers
- [ ] 2.5 Connection check before send_message

### Phase 3 (P2 Performance)
- [ ] 3.1 Replace O(n) eviction
- [ ] 3.2 Fix hash collisions
- [ ] 3.3 Split lock granularity
- [ ] 3.4 Add TTL pruning

### Phase 4 (P3 Observability)
- [ ] 4.1 Add metrics
- [ ] 4.2 IP-based rate limiting
