# Phase 4 Action Plan: Merge Network Managers

**Date**: 2025-11-03
**Status**: Ready to Execute
**Estimated Time**: 7-10 days
**Branch**: `feature/network-notifications` (already pushed)

---

## Executive Summary

Phase 4 consolidates 8 managers into 4 by merging closely-related functionality. This eliminates ~600 LOC through deduplication and simplifies the architecture.

**Key Merges**:
1. **BanMan → PeerManager** (~500 LOC eliminated)
2. **HeaderSyncManager + BlockRelayManager** (merge or keep separate - TBD)
3. **AddrManager + AnchorManager** (evaluate necessity)

---

## Current Manager Inventory

| Manager | LOC | Purpose | Status |
|---------|-----|---------|--------|
| **PeerManager** | ~1500 | Connection lifecycle, permissions, DoS | ✅ Refactored (Phase 3) |
| **BanMan** | 518 | Persistent bans, discouragement | 🎯 Merge into PeerManager |
| **HeaderSyncManager** | 769 | Header synchronization, sync peer | ⚠️ Evaluate merge |
| **BlockRelayManager** | 367 | Block announcements, INV relay | ⚠️ Evaluate merge |
| **AddrManager** | ~800 | Address book, peer discovery | ✅ Keep separate |
| **AnchorManager** | ~300 | Anchor peer persistence | ⚠️ Evaluate merge |
| **NetworkManager** | ~1200 | Top-level orchestration | ✅ Keep separate |
| **MessageRouter** | ~600 | Protocol message dispatch | ✅ Keep separate |

**Total Current**: ~6,054 LOC across 8 managers
**Target**: ~4,500 LOC across 4-5 managers (25% reduction)

---

## Task 4.1: Merge BanMan into PeerManager

### Rationale
- **Tight coupling**: PeerManager already checks `BanMan::IsBanned()` on every connection
- **Single responsibility**: Both manage peer lifecycle/permissions
- **No external dependencies**: BanMan only used by PeerManager
- **Bitcoin Core alignment**: Core's `PeerManager` includes ban logic

### Current Architecture (Before)
```
NetworkManager
  ├─> PeerManager (checks IsBanned on connect)
  └─> BanMan (stores bans/discouragement)
          ↑
          └─ PeerManager calls Ban() on misbehavior
```

### Target Architecture (After)
```
NetworkManager
  └─> PeerManager (includes ban management)
        ├─ Persistent bans (from disk)
        └─ Temporary discouragement (in-memory)
```

### Implementation Steps

#### Step 1: Move BanMan data structures into PeerManager (1-2 hours)
```cpp
// Add to peer_manager.hpp
private:
  // Ban management (formerly BanMan)
  struct CBanEntry {
    int64_t nCreateTime{0};
    int64_t nBanUntil{0};
    bool IsExpired(int64_t now) const;
  };

  mutable std::mutex ban_mutex_;
  std::map<std::string, CBanEntry> banned_; // Persistent bans
  std::unordered_set<std::string> discouraged_; // Temporary (in-memory)

  std::string ban_file_path_;
  bool ban_auto_save_{true};
```

#### Step 2: Migrate BanMan methods to PeerManager (2-3 hours)
```cpp
// Add to PeerManager public API
public:
  // Ban management
  void Ban(const std::string& address, int64_t ban_time_offset = 0);
  void Unban(const std::string& address);
  bool IsBanned(const std::string& address) const;

  void Discourage(const std::string& address);
  void ClearDiscouraged();
  bool IsDiscouraged(const std::string& address) const;

  std::map<std::string, CBanEntry> GetBanned() const;
  size_t GetDiscouragedCount() const;

  // Persistence
  bool LoadBans(const std::string& datadir);
  bool SaveBans();
```

#### Step 3: Update PeerManager::add_peer to check bans internally (30 min)
```cpp
int PeerManager::add_peer(PeerPtr peer, bool is_inbound, bool is_feeler) {
  // Check bans BEFORE slot accounting
  std::string addr = normalize_ip_string(peer->address());
  if (IsBanned(addr) || IsDiscouraged(addr)) {
    LOG_NET_TRACE("Rejecting connection from banned/discouraged peer: {}", addr);
    return -1;
  }

  // ... rest of add_peer logic
}
```

