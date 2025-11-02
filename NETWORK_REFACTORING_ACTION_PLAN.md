# Network Library Refactoring - Action Plan
**Version**: 2.0
**Date**: 2025-11-02 (Updated)
**Status**: DRAFT - Not Started
**Owner**: Development Team
**Timeline**: 7-9 weeks (phased approach)
**Change Log**:
- v1.0 - Initial action plan with EventBus
- v1.1 - Replaced EventBus with NetworkNotifications (reuses existing ChainNotifications pattern)
- v2.0 - **REORDERED: NetworkNotifications moved to Phase 1 (notifications-first approach)**

---

## Table of Contents
1. [Executive Summary](#executive-summary)
2. [Goals & Success Criteria](#goals--success-criteria)
3. [Phased Approach](#phased-approach)
4. [Detailed Phase Breakdown](#detailed-phase-breakdown)
5. [Testing Strategy](#testing-strategy)
6. [Risk Management](#risk-management)
7. [Rollback Plans](#rollback-plans)
8. [Progress Tracking](#progress-tracking)

---

## Executive Summary

### Current State
- **9 components** with circular dependencies and scattered state
- **4,050 LOC** with significant duplication
- **5+ circular dependencies** making testing difficult
- **Per-peer state** scattered across 5 different classes

### Target State
- **4 components** with clear boundaries
- **~2,800 LOC** (30% reduction through elimination of duplication)
- **0 circular dependencies** via event-driven architecture
- **Consolidated per-peer state** in single location

### Approach
**Incremental refactoring over 8-10 weeks** with continuous testing and backwards compatibility.

**NOT a rewrite** - each phase builds on the previous, maintaining functionality throughout.

---

## Goals & Success Criteria

### Primary Goals
1. ✅ Reduce code complexity by 30%
2. ✅ Eliminate all circular dependencies
3. ✅ Consolidate per-peer state into single source of truth
4. ✅ Maintain 100% test coverage throughout refactoring
5. ✅ Zero functional regressions

### Success Criteria

| Metric | Current | Target | Measurement |
|--------|---------|--------|-------------|
| Component count | 9 | 4 | Class count in `include/network/` |
| Lines of code | 4,050 | ~2,800 | `cloc src/network/ include/network/` |
| Circular dependencies | 5+ | 0 | Dependency graph analysis |
| Per-peer state locations | 5 | 1 | Grep for peer_id maps |
| Test pass rate | 100% | 100% | `./bin/coinbasechain_tests` |
| Code duplication | ~25% | <10% | PMD/CPD analysis |

### Non-Goals
- ❌ Changing network protocol behavior
- ❌ Performance optimization (maintain current performance)
- ❌ Adding new features during refactoring
- ❌ Rewriting from scratch

---

## Phased Approach

```
Phase 0: Preparation (Week 1)
  └─> Establish baselines, freeze features, create refactoring branch

Phase 1: Network Notifications (Week 2) ⭐ MOVED UP - Foundation First
  └─> Break circular dependencies with NetworkNotifications (reuses ChainNotifications pattern)
  └─> Singleton pattern = no wiring needed in later phases

Phase 2: Extract Utilities (Week 3)
  └─> ThreadSafeMap, ThreadSafeSet, PeriodicScheduler

Phase 3: Consolidate State (Weeks 4-5) ✅ Uses NetworkNotifications from day 1
  └─> Merge per-peer state, eliminate duplication
  └─> Components subscribe to notifications during migration

Phase 4: Merge Managers (Weeks 6-7) ✅ Uses NetworkNotifications from day 1
  └─> BanMan→PeerManager, HeaderSync+BlockRelay, Address+Anchor
  └─> No callback hell, use notifications directly

Phase 5: Cleanup & Documentation (Weeks 8-9)
  └─> Final cleanup, update docs, merge to main
```

### Critical Path
```
Phase 0 → Phase 1 → Phase 2 → Phase 3 → Phase 4 → Phase 5
   ↓         ↓         ↓         ↓         ↓         ↓
  2 days   5 days   5 days   10 days   10 days   5 days
```

**Total**: 37 days (7.5 weeks) + buffer = **9 weeks**

### Why Notifications First? ⭐
1. **No Wiring** - Singleton pattern, just use `NetworkEvents()` anywhere
2. **Less Rework** - Components use notifications from day 1, not callbacks→notifications migration
3. **Cleaner Code** - Refactor directly to the target architecture
4. **Lower Risk** - Prove notification pattern works before big structural changes
5. **Incremental Adoption** - Can start using notifications immediately without blocking other work

---

## Detailed Phase Breakdown

---

## Phase 0: Preparation (Week 1 - 2 days)

### Objectives
- Establish baseline metrics
- Create isolated refactoring branch
- Set up continuous testing infrastructure
- Document current architecture

### Tasks

#### Task 0.1: Establish Baselines
```bash
# Count components
find include/network/ -name "*.hpp" -exec grep -l "^class" {} \; | wc -l

# Count LOC
cloc src/network/ include/network/

# Measure test coverage
cmake --build build --target coinbasechain_tests
./bin/coinbasechain_tests --reporter=junit > test_results_baseline.xml

# Detect circular dependencies
python3 scripts/detect_circular_deps.py src/network/ include/network/
```

**Deliverable**: `BASELINE_METRICS.md` with current state

#### Task 0.2: Create Refactoring Branch
```bash
git checkout -b refactor/network-architecture
git push -u origin refactor/network-architecture
```

**Deliverable**: Protected branch with CI enabled

#### Task 0.3: Feature Freeze
- [ ] Announce feature freeze for network library
- [ ] Merge all pending network PRs
- [ ] Create backlog for post-refactor features

**Deliverable**: Frozen `main` branch (network library only)

#### Task 0.4: Set Up Continuous Testing
```bash
# Add pre-commit hook
cat > .git/hooks/pre-commit << 'EOF'
#!/bin/bash
cmake --build build --target coinbasechain_tests
./bin/coinbasechain_tests
if [ $? -ne 0 ]; then
    echo "Tests failed, commit aborted"
    exit 1
fi
EOF
chmod +x .git/hooks/pre-commit
```

**Deliverable**: Automated test execution on every commit

### Success Criteria
- ✅ Baseline metrics documented
- ✅ Refactoring branch created
- ✅ All tests passing (baseline)
- ✅ CI pipeline configured

### Estimated Time: 2 days
### Risk Level: LOW

---

## Phase 1: Network Notifications for Decoupling (Week 2 - 5 days) ⭐ FOUNDATION FIRST

### Objectives
- Introduce `NetworkNotifications` (reuses existing `ChainNotifications` pattern)
- Break circular dependencies between managers
- Enable decoupled communication
- Maintain consistency with existing notification system
- **Do this FIRST** so all later phases can use notifications from day 1

### Current Circular Dependencies
```
PeerManager ← → HeaderSyncManager (for disconnect notifications)
PeerManager ← → BlockRelayManager (for disconnect notifications)
PeerManager ← → MessageRouter (for misbehavior reporting)
SyncManager ← → PeerManager (for header validation → disconnect)
```

### Target: Notification-Driven Communication
```
Component A → NetworkNotifications → Component B
(No direct reference - same pattern as ChainNotifications)
```

### Tasks

#### Task 1.1: Implement NetworkNotifications (Mirror ChainNotifications)
```cpp
// include/network/notifications.hpp
class NetworkNotifications {
public:
    class Subscription {
        ~Subscription();  // RAII auto-unsubscribe
    };

    // Callback types
    using PeerDisconnectedCallback = std::function<void(int, const std::string&, const std::string&)>;
    using InvalidHeaderCallback = std::function<void(int, const uint256&, const std::string&)>;
    // ... etc

    // Subscribe methods
    [[nodiscard]] Subscription SubscribePeerDisconnected(PeerDisconnectedCallback callback);
    [[nodiscard]] Subscription SubscribeInvalidHeader(InvalidHeaderCallback callback);
    // ... etc

    // Notify methods
    void NotifyPeerDisconnected(int peer_id, const std::string& addr, const std::string& reason);
    void NotifyInvalidHeader(int peer_id, const uint256& hash, const std::string& reason);
    // ... etc

    static NetworkNotifications& Get();  // Singleton

private:
    std::mutex mutex_;
    std::vector<CallbackEntry> callbacks_;
};

// Global accessor (matches ChainNotifications pattern)
inline NetworkNotifications& NetworkEvents() { return NetworkNotifications::Get(); }
```

**Deliverable**: NetworkNotifications interface + implementation + tests

**Estimated Time**: 1 day (reusing ChainNotifications pattern)
**Risk**: LOW

#### Task 1.2: Add Unit Tests for NetworkNotifications
```cpp
// test/network/notifications_tests.cpp
TEST_CASE("NetworkNotifications: RAII subscription cleanup") {
    auto& notifications = NetworkEvents();
    bool called = false;

    {
        auto sub = notifications.SubscribePeerDisconnected([&](int, auto&, auto&) {
            called = true;
        });

        notifications.NotifyPeerDisconnected(1, "127.0.0.1", "test");
        REQUIRE(called);
    } // subscription goes out of scope

    called = false;
    notifications.NotifyPeerDisconnected(2, "127.0.0.1", "test");
    REQUIRE(!called);  // Callback no longer registered
}

TEST_CASE("NetworkNotifications: Multiple subscribers") {
    auto& notifications = NetworkEvents();
    int count = 0;

    auto sub1 = notifications.SubscribePeerDisconnected([&](int, auto&, auto&) { count++; });
    auto sub2 = notifications.SubscribePeerDisconnected([&](int, auto&, auto&) { count++; });

    notifications.NotifyPeerDisconnected(1, "127.0.0.1", "test");
    REQUIRE(count == 2);  // Both callbacks invoked
}
```

**Deliverable**: Comprehensive test suite

**Estimated Time**: 1 day
**Risk**: LOW

#### Task 1.3: Convert PeerDisconnected to Notifications (Dual-Run)
```cpp
// In PeerManager::remove_peer()
void PeerManager::remove_peer(int peer_id) {
    // ... removal logic ...

    // OLD callback (keep for now)
    if (peer_disconnect_callback_) {
        peer_disconnect_callback_(peer_id);
    }

    // NEW notification (run in parallel)
    NetworkEvents().NotifyPeerDisconnected(peer_id, address, reason);
}

// Components subscribe (in their constructors):
// HeaderSyncManager
peer_disconnected_sub_ = NetworkEvents().SubscribePeerDisconnected(
    [this](int peer_id, const auto&, const auto&) {
        OnPeerDisconnected(peer_id);
    }
);
```

**Deliverable**: Disconnect notifications via NetworkNotifications (dual-run mode)

**Estimated Time**: 1 day
**Risk**: LOW

#### Task 1.4: Convert Misbehavior Reporting to Notifications (Dual-Run)
```cpp
// In SyncManager::HandleHeaders()
if (invalid_header) {
    // OLD approach (keep for now)
    peer_manager_.ReportInvalidHeader(peer_id, reason);

    // NEW notification (run in parallel)
    NetworkEvents().NotifyInvalidHeader(peer_id, hash, reason);
}

// In PeerManager (subscriber in constructor)
invalid_header_sub_ = NetworkEvents().SubscribeInvalidHeader(
    [this](int peer_id, const uint256& hash, const std::string& reason) {
        Misbehaving(peer_id, MisbehaviorPenalty::INVALID_HEADER, reason);
    }
);
```

**Deliverable**: Misbehavior reporting via notifications (dual-run mode)

**Estimated Time**: 1 day
**Risk**: LOW

#### Task 1.5: Verify Notifications Working & Remove Old Callbacks
```bash
# Run full test suite with both systems
./bin/coinbasechain_tests

# Verify notifications are being called
# Add debug logging to confirm both old and new systems working

# After verification, remove old callbacks
# This is safe because notifications are proven to work
```

**Deliverable**: Old callbacks removed, notifications-only

**Estimated Time**: 1 day
**Risk**: LOW

### Success Criteria
- ✅ NetworkNotifications implemented (mirrors ChainNotifications)
- ✅ All inter-component communication via notifications
- ✅ Zero circular dependencies (verified by script)
- ✅ All tests passing
- ✅ Pattern consistency with ChainNotifications

### Estimated Time: 5 days
### Risk Level: LOW-MEDIUM (reusing proven pattern)

### Why This is Lower Risk
- ✅ Reuses proven pattern (ChainNotifications already in production)
- ✅ Team familiar with the pattern
- ✅ No template metaprogramming (simpler code)
- ✅ Named methods (better IDE support, easier debugging)
- ✅ Singleton = no wiring needed in later phases

### Rollback Plan
If NetworkNotifications causes issues:
```bash
# Dual-run approach means old callbacks still work
# Just remove notification calls and keep using callbacks
git revert <task-commit>
```

---

## Phase 2: Extract Utilities (Week 3 - 5 days)

### Objectives
- Extract `ThreadSafeMap<K,V>` template
- Extract `ThreadSafeSet<T>` template
- Extract `PeriodicScheduler` class (optional)
- Eliminate code duplication in locking patterns

### Current State - Repeated Locking Patterns
```cpp
// Location 1: Peer
class Peer {
    std::vector<uint256> blocks_for_inv_relay_;  // Block relay state
};

// Location 2: PeerManager
std::map<int, PeerMisbehaviorData> peer_misbehavior_;

// Location 3: HeaderSyncManager
SyncState sync_state_ {
    uint64_t sync_peer_id;
    int64_t sync_start_time_us;
    // ...
};

// Location 4: BlockRelayManager
std::unordered_map<int, uint256> last_announced_to_peer_;
std::unordered_map<int, int64_t> last_announce_time_s_;

// Location 5: MessageRouter
std::unordered_map<int, LearnedMap> learned_addrs_by_peer_;
std::unordered_set<int> getaddr_replied_;
```

### Target State (1 Location)
```cpp
// New unified state structure
struct PeerState {
    // Core peer (ownership)
    PeerPtr peer;

    // DoS & permissions
    MisbehaviorData misbehavior;
    NetPermissionFlags permissions;
    std::string address;

    // Block relay
    std::vector<uint256> pending_block_announcements;
    uint256 last_announced_block;
    int64_t last_announce_time_s;

    // Address management
    MessageRouter::LearnedMap learned_addresses;
    bool getaddr_replied;

    // Metadata
    std::chrono::steady_clock::time_point created_at;
    ConnectionType connection_type;
};

// Single map in PeerManager
ThreadSafeMap<int, PeerState> peer_states_;
```

### Tasks

#### Task 2.1: Design PeerState Structure
**Steps**:
1. Inventory ALL per-peer data across 5 managers
2. Design `PeerState` struct with all fields
3. Review with team (ensure nothing missed)

**Deliverable**: `include/network/peer_state.hpp`

#### Task 2.2: Add PeerState to PeerManager
```cpp
// PeerManager (new field)
ThreadSafeMap<int, PeerState> peer_states_;  // Replaces: peers_ + peer_misbehavior_

// Migration path:
// Step 1: Add peer_states_ alongside existing maps
// Step 2: Populate both old and new maps (dual-write)
// Step 3: Migrate readers to use peer_states_
// Step 4: Remove old maps
```

**Steps**:
1. Add `peer_states_` field to `PeerManager`
2. Update `add_peer()` to populate `PeerState`
3. Maintain backward compatibility (dual-write to old maps)

**Deliverable**: `PeerManager` with `peer_states_` (dual-write mode)

#### Task 2.3: Migrate Peer::blocks_for_inv_relay_
```cpp
// Before:
class Peer {
    std::vector<uint256> blocks_for_inv_relay_;
    std::mutex block_inv_mutex_;
};

// After:
// Moved to PeerState.pending_block_announcements
// BlockRelayManager accesses via PeerManager::GetState(peer_id).pending_block_announcements
```

**Steps**:
1. Add `pending_block_announcements` to `PeerState`
2. Update `BlockRelayManager::AnnounceTipToPeer()` to use `PeerState`
3. Update `BlockRelayManager::FlushBlockAnnouncements()` to use `PeerState`
4. Remove `Peer::blocks_for_inv_relay_` field

**Deliverable**: Migrated block announcement queue

#### Task 2.4: Migrate BlockRelayManager State
```cpp
// Before:
std::unordered_map<int, uint256> last_announced_to_peer_;
std::unordered_map<int, int64_t> last_announce_time_s_;

// After:
// Use PeerState.last_announced_block
// Use PeerState.last_announce_time_s
```

**Steps**:
1. Update all `BlockRelayManager` methods to use `PeerState`
2. Remove `last_announced_to_peer_` and `last_announce_time_s_` maps
3. Run block relay tests

**Deliverable**: Migrated `BlockRelayManager`

#### Task 2.5: Migrate MessageRouter State
```cpp
// Before:
std::unordered_map<int, LearnedMap> learned_addrs_by_peer_;
std::unordered_set<int> getaddr_replied_;

// After:
// Use PeerState.learned_addresses
// Use PeerState.getaddr_replied
```

**Steps**:
1. Update `handle_addr()` and `handle_getaddr()` to use `PeerState`
2. Remove old maps from `MessageRouter`
3. Run address discovery tests

**Deliverable**: Migrated `MessageRouter`

#### Task 2.6: Migrate HeaderSyncManager State
```cpp
// Before:
SyncState sync_state_ {
    uint64_t sync_peer_id;  // Which peer we're syncing from
    // ...
};

// After:
// Option 1: Keep sync_peer_id separate (it's global, not per-peer)
// Option 2: Add is_sync_peer flag to PeerState

// Decision: Keep separate (sync peer is a global singleton concept)
```

**Steps**:
1. Analyze if sync state should be per-peer or global
2. If per-peer: add to `PeerState`
3. If global: leave in `HeaderSyncManager` (current design is correct)

**Deliverable**: Analysis + decision document

#### Task 2.7: Cleanup - Remove Dual-Write Mode
```cpp
// PeerManager cleanup
// Remove old maps:
std::map<int, PeerPtr> peers_;  // ❌ DELETE
std::map<int, PeerMisbehaviorData> peer_misbehavior_;  // ❌ DELETE

// Keep only:
ThreadSafeMap<int, PeerState> peer_states_;  // ✅ KEEP
```

**Steps**:
1. Verify all readers migrated to `peer_states_`
2. Remove old `peers_` and `peer_misbehavior_` fields
3. Run full test suite

**Deliverable**: Cleaned-up `PeerManager` with single state map

#### Task 2.8: Update Cleanup Logic
```cpp
// Before: Cleanup in 5 places
PeerManager::remove_peer(id)
MessageRouter::OnPeerDisconnected(id)
BlockRelayManager::OnPeerDisconnected(id)
HeaderSyncManager::OnPeerDisconnected(id)
// ... etc

// After: Cleanup in 1 place
PeerManager::remove_peer(id) {
    peer_states_.Erase(id);  // Everything deleted at once

    // Still notify other managers for global state cleanup:
    if (peer_disconnect_callback_) {
        peer_disconnect_callback_(id);
    }
}
```

**Steps**:
1. Simplify `PeerManager::remove_peer()` to single erase
2. Update other managers to only clean up global state (not per-peer)
3. Run disconnect tests

**Deliverable**: Simplified cleanup logic

### Success Criteria
- ✅ All per-peer data consolidated into `PeerState`
- ✅ Single source of truth for peer data
- ✅ No duplicate peer_id maps in any manager
- ✅ All tests passing
- ✅ Simplified cleanup on disconnect

### Estimated Time: 10 days
### Risk Level: MEDIUM (touches many components)

### Testing Strategy
- Run full test suite after each migration step
- Add new tests for `PeerState` accessors
- Verify no memory leaks (Valgrind)
- Performance benchmark (ensure no regression)

### Rollback Plan
Each task is a separate commit. If migration fails:
```bash
git revert <task-commit>
# Revert individual tasks while keeping earlier progress
```

---

## Phase 3: Consolidate Per-Peer State (Weeks 4-5 - 10 days) ✅ Uses NetworkNotifications

### Objectives
- Merge `BanMan` into `PeerManager`
- Merge `HeaderSyncManager` + `BlockRelayManager` → `SyncManager`
- Merge `AddressManager` + `AnchorManager` → `DiscoveryManager`
- Reduce component count from 9 to 6

### Tasks

#### Task 3.1: Merge BanMan into PeerManager

**Rationale**: Banning is a peer lifecycle concern, belongs in `PeerManager`.

**Steps**:
1. Move `BanMan` methods into `PeerManager`:
   ```cpp
   // PeerManager gains:
   void Ban(const std::string& ip, int64_t duration);
   bool IsBanned(const std::string& ip) const;
   void Unban(const std::string& ip);
   ```

2. Move banned IP map into `PeerManager`:
   ```cpp
   ThreadSafeMap<std::string, int64_t> banned_ips_;
   ```

3. Update `Misbehaving()` to directly call internal `Ban()`:
   ```cpp
   bool PeerManager::Misbehaving(int peer_id, int penalty, const std::string& reason) {
       // ... existing logic ...
       if (score >= THRESHOLD) {
           Ban(data.address, 86400);  // Direct call, no BanMan indirection
       }
   }
   ```

4. Update `NetworkManager` to use `PeerManager::Ban()` instead of `BanMan::Ban()`

5. Delete `include/network/banman.hpp` and `src/network/banman.cpp`

**Deliverable**: `BanMan` merged into `PeerManager`

**Estimated Time**: 2 days
**Risk**: LOW

#### Task 3.2: Merge HeaderSyncManager + BlockRelayManager → SyncManager

**Rationale**: Header sync and block relay are tightly coupled parts of the sync process.

**New Structure**:
```cpp
// include/network/sync_manager.hpp
class SyncManager {
public:
    // Header sync (from HeaderSyncManager)
    void RequestHeaders(PeerPtr peer);
    bool HandleHeaders(PeerPtr peer, HeadersMessage* msg);
    bool HandleGetHeaders(PeerPtr peer, GetHeadersMessage* msg);
    void CheckInitialSync();
    bool IsSynced() const;
    CBlockLocator GetLocatorFromPrev() const;

    // Block relay (from BlockRelayManager)
    void AnnounceBlock(const uint256& hash);
    void AnnounceTipToAllPeers();
    void FlushBlockAnnouncements();
    bool HandleInv(PeerPtr peer, InvMessage* msg);

    // Sync state
    std::optional<int> GetSyncPeerId() const;
    void SetSyncPeer(int peer_id);
    void ClearSyncPeer();

    // Lifecycle
    void OnPeerDisconnected(int peer_id);

private:
    // Header sync state
    struct SyncState {
        std::optional<int> sync_peer_id;
        int64_t sync_start_time_us;
        int64_t last_headers_received_us;
    };
    ThreadSafeState<SyncState> sync_state_;

    // Dependencies
    ChainstateManager& chainstate_;
    PeerManager& peer_manager_;
};
```

**Steps**:
1. Create `include/network/sync_manager.hpp`
2. Copy methods from `HeaderSyncManager` + `BlockRelayManager`
3. Merge constructors (take references to both dependencies)
4. Update `NetworkManager` to use `SyncManager` instead of two managers
5. Delete old `HeaderSyncManager` and `BlockRelayManager`

**Migration**:
```cpp
// Before:
header_sync_manager_->HandleHeaders(peer, msg);
block_relay_manager_->AnnounceTipToAllPeers();

// After:
sync_manager_->HandleHeaders(peer, msg);
sync_manager_->AnnounceTipToAllPeers();
```

**Deliverable**: `SyncManager` replaces 2 managers

**Estimated Time**: 4 days
**Risk**: MEDIUM (merging significant logic)

#### Task 3.3: Merge AddressManager + AnchorManager → DiscoveryManager

**Rationale**: Anchors are a special subset of addresses, belong in same manager.

**New Structure**:
```cpp
// include/network/discovery_manager.hpp
class DiscoveryManager {
public:
    // Address book (from AddressManager)
    bool Add(const protocol::NetworkAddress& addr, uint32_t timestamp);
    std::optional<protocol::NetworkAddress> Select();
    std::optional<protocol::NetworkAddress> SelectNewForFeeler();
    void Attempt(const protocol::NetworkAddress& addr);
    void Good(const protocol::NetworkAddress& addr);
    void Failed(const protocol::NetworkAddress& addr);
    size_t Size() const;

    // Anchors (from AnchorManager)
    std::vector<protocol::NetworkAddress> GetAnchors() const;
    bool SaveAnchors(const std::string& filepath);
    bool LoadAnchors(const std::string& filepath);

    // ADDR message handling
    bool HandleAddr(PeerPtr peer, AddrMessage* msg);
    bool HandleGetAddr(PeerPtr peer);

private:
    // Tried/new tables
    ThreadSafeMap<std::string, AddrInfo> tried_;
    ThreadSafeMap<std::string, AddrInfo> new_;

    // Anchors
    std::vector<protocol::NetworkAddress> anchors_;
    mutable std::mutex anchors_mutex_;
};
```

**Steps**:
1. Create `include/network/discovery_manager.hpp`
2. Copy all methods from `AddressManager` + `AnchorManager`
3. Update `NetworkManager` to use `DiscoveryManager`
4. Delete old managers

**Deliverable**: `DiscoveryManager` replaces 2 managers

**Estimated Time**: 3 days
**Risk**: LOW (relatively independent managers)

#### Task 3.4: Update NetworkManager Dependencies
```cpp
// Before: 8 dependencies
std::unique_ptr<PeerManager> peer_manager_;
std::unique_ptr<AddressManager> addr_manager_;
std::unique_ptr<HeaderSyncManager> header_sync_manager_;
std::unique_ptr<BlockRelayManager> block_relay_manager_;
std::unique_ptr<MessageRouter> message_router_;
std::unique_ptr<BanMan> ban_man_;
std::unique_ptr<NATManager> nat_manager_;
std::unique_ptr<AnchorManager> anchor_manager_;

// After: 5 dependencies (3 merged)
std::unique_ptr<PeerManager> peer_manager_;  // Now includes BanMan
std::unique_ptr<DiscoveryManager> discovery_manager_;  // AddressManager + AnchorManager
std::unique_ptr<SyncManager> sync_manager_;  // HeaderSyncManager + BlockRelayManager
std::unique_ptr<MessageRouter> message_router_;
std::unique_ptr<NATManager> nat_manager_;
```

**Steps**:
1. Update `NetworkManager` constructor
2. Update initialization order
3. Update all call sites
4. Run full test suite

**Deliverable**: Updated `NetworkManager` with 5 dependencies

**Estimated Time**: 1 day
**Risk**: LOW

### Success Criteria
- ✅ Component count reduced from 9 to 6
- ✅ Related functionality grouped together
- ✅ All tests passing
- ✅ No functional changes

### Estimated Time: 10 days
### Risk Level: MEDIUM

---

## Phase 4: Merge Related Managers (Weeks 6-7 - 10 days) ✅ Uses NetworkNotifications

### Objectives
- Introduce `NetworkNotifications` (reuses existing `ChainNotifications` pattern)
- Break circular dependencies between managers
- Enable decoupled communication
- Maintain consistency with existing notification system

### Current Circular Dependencies
```
PeerManager ← → HeaderSyncManager (for disconnect notifications)
PeerManager ← → BlockRelayManager (for disconnect notifications)
PeerManager ← → MessageRouter (for misbehavior reporting)
SyncManager ← → PeerManager (for header validation → disconnect)
```

### Target: Notification-Driven Communication
```
Component A → NetworkNotifications → Component B
(No direct reference - same pattern as ChainNotifications)
```

### Tasks

#### Task 4.1: Implement NetworkNotifications (Mirror ChainNotifications)
```cpp
// include/network/notifications.hpp
class NetworkNotifications {
public:
    class Subscription {
        ~Subscription();  // RAII auto-unsubscribe
    };

    // Callback types
    using PeerDisconnectedCallback = std::function<void(int, const std::string&, const std::string&)>;
    using InvalidHeaderCallback = std::function<void(int, const uint256&, const std::string&)>;
    // ... etc

    // Subscribe methods
    [[nodiscard]] Subscription SubscribePeerDisconnected(PeerDisconnectedCallback callback);
    [[nodiscard]] Subscription SubscribeInvalidHeader(InvalidHeaderCallback callback);
    // ... etc

    // Notify methods
    void NotifyPeerDisconnected(int peer_id, const std::string& addr, const std::string& reason);
    void NotifyInvalidHeader(int peer_id, const uint256& hash, const std::string& reason);
    // ... etc

    static NetworkNotifications& Get();  // Singleton

private:
    std::mutex mutex_;
    std::vector<CallbackEntry> callbacks_;
};

// Global accessor (matches ChainNotifications pattern)
inline NetworkNotifications& NetworkEvents() { return NetworkNotifications::Get(); }
```

**Deliverable**: NetworkNotifications interface + implementation + tests

**Estimated Time**: 1 day (reusing ChainNotifications pattern)
**Risk**: LOW

#### Task 4.2: Add Unit Tests for NetworkNotifications
```cpp
// test/network/notifications_tests.cpp
TEST_CASE("NetworkNotifications: RAII subscription cleanup") {
    auto& notifications = NetworkEvents();
    bool called = false;

    {
        auto sub = notifications.SubscribePeerDisconnected([&](int, auto&, auto&) {
            called = true;
        });

        notifications.NotifyPeerDisconnected(1, "127.0.0.1", "test");
        REQUIRE(called);
    } // subscription goes out of scope

    called = false;
    notifications.NotifyPeerDisconnected(2, "127.0.0.1", "test");
    REQUIRE(!called);  // Callback no longer registered
}

TEST_CASE("NetworkNotifications: Multiple subscribers") {
    auto& notifications = NetworkEvents();
    int count = 0;

    auto sub1 = notifications.SubscribePeerDisconnected([&](int, auto&, auto&) { count++; });
    auto sub2 = notifications.SubscribePeerDisconnected([&](int, auto&, auto&) { count++; });

    notifications.NotifyPeerDisconnected(1, "127.0.0.1", "test");
    REQUIRE(count == 2);  // Both callbacks invoked
}
```

**Deliverable**: Comprehensive test suite

**Estimated Time**: 1 day
**Risk**: LOW

#### Task 4.3: Update Components to Use NetworkNotifications

**No changes to NetworkManager needed** - NetworkNotifications is a singleton.

```cpp
// Components just use the global NetworkEvents() accessor
// (same as how they use ChainNotifications)

// Example: PeerManager publishes events
void PeerManager::remove_peer(int peer_id) {
    // ... removal logic ...
    NetworkEvents().NotifyPeerDisconnected(peer_id, address, reason);
}

// Example: HeaderSyncManager subscribes
HeaderSyncManager::HeaderSyncManager(...) {
    peer_disconnected_sub_ = NetworkEvents().SubscribePeerDisconnected(
        [this](int peer_id, auto&, auto&) { OnPeerDisconnected(peer_id); }
    );
}
```

**Deliverable**: Components using NetworkNotifications

**Estimated Time**: 0.5 days (no wiring needed, just use singleton)
**Risk**: LOW

#### Task 4.4: Convert PeerDisconnected to Notifications
```cpp
// Before: Direct callback
peer_manager_->SetPeerDisconnectCallback([this](int peer_id) {
    header_sync_manager_->OnPeerDisconnected(peer_id);
    block_relay_manager_->OnPeerDisconnected(peer_id);
    message_router_->OnPeerDisconnected(peer_id);
});

// After: Notification publication
// In PeerManager::remove_peer()
void PeerManager::remove_peer(int peer_id) {
    // ... removal logic ...

    // Publish notification
    NetworkEvents().NotifyPeerDisconnected(peer_id, address, reason);
}

// Subscribers (in each manager's constructor):
// HeaderSyncManager
peer_disconnected_sub_ = NetworkEvents().SubscribePeerDisconnected(
    [this](int peer_id, const auto&, const auto&) {
        OnPeerDisconnected(peer_id);
    }
);

// BlockRelayManager
peer_disconnected_sub_ = NetworkEvents().SubscribePeerDisconnected(
    [this](int peer_id, const auto&, const auto&) {
        OnPeerDisconnected(peer_id);
    }
);

// MessageRouter
peer_disconnected_sub_ = NetworkEvents().SubscribePeerDisconnected(
    [this](int peer_id, const auto&, const auto&) {
        OnPeerDisconnected(peer_id);
    }
);
```

**Deliverable**: Disconnect notifications via NetworkNotifications

**Estimated Time**: 1 day
**Risk**: LOW

#### Task 4.5: Convert Misbehavior Reporting to Notifications
```cpp
// Before: Direct method call
peer_manager_.ReportInvalidHeader(peer_id, reason);
// → PeerManager::ReportInvalidHeader() → Misbehaving()

// After: Notification publication
// In SyncManager::HandleHeaders()
if (invalid_header) {
    NetworkEvents().NotifyInvalidHeader(peer_id, hash, reason);
}

// In PeerManager (subscriber in constructor)
invalid_header_sub_ = NetworkEvents().SubscribeInvalidHeader(
    [this](int peer_id, const uint256& hash, const std::string& reason) {
        Misbehaving(peer_id, MisbehaviorPenalty::INVALID_HEADER, reason);
    }
);

low_work_sub_ = NetworkEvents().SubscribeLowWorkHeaders(
    [this](int peer_id, size_t count, const std::string& reason) {
        Misbehaving(peer_id, MisbehaviorPenalty::LOW_WORK_HEADERS, reason);
    }
);
```

**Deliverable**: Misbehavior reporting via notifications

**Estimated Time**: 1 day
**Risk**: LOW

#### Task 4.6: Verify Circular Dependencies Eliminated
```bash
# Run dependency analysis
python3 scripts/detect_circular_deps.py src/network/ include/network/

# Should show: 0 circular dependencies
```

**Deliverable**: Dependency graph with no cycles

**Estimated Time**: 1 day
**Risk**: LOW

#### Task 4.7: Remove Old Callback Mechanisms
```cpp
// Delete from PeerManager:
std::function<void(int)> peer_disconnect_callback_;
void SetPeerDisconnectCallback(std::function<void(int)> callback);

// Delete from HeaderSyncManager/BlockRelayManager/etc:
PeerManager& peer_manager_;  // No longer need direct reference
void ReportInvalidHeader(int peer_id, const std::string& reason);
void ReportMisbehavior(...);

// All replaced by NetworkNotifications
```

**Deliverable**: Cleaned-up interfaces, circular dependencies eliminated

**Estimated Time**: 0.5 days
**Risk**: LOW

### Success Criteria
- ✅ NetworkNotifications implemented (mirrors ChainNotifications)
- ✅ All inter-component communication via notifications
- ✅ Zero circular dependencies (verified by script)
- ✅ All tests passing
- ✅ Pattern consistency with ChainNotifications

### Estimated Time: 5 days (down from 10 days for EventBus)
### Risk Level: LOW-MEDIUM (reusing proven pattern)

### Rollback Plan
If NetworkNotifications causes issues:
```bash
# Dual-run approach: Keep old callbacks alongside notifications
# Phase 4.4-4.5: Run both systems in parallel
# Phase 4.6-4.7: Remove old callbacks only after verification

# Easy rollback: Each task is a separate commit
git revert <task-commit>
```

### Why This is Lower Risk Than EventBus
- ✅ Reuses proven pattern (ChainNotifications already in production)
- ✅ Team familiar with the pattern
- ✅ No template metaprogramming (simpler code)
- ✅ Named methods (better IDE support, easier debugging)
- ✅ Half the implementation time (5 days vs 10 days)

---

## Phase 5: Cleanup & Documentation (Weeks 8-9 - 5 days)

### Objectives
- Final code cleanup
- Update all documentation
- Performance benchmarking
- Merge to main

### Tasks

#### Task 5.1: Code Cleanup
- [ ] Remove commented-out code
- [ ] Fix TODO comments
- [ ] Consistent naming conventions
- [ ] Clang-format entire codebase

**Deliverable**: Clean, formatted code

**Estimated Time**: 1 day

#### Task 5.2: Update Documentation
- [ ] Update `NETWORK_LIBRARY_ARCHITECTURE.md` with new design
- [ ] Update component diagrams
- [ ] Add event bus documentation
- [ ] Update API reference

**Deliverable**: Updated documentation

**Estimated Time**: 2 days

#### Task 5.3: Performance Benchmarking
```cpp
// Benchmark suite
- Connection throughput (connections/sec)
- Message processing latency (μs)
- Memory footprint (MB per peer)
- CPU usage (% under load)

// Compare before/after refactoring
// Ensure no regressions
```

**Deliverable**: Performance report

**Estimated Time**: 1 day

#### Task 5.4: Final Test Suite Run
```bash
# Full test suite
./bin/coinbasechain_tests --reporter=junit

# Stress tests
./bin/coinbasechain_tests --tag=stress

# Memory leak detection
valgrind --leak-check=full ./bin/coinbasechain_tests

# Thread sanitizer
./bin/coinbasechain_tests_tsan
```

**Deliverable**: 100% tests passing, no leaks, no races

**Estimated Time**: 1 day

#### Task 5.5: Merge to Main
```bash
# Final review
git diff main...refactor/network-architecture

# Squash commits (optional)
git rebase -i main

# Create PR
gh pr create --title "Refactor: Network Library Architecture" \
             --body "See NETWORK_REFACTORING_ACTION_PLAN.md"

# After approval, merge
git checkout main
git merge --no-ff refactor/network-architecture
git push origin main
```

**Deliverable**: Refactoring merged to main

**Estimated Time**: 0.5 days (+ review time)

### Success Criteria
- ✅ All documentation updated
- ✅ Performance within 5% of baseline
- ✅ 100% test coverage maintained
- ✅ Code merged to main

### Estimated Time: 5 days (+ review time)
### Risk Level: LOW

---

## Summary: Timeline & Reordering Benefits

### v1.0: Original Estimate (with EventBus at end)
```
Phase 0: Preparation - 2 days
Phase 1: Extract Utilities - 5 days
Phase 2: Consolidate State - 10 days (fighting callback hell)
Phase 3: Merge Managers - 10 days (fighting callback hell)
Phase 4: EventBus - 10 days (then migrate everything again)
Phase 5: Cleanup - 5 days
────────────────
Total: 42 days (8.5 weeks) + buffer = 10 weeks
```

### v1.1: NetworkNotifications (same order, just swapped EventBus for Notifications)
```
Phase 0: Preparation - 2 days
Phase 1: Extract Utilities - 5 days
Phase 2: Consolidate State - 10 days (fighting callback hell)
Phase 3: Merge Managers - 10 days (fighting callback hell)
Phase 4: NetworkNotifications - 5 days (then migrate everything)
Phase 5: Cleanup - 5 days
────────────────
Total: 37 days (7.5 weeks) + buffer = 9 weeks
Savings: 5 days by reusing ChainNotifications pattern
```

### v2.0: Notifications-First Approach (CURRENT) ⭐
```
Phase 0: Preparation - 2 days
Phase 1: NetworkNotifications - 5 days ⭐ DO THIS FIRST
Phase 2: Extract Utilities - 5 days (uses notifications)
Phase 3: Consolidate State - 10 days (uses notifications from day 1)
Phase 4: Merge Managers - 10 days (uses notifications from day 1)
Phase 5: Cleanup - 5 days
────────────────
Total: 37 days (7.5 weeks) + buffer = 9 weeks
SAME timeline, but CLEANER implementation
```

### Why v2.0 is Superior

**Same timeline, but:**
1. ✅ **No double migration** - Components use notifications from day 1 in Phases 3-4
2. ✅ **No wiring** - Singleton pattern, just use `NetworkEvents()` anywhere
3. ✅ **Cleaner refactoring** - Refactor directly to target architecture
4. ✅ **Lower risk** - Prove notifications work before big structural changes
5. ✅ **Better code** - No temporary callback→notification migration code

**Key Insight**: NetworkNotifications is **foundational infrastructure**, not a feature.
Do it first, then everything else is cleaner.

---

## Testing Strategy

### Unit Testing
```bash
# Run after every phase
cmake --build build --target coinbasechain_tests
./bin/coinbasechain_tests

# Minimum pass rate: 100%
```

### Integration Testing
```bash
# Network integration tests
./bin/coinbasechain_tests --tag=network

# Peer discovery tests
./bin/coinbasechain_tests --tag=discovery

# Sync tests
./bin/coinbasechain_tests --tag=sync
```

### Regression Testing
```bash
# Baseline before refactoring
./bin/coinbasechain_tests > baseline_results.txt

# After each phase
./bin/coinbasechain_tests > phase_N_results.txt
diff baseline_results.txt phase_N_results.txt

# No differences = no regressions
```

### Performance Testing
```bash
# Benchmark script
scripts/benchmark_network.sh

# Metrics to track:
- Connection time (avg, p95, p99)
- Message processing latency
- Memory per peer
- CPU under load
```

### Memory Safety
```bash
# Valgrind
valgrind --leak-check=full --show-leak-kinds=all ./bin/coinbasechain_tests

# AddressSanitizer
cmake -DCMAKE_CXX_FLAGS="-fsanitize=address" ..
./bin/coinbasechain_tests

# ThreadSanitizer
cmake -DCMAKE_CXX_FLAGS="-fsanitize=thread" ..
./bin/coinbasechain_tests
```

---

## Risk Management

### High-Risk Areas

#### Risk 1: Phase 2 (State Consolidation)
**Risk**: Breaking existing code that accesses per-peer state

**Mitigation**:
- Use dual-write pattern during migration
- Migrate readers gradually
- Extensive testing at each step

**Contingency**: Revert individual tasks, keep earlier progress

#### Risk 2: Phase 4 (Network Notifications)
**Risk**: NetworkNotifications introduces bugs or performance issues

**Mitigation**:
- Reuses proven ChainNotifications pattern (already in production)
- Keep old callback mechanisms during migration (dual-run)
- Extensive integration testing
- Performance benchmarking

**Contingency**: Run both notifications + callbacks in parallel, revert if issues

**Note**: Lower risk than generic EventBus because pattern is already proven

#### Risk 3: Thread Safety
**Risk**: Introducing new race conditions during refactoring

**Mitigation**:
- Run ThreadSanitizer after every commit
- Code review focusing on concurrency
- Stress testing under load

**Contingency**: Fix issues immediately or revert commits

### Medium-Risk Areas

#### Risk 4: Test Coverage Gaps
**Risk**: Refactoring exposes untested code paths

**Mitigation**:
- Add tests for new utilities before migration
- Increase coverage during refactoring
- Target: 100% coverage for refactored code

**Contingency**: Add missing tests retroactively

### Low-Risk Areas

#### Risk 5: Performance Regression
**Risk**: Refactored code is slower

**Mitigation**:
- Benchmark before/after each phase
- Profile hotspots
- Optimize if needed

**Contingency**: Revert performance-critical changes, optimize differently

---

## Rollback Plans

### Per-Phase Rollback
Each phase is a separate branch from `refactor/network-architecture`:

```bash
# If Phase N fails:
git checkout refactor/network-architecture
git revert <phase-N-commits>

# Continue with next phase
```

### Per-Task Rollback
Each task is a separate commit:

```bash
# If Task N.M fails:
git revert <task-commit>

# Continue with next task
```

### Full Rollback
If entire refactoring needs to be abandoned:

```bash
# Delete refactoring branch
git branch -D refactor/network-architecture

# Start over or abandon
```

### Partial Merge
If some phases work but others fail:

```bash
# Merge only successful phases
git checkout main
git cherry-pick <phase-1-commits>
git cherry-pick <phase-2-commits>
# Skip failed phases
```

---

## Progress Tracking

### Kanban Board (GitHub Projects)

```
TODO | IN PROGRESS | TESTING | DONE
-----|-------------|---------|------
     |             |         | Phase 0: Preparation
     | Phase 1: Extract Utilities | |
Phase 2: Consolidate State | | |
Phase 3: Merge Managers | | |
Phase 4: Event Bus | | |
Phase 5: Cleanup | | |
```

### Daily Standup Questions
1. What did you complete yesterday?
2. What will you work on today?
3. Any blockers or risks?

### Weekly Review
- Review completed tasks
- Update timeline if needed
- Adjust priorities
- Report to stakeholders

### Success Metrics Dashboard
```
Component Count:    9 → 6 → 4
LOC:                4050 → 3500 → 2800
Circular Deps:      5 → 3 → 0
Test Pass Rate:     100% → 100% → 100%
Code Coverage:      85% → 90% → 95%
```

---

## Communication Plan

### Stakeholders
- Development team (daily updates)
- Project manager (weekly reports)
- Users (no impact, internal refactoring)

### Update Frequency
- **Daily**: Team standup
- **Weekly**: Progress report to PM
- **Bi-weekly**: Demo to stakeholders (if requested)

### Escalation Path
If blockers arise:
1. Team discussion (same day)
2. Tech lead decision (within 1 day)
3. Project manager (if timeline affected)

---

## Post-Refactoring Benefits

### Immediate Benefits
- ✅ 30% less code to maintain
- ✅ Clearer component boundaries
- ✅ Easier to understand and onboard
- ✅ Zero circular dependencies

### Long-Term Benefits
- ✅ Easier to add new features (clear extension points)
- ✅ Better testability (decoupled components)
- ✅ Reduced bug surface (less duplication)
- ✅ Foundation for future optimizations

### Metrics Before/After

| Metric | Before | After | Improvement |
|--------|--------|-------|-------------|
| Components | 9 | 4 | -56% |
| LOC | 4,050 | 2,800 | -30% |
| Circular deps | 5+ | 0 | -100% |
| Per-peer state locations | 5 | 1 | -80% |
| Avg component LOC | 450 | 700 | +55% (better cohesion) |
| Code duplication | ~25% | <10% | -60% |

---

## Appendix

### A. Dependency Analysis Script
```python
# scripts/detect_circular_deps.py
import os
import re
import sys
from collections import defaultdict

def find_includes(file_path):
    """Extract #include statements from file."""
    includes = []
    with open(file_path, 'r') as f:
        for line in f:
            match = re.match(r'#include [<"]network/(\w+)\.hpp[>"]', line)
            if match:
                includes.append(match.group(1))
    return includes

def build_dependency_graph(src_dir):
    """Build dependency graph for all headers."""
    graph = defaultdict(list)

    for root, dirs, files in os.walk(src_dir):
        for file in files:
            if file.endswith('.hpp') or file.endswith('.cpp'):
                file_path = os.path.join(root, file)
                component = file.replace('.hpp', '').replace('.cpp', '')
                includes = find_includes(file_path)
                graph[component].extend(includes)

    return graph

def detect_cycles(graph):
    """Detect circular dependencies using DFS."""
    visited = set()
    rec_stack = set()
    cycles = []

    def dfs(node, path):
        visited.add(node)
        rec_stack.add(node)
        path.append(node)

        for neighbor in graph.get(node, []):
            if neighbor not in visited:
                if dfs(neighbor, path.copy()):
                    return True
            elif neighbor in rec_stack:
                cycle_start = path.index(neighbor)
                cycles.append(path[cycle_start:] + [neighbor])
                return True

        rec_stack.remove(node)
        return False

    for node in graph:
        if node not in visited:
            dfs(node, [])

    return cycles

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python detect_circular_deps.py <src_dir>")
        sys.exit(1)

    src_dir = sys.argv[1]
    graph = build_dependency_graph(src_dir)
    cycles = detect_cycles(graph)

    if cycles:
        print(f"Found {len(cycles)} circular dependencies:")
        for i, cycle in enumerate(cycles, 1):
            print(f"{i}. {' → '.join(cycle)}")
        sys.exit(1)
    else:
        print("No circular dependencies found!")
        sys.exit(0)
```

### B. Performance Benchmark Script
```bash
#!/bin/bash
# scripts/benchmark_network.sh

set -e

echo "=== Network Library Performance Benchmark ==="
echo "Building tests..."
cmake --build build --target coinbasechain_tests

echo ""
echo "=== Connection Throughput ==="
./bin/coinbasechain_tests --tag=benchmark_connection_throughput

echo ""
echo "=== Message Processing Latency ==="
./bin/coinbasechain_tests --tag=benchmark_message_latency

echo ""
echo "=== Memory Footprint ==="
./bin/coinbasechain_tests --tag=benchmark_memory_footprint

echo ""
echo "=== CPU Under Load ==="
./bin/coinbasechain_tests --tag=benchmark_cpu_usage

echo ""
echo "Benchmark complete!"
```

### C. Code Metrics Script
```bash
#!/bin/bash
# scripts/measure_metrics.sh

echo "=== Code Metrics ==="

echo "Component count:"
find include/network/ -name "*.hpp" -exec grep -l "^class" {} \; | wc -l

echo ""
echo "Lines of code:"
cloc src/network/ include/network/

echo ""
echo "Circular dependencies:"
python3 scripts/detect_circular_deps.py src/network/ include/network/

echo ""
echo "Code duplication:"
pmd cpd --minimum-tokens 50 --files src/network/,include/network/ --language cpp

echo ""
echo "Test coverage:"
# Assumes gcov/lcov setup
lcov --capture --directory build/src/network/ --output-file coverage.info
genhtml coverage.info --output-directory coverage_report
echo "Coverage report: coverage_report/index.html"
```

---

## Approval & Sign-Off

### Required Approvals
- [ ] Tech Lead: _____________________ Date: _______
- [ ] Architect: _____________________ Date: _______
- [ ] Project Manager: _______________ Date: _______

### Change Log

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2025-11-02 | Review Team | Initial action plan with EventBus |
| 1.1 | 2025-11-02 | Review Team | Replaced EventBus with NetworkNotifications (reuses ChainNotifications) |
| 2.0 | 2025-11-02 | Review Team | **REORDERED: NetworkNotifications moved to Phase 1 (notifications-first)** |

---

**END OF ACTION PLAN**
