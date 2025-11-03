# Network 3-Manager Architecture Refactoring Plan

**Status**: Planning
**Started**: 2025-11-03
**Goal**: Implement clean 3-manager architecture as designed

## Target Architecture

```
NetworkManager (NetworkService)
├── ConnectionManager (currently PeerManager)
│   ├── Peer lifecycle, DoS, banning, eviction
│   └── Per-peer state management
├── SyncManager (NEW - consolidates sync operations)
│   ├── HeaderSyncManager (owned)
│   ├── BlockRelayManager (owned)
│   └── IBD coordination logic
└── DiscoveryManager (UPGRADE - consolidates discovery)
    ├── AddressManager (owned)
    ├── AnchorManager (owned)
    └── ADDR/GETADDR protocol handlers
```

## Current State Analysis

### What We Have Now (INCORRECT)
```
NetworkManager
├── PeerManager ✓ (ConnectionManager equivalent - correct)
├── AddressManager ✗ (should be IN DiscoveryManager)
├── AnchorManager ✗ (should be IN DiscoveryManager)
├── HeaderSyncManager ✗ (should be IN SyncManager)
├── BlockRelayManager ✗ (should be IN SyncManager)
├── MessageDispatcher ✓ (correct - routing infrastructure)
├── NATManager ✓ (correct - utility component)
└── DiscoveryManager ✗ (currently just a thin protocol handler, needs to OWN components)
```

### Problems with Current State
1. **Flat structure**: All managers are siblings under NetworkManager
2. **DiscoveryManager is hollow**: Just a protocol handler, doesn't own AddressManager/AnchorManager
3. **No SyncManager**: HeaderSync and BlockRelay are separate, no coordination
4. **Ownership confusion**: NetworkManager owns too many low-level components
5. **Violates design**: Doesn't match the approved 3-manager architecture

## Refactoring Phases

### Phase 1: Create SyncManager ✅ COMPLETED
**Goal**: Consolidate HeaderSyncManager and BlockRelayManager under SyncManager

#### Tasks
- [x] 1.1: Create `include/network/sync_manager.hpp`
  - Constructor: `SyncManager(HeaderSyncManager*, BlockRelayManager*)` (Phase 1: facade pattern)
  - Members: Raw pointers (Phase 1), will become `unique_ptr` in later phases
  - Public API: `HandleHeaders()`, `HandleGetHeaders()`, `HandleInv()`
  - Forward declarations and includes

- [x] 1.2: Create `src/network/sync_manager.cpp`
  - Constructor implementation (facade pattern for Phase 1)
  - Implement delegation methods to referenced managers
  - Added null-checks for safety

- [x] 1.3: Update `include/network/network_manager.hpp`
  - Add forward declaration for SyncManager
  - Keep `header_sync_manager_` and `block_relay_manager_` (Phase 1: still owned by NetworkManager)
  - Add `sync_manager_` member and test accessor

- [x] 1.4: Update `src/network/network_manager.cpp`
  - Create SyncManager with references to existing managers (facade pattern)
  - Update message handler registrations (HEADERS, GETHEADERS, INV) to route through SyncManager
  - Keep existing manager initialization (Phase 1: no ownership transfer yet)

- [x] 1.5: Add to CMakeLists.txt
  - Added `src/network/sync_manager.cpp` to build

- [x] 1.6: Update tests
  - No test updates needed (Phase 1: tests still access managers directly)
  - SyncManager is transparent facade over existing managers

- [x] 1.7: Build and test
  - Build successful
  - All 569 tests pass with 16,368 assertions

**Success Criteria**: ✅ All tests pass, SyncManager provides facade for sync operations
**Note**: Phase 1 uses facade pattern (references) not ownership yet. Ownership transfer happens in later phases when all dependencies are updated.

---

### Phase 2: Upgrade DiscoveryManager ⬜ NOT STARTED
**Goal**: Make DiscoveryManager own AddressManager and AnchorManager

#### Tasks
- [ ] 2.1: Update `include/network/discovery_manager.hpp`
  - Change constructor signature: remove pointers, add config params
  - Add members: `unique_ptr<AddressManager>`, `unique_ptr<AnchorManager>`
  - Add forwarding methods for AddressManager operations:
    - `SelectAddress()`, `Good()`, `Failed()`, `Attempt()`, `Add()`, etc.
  - Add forwarding methods for AnchorManager operations:
    - `GetAnchors()`, `SaveAnchors()`, etc.
  - Keep existing HandleAddr/HandleGetAddr methods