#### Step 4: Update misbehavior system to call internal Ban() (1 hour)
```cpp
void PeerManager::penalize_peer(int peer_id, int penalty, const std::string& reason) {
  // ... existing misbehavior accumulation ...

  if (data.should_discourage && !HasPermission(data.permissions, NetPermissionFlags::NoBan)) {
    std::string addr = data.address;
    Discourage(addr); // Now internal call
    remove_peer(peer_id);
  }
}
```

#### Step 5: Remove BanMan from NetworkManager (1 hour)
```cpp
// In network_manager.hpp - DELETE:
// std::unique_ptr<BanMan> ban_man_;
// BanMan& ban_man() { return *ban_man_; }

// In network_manager.cpp - DELETE:
// ban_man_ = std::make_unique<BanMan>(datadir);
// ban_man_->Load();

// INSTEAD: Call PeerManager::LoadBans() during initialization
peer_manager_->LoadBans(datadir);
```

#### Step 6: Update all call sites (2-3 hours)
```bash
# Find all BanMan usage
grep -rn "ban_man()" src/ include/

# Replace with PeerManager methods:
# network_manager.ban_man().IsBanned(addr)  →  peer_manager_.IsBanned(addr)
# network_manager.ban_man().Ban(addr, 0)   →  peer_manager_.Ban(addr, 0)
```

#### Step 7: Update tests (2-3 hours)
- Move `test/network/banman_tests.cpp` → `test/network/peer_manager_bans_tests.cpp`
- Update test fixtures to use PeerManager instead of BanMan
- Verify persistence tests still pass
- Verify misbehavior → ban flow works

#### Step 8: Delete BanMan files (5 min)
```bash
git rm include/network/banman.hpp src/network/banman.cpp
git commit -m "Refactor: Merge BanMan into PeerManager"
```

### Testing Checklist
- [ ] Ban persistence (Load/Save) works
- [ ] Discouraged peers rejected on connection
- [ ] Banned peers rejected on connection
- [ ] NoBan permission bypasses checks
- [ ] Misbehavior triggers discouragement
- [ ] RPC ban commands work (if any)
- [ ] All existing tests pass

### Rollback Plan
If issues arise:
1. `git revert` the merge commit
2. Restore BanMan files from previous commit
3. Re-run tests to verify clean rollback

---

## Task 4.2: Evaluate HeaderSyncManager + BlockRelayManager Merge

### Analysis Required (1 day investigation)

**Question**: Should we merge these two managers or keep them separate?

#### Arguments FOR Merging:
1. **Tight coupling**: BlockRelayManager calls `HeaderSyncManager::RequestHeadersFromPeer()` on INV
2. **Single domain**: Both manage blockchain synchronization
3. **Shared state**: Both track sync peer, sync status
4. **Bitcoin Core**: Core has single `PeerManager` that handles both

#### Arguments AGAINST Merging:
1. **Separation of concerns**: Headers-only vs full block relay
2. **Already small**: 367 LOC (BlockRelay) is manageable
3. **Different lifecycles**: Header sync completes during IBD, block relay is ongoing
4. **Clear interfaces**: Current split is clean and testable

### Investigation Tasks
- [ ] Count cross-manager calls (how many times does BlockRelay call HeaderSync?)
- [ ] Check for duplicate logic (any code duplication between them?)
- [ ] Review Bitcoin Core architecture (how does Core organize this?)
- [ ] Measure test complexity (would merged tests be simpler or more complex?)

### Decision Criteria
**Merge IF**:
- Cross-manager calls > 5 locations
- Significant code duplication (>20%)
- Shared state causes race conditions

**Keep Separate IF**:
- Clean interfaces, minimal coupling
- Different testing needs
- Easier to understand as separate concerns

### If Merge Decision: Estimated 3-5 days
1. Create `SyncManager` combining both
2. Migrate header sync logic
3. Migrate block relay logic
4. Update NetworkManager wiring
5. Update all call sites
6. Rewrite tests

### If Keep Separate: Estimated 1 day
1. Document rationale in architecture docs
2. Add integration tests for HeaderSync ↔ BlockRelay interaction
3. Ensure clear API boundaries

---

## Task 4.3: Evaluate AddrManager + AnchorManager Merge

### Analysis Required (1 day investigation)

**Question**: Should anchor persistence be part of AddrManager?

#### Current Architecture
```
AddrManager
  - Peer address book (~2000 addresses)
  - Good/tried buckets
  - Peer selection logic

AnchorManager
  - Persist ~2 anchor addresses on shutdown
  - Load anchors on startup
  - Single-purpose: connection reliability
```

