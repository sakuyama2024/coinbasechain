# test2 - Rewritten Tests with Improved Framework

## Overview

This directory contains **rewritten from-scratch tests** using the improved testing framework. Unlike the original `test/` directory, these tests were built cleanly without referencing old brittle patterns.

## Why test2?

The original tests in `test/network/` suffered from:
- Manual timing loops with magic numbers
- Confusing node_id with peer_id
- Wrong ban list checks (IsBanned vs IsDiscouraged)
- No debugging info when tests fail
- Brittle, hard-to-maintain code

Rather than incrementally fix these issues, we started fresh with better patterns.

## New Framework Components

### 1. TestOrchestrator (`test_orchestrator.hpp`)
High-level test coordinator that eliminates timing brittleness:
- `WaitForConnection()` - No more magic number loops
- `WaitForSync()` - Handles async operations automatically
- `AssertPeerDiscouraged()` - Correct ban checks with helpful errors
- `GetPeerId()` - Automatic node_id → peer_id mapping

### 2. NetworkObserver (`network_observer.hpp`)
Event capture system for debugging:
- Records all network events with timestamps
- `AutoDumpOnFailure` - Dumps full timeline when test fails
- Shows exactly what happened instead of cryptic errors

### 3. MaliciousBehavior (`malicious_behaviors.hpp`)
Composable attack patterns:
- `DropMessagesBehavior`, `DelayMessagesBehavior`
- `CorruptHeadersBehavior`, `StallResponsesBehavior`
- `SelfishMiningBehavior`, `OversizedMessageBehavior`
- `BehaviorChain` - Compose multiple attacks

## Test Files

### `dos_attack_tests.cpp` (352 lines)
DoS attack protection tests:
- ✅ Invalid PoW headers trigger discourage
- ✅ Orphan header spam triggers protection
- ✅ Oversized message triggers disconnect
- ✅ Low-work headers ignored without penalty (Bitcoin Core behavior)
- ✅ Stalling peer timeout
- ✅ Multiple attack vectors combined

### `permission_tests.cpp` (387 lines)
NetPermissionFlags integration tests:
- ✅ Normal peer gets disconnected for invalid PoW
- ✅ NoBan peer survives invalid PoW
- ✅ NoBan peer survives orphan spam
- ✅ NoBan vs Normal peer comparison
- ✅ NoBan with multiple attack types
- ✅ Score tracking works for NoBan peers

## Building and Running

```bash
# From project root
mkdir build && cd build
cmake ..
make coinbasechain_tests2

# Run all tests
./coinbasechain_tests2

# Run specific tag
./coinbasechain_tests2 "[dos]"
./coinbasechain_tests2 "[permissions]"
./coinbasechain_tests2 "[noban]"

# Verbose output
./coinbasechain_tests2 -v
```

## Example: Before vs After

### Before (Brittle)
```cpp
// From old test/network/attack_simulation_tests.cpp
for (int i = 0; i < 15; i++) {
    time_ms += 100;
    network.AdvanceTime(time_ms);
}
CHECK(victim.GetPeerCount() == 1);  // Why 15 iterations?

auto& peer_manager = victim.GetNetworkManager().peer_manager();
int score = peer_manager.GetMisbehaviorScore(attacker.GetId());  // WRONG!
```

### After (Robust)
```cpp
// From test2/network/dos_attack_tests.cpp
REQUIRE(orchestrator.WaitForConnection(victim, attacker));
orchestrator.AssertMisbehaviorScore(victim, attacker, 100);
// If fails: Full timeline shows MSG_SEND, VALIDATION_FAIL, MISBEHAVIOR events
```

## Key Improvements

| Aspect | Old (test/) | New (test2/) |
|--------|------------|--------------|
| **Timing** | Manual loops, magic numbers | `orchestrator.WaitFor*()` |
| **ID Mapping** | Manual, error-prone | `orchestrator.GetPeerId()` |
| **Assertions** | Wrong methods, cryptic errors | Correct + helpful messages |
| **Debugging** | "0 >= 100" - useless | Full event timeline |
| **Readability** | Unclear intentions | Phase markers, clean structure |
| **Maintainability** | Brittle, hard to fix | Composable, extensible |

## Test Coverage

**Original tests converted:**
- ✅ `attack_simulation_tests.cpp` → `dos_attack_tests.cpp`
  - OrphanSpamAttack → "DoS: Orphan header spam"
  - OrphanChainGrinding → Combined into orphan tests
  - FakeOrphanParentAttack → "DoS: Stalling peer timeout"

- ✅ `low_work_headers_test.cpp` → `dos_attack_tests.cpp`
  - Low-work ignored test → "DoS: Low-work headers ignored"
  - High-work accepted test → Implicit in sync tests

- ✅ `permission_integration_tests.cpp` → `permission_tests.cpp`
  - NoBan invalid PoW → "Permission: NoBan peer survives invalid PoW"
  - NoBan low-work → "Permission: NoBan peer survives orphan spam"
  - Normal vs NoBan → "Permission: NoBan vs Normal comparison"

**Total: 12 comprehensive tests** covering all major attack vectors and permission scenarios.

## When to Use

- ✅ **Use test2 for**: New tests, complex scenarios, multi-node tests
- ⚠️ **Keep test/ for**: Existing unit tests, simple cases, backwards compatibility
- 🎯 **Migration path**: Gradually move tests from `test/` to `test2/` as they break

## Documentation

See `test/network/IMPROVED_TESTING.md` for complete framework documentation including:
- API reference for all components
- Migration guide from old patterns
- Best practices and troubleshooting
- Performance considerations

## Future Tests to Add

Priority tests to rewrite next:
1. `peer_connection_tests.cpp` - Connection management
2. `sync_ibd_tests.cpp` - Initial block download
3. `reorg_partition_tests.cpp` - Network partitions and reorgs
4. `peer_discovery_tests.cpp` - Address manager and peer discovery

## Summary

**test2 demonstrates how the improved framework makes tests:**
- ✅ More reliable (no timing brittleness)
- ✅ More readable (high-level operations)
- ✅ Easier to debug (full event timeline)
- ✅ More maintainable (composable patterns)

All tests are **built from scratch** - no old baggage, just clean modern patterns.