- [ ] 2.2: Update `src/network/discovery_manager.cpp`
  - Update constructor to create AddressManager and AnchorManager
  - Implement forwarding methods
  - Update HandleAddr/HandleGetAddr to use owned addr_manager_

- [ ] 2.3: Update `include/network/network_manager.hpp`
  - Remove `addr_manager_` and `anchor_manager_` members
  - Keep only `discovery_manager_`
  - Remove AddressManager/AnchorManager forward declarations (if not needed)

- [ ] 2.4: Update `src/network/network_manager.cpp`
  - Update initialization: create DiscoveryManager with config
  - Update all `addr_manager_->` calls to `discovery_manager_->`
  - Update all `anchor_manager_->` calls to `discovery_manager_->`
  - Update ADDR/GETADDR handler registrations (should be minimal changes)

- [ ] 2.5: Update ConnectionManager (PeerManager)
  - Change constructor to take `DiscoveryManager&` instead of `AddressManager&`
  - Update internal references from addr_manager_ to discovery_manager_
  - Update Good(), Failed(), Attempt() calls to go through DiscoveryManager

- [ ] 2.6: Find and update all AddressManager/AnchorManager usage
  - Search codebase for direct AddressManager/AnchorManager access
  - Update to go through NetworkManager's discovery_manager
  - Common places: connection logic, peer lifecycle, tests

- [ ] 2.7: Update tests
  - Find tests that access addr_manager or anchor_manager directly
  - Update to go through discovery_manager
  - Verify discovery_manager_for_test() provides needed access

- [ ] 2.8: Build and test
  - `cmake --build build --target network`
  - `cmake --build build --target coinbasechain_tests`
  - `./build/bin/coinbasechain_tests`

**Success Criteria**: All tests pass, AddressManager and AnchorManager owned by DiscoveryManager

---

### Phase 3: Rename PeerManager → ConnectionManager ⬜ NOT STARTED
**Goal**: Align naming with design (ConnectionManager is the proper name)

#### Tasks
- [ ] 3.1: Rename header file
  - `git mv include/network/peer_manager.hpp include/network/connection_manager.hpp`

- [ ] 3.2: Rename source file
  - `git mv src/network/peer_manager.cpp src/network/connection_manager.cpp`

- [ ] 3.3: Update file contents
  - Replace `class PeerManager` with `class ConnectionManager` in header
  - Update include guards: `PEER_MANAGER_HPP` → `CONNECTION_MANAGER_HPP`
  - Update implementation file class names

- [ ] 3.4: Update all includes
  - Find: `#include "network/peer_manager.hpp"`
  - Replace: `#include "network/connection_manager.hpp"`
  - Files to check: NetworkManager, tests, other network components

- [ ] 3.5: Update all references
  - Find: `PeerManager`
  - Replace: `ConnectionManager`
  - Find: `peer_manager_`
  - Replace: `connection_manager_`

- [ ] 3.6: Update CMakeLists.txt
  - Replace `src/network/peer_manager.cpp` with `src/network/connection_manager.cpp`

- [ ] 3.7: Update test files
  - Rename test fixtures and update class references
  - Update any test helper utilities

- [ ] 3.8: Build and test
  - `cd build && cmake ..`  (reconfigure to pick up renamed files)
  - `cmake --build build --target network`
  - `cmake --build build --target coinbasechain_tests`
  - `./build/bin/coinbasechain_tests`

**Success Criteria**: All tests pass, consistent naming with design

---

### Phase 4: Clean Up NetworkManager ⬜ NOT STARTED
**Goal**: Finalize NetworkManager as clean coordinator with 3 main managers