#### Arguments FOR Merging:
1. **Related domain**: Both manage addresses
2. **Small size**: AnchorManager is only ~300 LOC
3. **Simplification**: One less manager to maintain

#### Arguments AGAINST Merging:
1. **Different purpose**: AddrManager = discovery, AnchorManager = persistence
2. **Different lifecycle**: Anchors only touched on startup/shutdown
3. **Minimal coupling**: AnchorManager doesn't depend on AddrManager internals
4. **Clear responsibility**: Small, focused, easy to understand

### Investigation Tasks
- [ ] Check if AnchorManager needs AddrManager data (address quality, etc.)
- [ ] Count interaction points
- [ ] Review Bitcoin Core design

### Recommendation: KEEP SEPARATE (Tentative)
Reasoning:
- AnchorManager is already tiny and focused
- No apparent code duplication
- Different lifecycles (startup/shutdown vs continuous)
- Merging saves minimal LOC, adds complexity

---

## Task 4.4: Final Cleanup & Documentation (1-2 days)

### After All Merges Complete

#### Update Architecture Docs
- [ ] Update `NETWORK_ARCHITECTURE_REVIEW.md` with new manager count
- [ ] Create `docs/MANAGER_RESPONSIBILITIES.md` clarifying final architecture
- [ ] Update dependency diagrams

#### Code Quality
- [ ] Run `cloc` to measure final LOC reduction
- [ ] Check for any orphaned code from merges
- [ ] Ensure all public APIs have Doxygen comments
- [ ] Run static analysis (if available)

#### Testing
- [ ] Full test suite: `./build/bin/coinbasechain_tests`
- [ ] Run tests 3x to catch any flaky tests
- [ ] Verify no performance regressions (if benchmarks exist)

#### Git Hygiene
- [ ] Squash WIP commits if needed
- [ ] Write comprehensive commit messages
- [ ] Update CHANGELOG

---

## Phase 4 Success Criteria

| Metric | Current | Target | Status |
|--------|---------|--------|--------|
| Manager count | 8 | 4-5 | ⏳ |
| Total LOC (network) | ~6,054 | ~4,500 | ⏳ |
| Circular dependencies | 0 | 0 | ✅ (from Phase 1) |
| Test pass rate | 100% | 100% | ✅ |
| Code duplication | ~15% | <10% | ⏳ |

### Definition of Done
- [ ] BanMan merged into PeerManager
- [ ] Decision made on HeaderSync/BlockRelay merge (merged or documented rationale)
- [ ] Decision made on AddrManager/AnchorManager merge (merged or documented rationale)
- [ ] All 593+ tests passing
- [ ] No regressions in functionality
- [ ] Documentation updated
- [ ] Code reviewed and committed

---

## Risk Assessment

### High Risk
- **BanMan merge**: Persistence logic is critical, ban file format must not change

### Medium Risk
- **Test migration**: Moving 50+ ban tests to new location could introduce gaps

### Low Risk
- **HeaderSync/BlockRelay**: Well-tested, decision to keep separate is low-risk

### Mitigation Strategies
1. **Incremental commits**: Each subtask is a separate commit for easy rollback
2. **Test-first**: Update tests before moving code
3. **Persistence testing**: Manually test ban file Load/Save before and after
4. **Backup**: Branch is already pushed to remote

---

## Timeline Estimate

| Task | Estimated Time | Dependencies |
|------|---------------|--------------|
| 4.1: Merge BanMan | 2 days | None |
| 4.2: Investigate HeaderSync/BlockRelay | 1 day | None (can run parallel) |
| 4.2: Execute merge (if needed) | 3-5 days | Investigation complete |
| 4.3: Investigate AddrManager/Anchor | 1 day | None (can run parallel) |
| 4.3: Execute merge (if needed) | 1-2 days | Investigation complete |
| 4.4: Cleanup & docs | 1-2 days | All merges complete |

**Total (if merging all)**: 9-13 days
**Total (if only BanMan)**: 4-5 days

**Recommendation**: Start with Task 4.1 (BanMan merge), then decide on others based on investigation.

---

## Next Steps

1. Review this plan with team/stakeholders
2. Begin Task 4.1: BanMan → PeerManager merge
3. Create sub-branch `feature/merge-banman` for easier review
4. Execute step-by-step with commits per subtask
5. Open PR for review after each major merge

---

## References
- Phase 1-3 work: `feature/network-notifications` branch
- Bitcoin Core PeerManager: `src/net_processing.cpp`
- Design doc: `NETWORK_REFACTORING_ACTION_PLAN.md`
