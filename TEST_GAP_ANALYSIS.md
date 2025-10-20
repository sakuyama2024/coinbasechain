# Test Coverage Gap Analysis: Coinbase Chain vs Bitcoin Core

## Summary of Findings

After reviewing Bitcoin Core's test suite (particularly `denialofservice_tests.cpp`, `peerman_tests.cpp`, and functional tests), we identified significant gaps in our test coverage that may explain why we keep finding issues in production code.

## What Bitcoin Tests That We're Missing

### 1. **Peer Manager Integration Tests** (Unit Level)

**Bitcoin has (`denialofservice_tests.cpp`):**
- ✅ Misbehavior scoring with actual peer disconnection flow
- ✅ BanMan integration (discouraged addresses added to ban list)
- ✅ Multiple peers with independent scores
- ✅ Outbound vs Inbound peer distinction in misbehavior handling
- ✅ Slow chain eviction (peers that don't send headers)
- ✅ Stale tip management and peer eviction
- ✅ Time-based banning behavior

**We have (`dos_protection_tests.cpp`):**
- ✅ Misbehavior scoring basic API
- ✅ Permission flags (NoBan)
- ✅ Score thresholds
- ❌ No BanMan integration
- ❌ No actual peer disconnection testing
- ❌ No time-based behavior
- ❌ No multi-peer interaction scenarios

**Gap:** Our tests only verify the scoring API, not the full disconnection and banning workflow.

### 2. **Permission System Tests**

**Bitcoin has:**
- ✅ Functional test for all permission types (`p2p_permissions.py`)
- ✅ Permission merging (whitelist + whitebind)
- ✅ Permission validation (invalid permissions rejected)
- ✅ forcerelay permission behavior
- ✅ NoBan permission prevents disconnection (implicit in code, not explicit test)

**We have:**
- ✅ NoBan prevents disconnect
- ✅ Manual connection flag handling
- ❌ No other permission types tested
- ❌ No permission parsing/validation tests
- ❌ No functional tests for permissions

**Gap:** We only test NoBan/Manual, not the broader permission system.

### 3. **Message Handling with Misbehavior**

**Bitcoin tests:**
- Real message processing that triggers misbehavior
- Invalid messages cause appropriate penalties
- Multiple sequential violations accumulate correctly

**We test:**
- Direct API calls (`ReportLowWorkHeaders()`, etc.)
- ❌ Not testing actual message handling that leads to penalties

**Gap:** We're not testing the integration between message handlers and misbehavior system.

### 4. **Network Manager + PeerManager Integration**

**What's missing:**
- Test that NetworkManager correctly calls PeerManager.Report*() methods
- Test that ShouldDisconnect() is checked after penalties
- Test that BanMan is called when peers are disconnected
- Test the full flow: bad message → penalty → threshold → disconnect → ban

**Current issue:** We found the NoBan bug because the test expected one behavior but production had another. This suggests we're not testing the full integration.

### 5. **Headers-Only Specific Tests**

**Bitcoin tests headers thoroughly:**
- `p2p_dos_header_tree.py` - DoS protection for header tree attacks
- `p2p_headers_sync_with_minchainwork.py` - Minimum chainwork requirements
- Low-work header protection (integrated into net_processing)
- Non-continuous headers handling
- Orphan header limits

**We have:**
- ✅ Low-work header integration test (just added)
- ✅ Orphan tests
- ❌ No header tree DoS tests
- ❌ No systematic header sync tests

### 6. **Test Infrastructure Gaps**

**Bitcoin has:**
- `TestingSetup` / `RegTestingSetup` fixtures
- Mock time support for time-based tests
- `UnitTestMisbehaving()` - special testing interface
- Test-only methods for validation
- Extensive test utilities (`test/util/net.h`, `test/util/setup_common.h`)

**We have:**
- Custom `SimulatedNetwork`/`SimulatedNode` (good for integration tests)
- ❌ No comprehensive unit test fixtures
- ❌ No mock time support
- ❌ No testing-only interfaces for production code
- ❌ Limited test utilities

## Root Cause Analysis

### Why We Keep Finding Issues

1. **Testing API, Not Behavior**
   - Our tests call `ReportLowWorkHeaders()` directly
   - Don't test the actual code path that leads to calling it
   - Miss integration issues (like NoBan handling)

2. **Missing Integration Tests**
   - PeerManager tested in isolation
   - NetworkManager tested separately
   - Not testing them together

3. **No BanMan Integration**
   - We have `BanMan` class but don't test it with PeerManager
   - Don't verify discouraged peers are actually banned

4. **Limited Scenario Coverage**
   - Don't test time-based behaviors
   - Don't test complex multi-peer scenarios
   - Don't test edge cases (e.g., peer disconnects while being penalized)

## Recommended Additions

### Priority 1: Integration Tests (Most Important)

```cpp
// test/integration/peer_misbehavior_integration_tests.cpp
BOOST_AUTO_TEST_CASE(misbehaving_peer_gets_banned) {
    // Setup: NetworkManager + PeerManager + BanMan
    // Send invalid headers
    // Verify: penalty applied → ShouldDisconnect → BanMan.Discourage called
    // Verify: peer actually disconnected and can't reconnect
}

BOOST_AUTO_TEST_CASE(noban_peer_tracked_but_not_disconnected) {
    // Setup: Peer with NoBan permission
    // Send invalid data (exceeds threshold)
    // Verify: score tracked, but peer NOT disconnected, NOT banned
}
```

### Priority 2: Message Handler Tests

```cpp
// test/unit/message_handler_dos_tests.cpp
BOOST_AUTO_TEST_CASE(invalid_header_triggers_penalty) {
    // Send actual HEADERS message with invalid PoW
    // Verify ReportInvalidPoW was called
    // Verify peer disconnected
}
```

### Priority 3: BanMan Tests

```cpp
// test/unit/banman_tests.cpp
BOOST_AUTO_TEST_CASE(discouraged_addresses_persisted) {
    // Discourage address
    // Save ban list
    // Restart
    // Verify address still discouraged
}
```

### Priority 4: Time-Based Tests

```cpp
// Requires mock time support
BOOST_AUTO_TEST_CASE(ban_expires_after_duration) {
    // Ban peer
    // Advance mock time
    // Verify ban expires
}
```

## Specific Bitcoin Tests to Replicate

Based on priority and applicability to our headers-only chain:

1. **`DoS_banning` test** - Full misbehavior → ban → reconnect prevention flow
2. **`outbound_slow_chain_eviction` test** - Timeout-based peer management
3. **`stale_tip_peer_management` test** - Managing peers when tip is stale

## Action Items

1. **Create test utilities** (`test/util/test_setup.hpp`)
   - Common fixtures for NetworkManager + PeerManager + BanMan
   - Mock time support
   - Helper functions for creating test peers

2. **Add integration test suite** (`test/integration/peer_dos_integration_tests.cpp`)
   - Full message → penalty → disconnect → ban flow
   - Permission handling (NoBan, Manual)
   - Multi-peer scenarios

3. **Audit existing tests**
   - Identify which tests are actually unit tests (isolated component)
   - Identify which are integration tests (multiple components)
   - Move to appropriate directories

4. **Add BanMan tests** (`test/unit/banman_tests.cpp`)
   - Persistence
   - Expiry (requires mock time)
   - Address matching

5. **Test actual message handlers**
   - Not just the Report*() APIs
   - The actual code paths in NetworkManager that detect violations

## Conclusion

Our test coverage focuses too heavily on **API correctness** rather than **behavioral correctness**. Bitcoin's tests verify the entire system works together correctly, while ours verify individual components work in isolation. This is why we keep finding integration issues when we add new features or fix bugs.

**Key takeaway:** We need more integration tests that verify end-to-end behavior, not just unit tests that verify individual methods work.