#### Tasks
- [ ] 4.1: Review NetworkManager member variables
  - Should have: connection_manager_, sync_manager_, discovery_manager_
  - Should have: message_dispatcher_ (routing infrastructure)
  - Should have: nat_manager_ (utility)
  - Should NOT have: Low-level components (they're now owned by the 3 managers)

- [ ] 4.2: Review NetworkManager::Initialize()
  - Verify creation order is correct (dependencies matter)
  - Verify each manager gets correct dependencies
  - Verify message handler registrations are clean

- [ ] 4.3: Review message handler registrations
  - VERACK → connection_manager_->HandleVerack()
  - ADDR → discovery_manager_->HandleAddr()
  - GETADDR → discovery_manager_->HandleGetAddr()
  - HEADERS → sync_manager_->HandleHeaders()
  - GETHEADERS → sync_manager_->HandleGetHeaders()
  - INV → sync_manager_->HandleInv()

- [ ] 4.4: Remove any leftover helper methods
  - Check for methods that should be moved to managers
  - Check for duplicate functionality

- [ ] 4.5: Update documentation
  - Update file header comments
  - Update inline documentation
  - Document manager responsibilities

- [ ] 4.6: Final build and test
  - Full clean build: `rm -rf build && mkdir build && cd build && cmake ..`
  - `cmake --build build`
  - `./build/bin/coinbasechain_tests`
  - Run any integration tests

**Success Criteria**: Clean architecture, all tests pass, clear responsibilities

---

### Phase 5: Documentation & Validation ⬜ NOT STARTED
**Goal**: Document final architecture and verify correctness

#### Tasks
- [ ] 5.1: Update architecture diagrams
  - Update NETWORK_ARCHITECTURE_REVIEW.md
  - Update any other architecture docs

- [ ] 5.2: Document manager interfaces
  - Document ConnectionManager public API
  - Document SyncManager public API
  - Document DiscoveryManager public API

- [ ] 5.3: Create architecture validation test
  - Write test that verifies ownership structure
  - Verify no circular dependencies
  - Verify clean separation of concerns

- [ ] 5.4: Measure LOC impact
  - Run git diff stats
  - Document before/after line counts
  - Calculate net LOC change

- [ ] 5.5: Performance validation
  - Run performance tests (if any exist)
  - Verify no regressions
  - Document any improvements

- [ ] 5.6: Code review checklist
  - [ ] All managers have clear, single responsibility
  - [ ] No circular dependencies
  - [ ] Ownership is clear (who owns what)
  - [ ] All tests pass
  - [ ] No memory leaks (run with sanitizers if possible)
  - [ ] Code follows project style guide

**Success Criteria**: Complete documentation, validated architecture

---

## Risk Assessment

### High Risk Items
1. **Phase 2 (DiscoveryManager upgrade)**: Most complex, touches many files
2. **Test updates**: Easy to miss test files that need updating
3. **Initialization order**: Managers have dependencies, order matters

### Mitigation Strategies
1. Build and test after EACH phase completion
2. Use grep/search to find ALL references before refactoring
3. Keep git commits atomic (one phase per commit for easy rollback)
4. Test on a branch before merging to main

## Progress Tracking

- [x] Phase 0: Planning and documentation
- [x] Phase 1: Create SyncManager ✅ COMPLETED (2025-11-03)
- [ ] Phase 2: Upgrade DiscoveryManager 🔄 IN PROGRESS
- [ ] Phase 3: Rename to ConnectionManager
- [ ] Phase 4: Clean up NetworkManager
- [ ] Phase 5: Documentation & Validation

## Notes & Decisions

### 2025-11-03: Initial Planning
- Identified that current "DiscoveryManager" is just a protocol handler
- Need to follow through on original 3-manager architecture design
- Will proceed incrementally, testing after each phase

### 2025-11-03: Phase 1 Complete
- Created SyncManager as facade over HeaderSyncManager and BlockRelayManager
- All 569 tests pass (16,368 assertions)
- Using facade pattern (references) not ownership yet
- Ownership transfer will happen in later phases when dependencies updated

### Open Questions
- [ ] Should NATManager be moved under DiscoveryManager? (NAT-PMP/UPnP for discovery)
- [ ] How to handle RPC dependencies? (Some RPC commands access managers directly)
- [ ] Should we create manager interfaces for easier testing?

---

## Definition of Done

The refactoring is complete when:
1. ✅ All 5 phases completed
2. ✅ All tests pass (16,368+ assertions)
3. ✅ Architecture matches design diagram
4. ✅ Code is documented
5. ✅ No performance regressions
6. ✅ Changes are reviewed and merged

---

**Next Action**: Phase 2 - Upgrade DiscoveryManager (make it own AddressManager + AnchorManager)
