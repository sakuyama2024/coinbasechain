# Test Coverage Gap Analysis (REVISED): Coinbase Chain vs Bitcoin Core

## What We Actually Have

After reviewing our existing test infrastructure:

### ✅ **Strong Integration Test Infrastructure**
- `SimulatedNetwork` + `SimulatedNode` for full P2P simulation
- `AttackSimulatedNode` for adversarial testing
- Mock time support via `network.AdvanceTime()`
- 20+ functional tests (Python-based)
- 9+ integration test files (C++, using simulated network)
- 18+ network tests (C++, using simulated network)

### ✅ **What Our Integration Tests DO Cover**
1. **Misbehavior penalties trigger disconnection** (`misbehavior_penalty_tests.cpp`)
   - Invalid PoW → instant disconnect
   - Oversized messages → disconnect after 5
   - Low-work headers → disconnect after 10
2. **BanMan integration** - Tests check `victim.IsBanned()`
3. **Full message flow** - Actual message sending/receiving
4. **Time-based behavior** - Uses `AdvanceTime()`
5. **Multi-peer scenarios** - Attack simulations with multiple nodes
6. **Orphan handling** - Comprehensive orphan attack tests

### ❌ **Critical Gap: NoBan Permissions Not Tested in Integration**

**The NoBan bug we just found proves this:**

```cpp
// Unit test (dos_protection_tests.cpp) - TESTED THIS ✅
TEST: NoBan peer gets high score but ShouldDisconnect() = false

// Integration test - MISSING ❌
TEST: NoBan peer sends invalid data in real network
      → Message handler calls ReportInvalidPoW()
      → Score tracked
      → ShouldDisconnect() = false
      → Peer stays connected
      → Can continue communicating
```

**Why we missed it:**
- Unit test verified the API worked correctly
- Integration tests didn't cover permission flags
- `SimulatedNode` doesn't support permissions

## Specific Gaps vs Bitcoin

### 1. **Permission Testing**

**Bitcoin has:**
- Functional test `p2p_permissions.py` - comprehensive permission testing
- Permission flags passed when creating connections
- Tests verify NoBan, forcerelay, all permission types

**We have:**
- Unit test for NoBan (just added)
- ❌ No integration tests with permissions
- ❌ No functional tests for permissions
- ❌ `SimulatedNode` doesn't support permission flags

**Fix needed:**
```cpp
// Add to SimulatedNode
class SimulatedNode {
    NetPermissionFlags default_permissions_ = NetPermissionFlags::None;

    void SetDefaultPermissions(NetPermissionFlags flags);
    void ConnectTo(int peer_id, NetPermissionFlags permissions);
};

// Then test
TEST_CASE("NoBan peer survives misbehavior") {
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);

    attacker.ConnectTo(1, NetPermissionFlags::NoBan);  // ← KEY
    attacker.SendInvalidPoWHeaders(...);

    // Peer should stay connected despite misbehavior
    CHECK(victim.GetPeerCount() == 1);
    CHECK(!victim.IsBanned(attacker.GetAddress()));
}
```

### 2. **Systematic Coverage Checklist**

Looking at Bitcoin's `denialofservice_tests.cpp`, they test:

**✅ We have:**
- Misbehavior scoring
- Disconnect on threshold
- BanMan integration
- Time-based behaviors
- Multi-peer scenarios

**❌ We're missing:**
- **Permission interactions** (NoBan, Manual, etc.)
- **Slow peer eviction** (peers that don't send headers timeout)
- **Stale tip management** (what happens when tip gets stale)
- **BanMan persistence** (save/reload ban list)
- **Ban expiry** (bans expire after duration)

### 3. **Unit Tests vs Integration Tests Balance**

**Current split:**
- Unit tests: ~12 files
- Integration tests: ~9 files
- Network tests: ~18 files
- Functional tests: ~20 files

**Ratio looks good**, but **coverage gaps remain:**

**Unit tests:**
- ✅ Component APIs work correctly
- ✅ Edge cases handled
- ❌ Don't test integration points (like we saw with NoBan)

**Integration/Network tests:**
- ✅ Full message flow
- ✅ Multi-peer interactions
- ✅ Attack scenarios
- ❌ Missing permission testing
- ❌ Missing some time-based scenarios

## Root Cause of NoBan Bug

The NoBan bug slipped through because:

1. ✅ **Unit test passed** - `dos_protection_tests.cpp` correctly tested the API
2. ❌ **Integration test missing** - No test verified NoBan works in actual network
3. ❌ **Infrastructure gap** - `SimulatedNode` doesn't support permissions

**The fix required production code change** because we were missing **behavioral testing of permissions in realistic scenarios**.

## Action Items (Prioritized)

### Priority 1: Add Permission Support to Test Infrastructure

```cpp
// test/network/simulated_node.hpp
class SimulatedNode {
public:
    // Add permission support
    void ConnectTo(int peer_id, NetPermissionFlags permissions = NetPermissionFlags::None);
    void SetDefaultPermissions(NetPermissionFlags flags);

protected:
    NetPermissionFlags default_permissions_{NetPermissionFlags::None};
};
```

### Priority 2: Add Permission Integration Tests

```cpp
// test/network/permission_integration_tests.cpp
TEST_CASE("NoBan peer survives invalid PoW") {
    // ... (as shown above)
}

TEST_CASE("NoBan peer can be manually disconnected") {
    // Verify we can still manually disconnect NoBan peers
}

TEST_CASE("Multiple permission flags work together") {
    // Test Manual | NoBan, etc.
}
```

### Priority 3: Add Missing Bitcoin-Style Tests

```cpp
// test/network/peer_eviction_tests.cpp
TEST_CASE("Slow peer evicted for not sending headers") {
    // Like Bitcoin's outbound_slow_chain_eviction test
}

TEST_CASE("Stale tip triggers peer replacement") {
    // Like Bitcoin's stale_tip_peer_management test
}
```

### Priority 4: Add Functional Permission Tests

```python
# test/functional/p2p_permissions.py
class PermissionTests:
    def test_noban_permission(self):
        # Connect with noban flag
        # Send invalid data
        # Verify peer stays connected

    def test_permission_combinations(self):
        # Test various flag combinations
```

## What We Don't Need

After review, we already have:
- ✅ Comprehensive orphan attack tests
- ✅ DoS protection tests (basic)
- ✅ Time-based testing support
- ✅ BanMan integration in tests
- ✅ Multi-peer scenarios

## Conclusion

**Our test infrastructure is solid**, but we have a **specific gap**: **permission testing**.

**The NoBan bug occurred because:**
1. We test components in isolation (unit tests) ✅
2. We test full system behavior (integration tests) ✅
3. But we don't test **permissions in integration tests** ❌

**The fix is focused:**
1. Add permission support to `SimulatedNode` (small change)
2. Add ~5-10 permission integration tests
3. Consider adding functional permission tests

**This is NOT a fundamental testing problem** - it's a specific missing test category that we can address incrementally.

## Immediate Next Step

Before adding any more features, add permission integration tests to catch bugs like the NoBan issue before they reach production.
