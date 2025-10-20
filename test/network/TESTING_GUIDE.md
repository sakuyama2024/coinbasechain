# Network Testing Guide

This guide helps developers (including AI coding agents) write robust tests for the P2P networking layer using the SimulatedNetwork infrastructure.

## Table of Contents
- [Quick Reference for AI Agents](#quick-reference-for-ai-agents)
- [Architecture Overview](#architecture-overview)
- [Common Pitfalls](#common-pitfalls)
- [Best Practices](#best-practices)
- [Debugging Tips](#debugging-tips)
- [Example Patterns](#example-patterns)

---

## Quick Reference for AI Agents

### Error → Fix Lookup Table

| Symptom / Error Message | Root Cause | Fix |
|------------------------|------------|-----|
| `REQUIRE(score >= 100)` fails with `0 >= 100` | Using node_id instead of peer_id | Use `peer_manager.get_all_peers()[0]->id()` to get peer_id |
| `REQUIRE(IsBanned(...))` fails | Checking wrong ban list | Use `GetBanMan().IsDiscouraged()` instead of `IsBanned()` |
| `Received verack before VERSION` | Handshake race condition | Verify first peer connected (`GetPeerCount() == 1`) before starting second connection |
| `CheckProofOfWork: result=false` but no disconnect | PoW validation working but wrong timing | Move `SetBypassPOWValidation(false)` to AFTER sync, BEFORE attack |
| Headers accepted during sync but rejected during attack | PoW bypass disabled too early | Keep bypass enabled during mining and sync, disable only for attack validation |
| Peer count expectations fail | Async operations not complete | Add more `AdvanceTime()` iterations (15-20) |

### Code Pattern Recognition

**IF you see:** `peer_manager.GetMisbehaviorScore(attacker.GetId())`
**REPLACE WITH:**
```cpp
auto peers = peer_manager.get_all_peers();
REQUIRE(peers.size() == 1);
int peer_id = peers[0]->id();
int score = peer_manager.GetMisbehaviorScore(peer_id);
```

**IF you see:** `REQUIRE(victim.IsBanned(attacker.GetAddress()))`
**REPLACE WITH:** `REQUIRE(victim.GetBanMan().IsDiscouraged(attacker.GetAddress()))`

**IF you see:** `SetBypassPOWValidation(false)` before `ConnectTo()`
**MOVE IT TO:** After connection completes, before sending attack

**IF you see:** Two connections without delay
**ADD BETWEEN THEM:**
```cpp
REQUIRE(victim.GetPeerCount() == 1);  // Verify first connected
for (int i = 0; i < 10; i++) {
    time_ms += 100;
    network.AdvanceTime(time_ms);
}
```

### Critical Rules (Always Apply)

```
RULE 1: node_id ≠ peer_id
  - node_id: assigned in SimulatedNode constructor
  - peer_id: assigned by PeerManager on connection
  - Action: Always use get_all_peers() to map node to peer

RULE 2: Discourage ≠ Ban
  - Ban: permanent (m_banned map, checked by IsBanned())
  - Discourage: temporary 24h (m_discouraged map, checked by IsDiscouraged())
  - Action: DoS protection uses Discourage, so check IsDiscouraged()

RULE 3: PoW Bypass Timing
  - Enable during: mining, initial sync
  - Disable before: attack message validation
  - Never disable before: peer connection and sync

RULE 4: Sequential Peer Connections
  - Always verify: first peer connected before second connection
  - Always delay: 10-20 AdvanceTime() cycles between connections
  - Reason: avoid VERSION/VERACK handshake race condition

RULE 5: Async Operation Timing
  - Minimum cycles: 15-20 AdvanceTime() iterations
  - After connection: 15-20 iterations before checking peer count
  - After attack: 20+ iterations before checking disconnect/ban
```

### Test Template Selection

**Use Pattern 1 (Basic DoS)** when:
- Testing single attacker
- Testing DoS protection (invalid PoW, oversized messages, etc.)
- Need to verify peer gets discouraged

**Use Pattern 2 (Permission)** when:
- Testing NoBan or Manual permissions
- Verifying peer stays connected despite misbehavior
- Checking misbehavior score tracking

**Use Pattern 3 (Multi-Peer)** when:
- Testing different permissions on different peers
- Comparing normal vs NoBan behavior
- Need 2+ simultaneous connections

---

## Architecture Overview

### Test Infrastructure Components

```
┌─────────────────────────────────────────────────────────────┐
│ SimulatedNetwork                                            │
│ - Manages simulated time                                   │
│ - Routes messages between nodes                            │
│ - Deterministic, no real network I/O                       │
└─────────────────────────────────────────────────────────────┘
                          │
                          │ registers nodes
                          ▼
┌─────────────────────────────────────────────────────────────┐
│ SimulatedNode                                               │
│ - Uses REAL P2P components (NetworkManager, PeerManager)   │
│ - TestChainstateManager with optional PoW bypass           │
│ - NetworkBridgedTransport routes through SimulatedNetwork  │
└─────────────────────────────────────────────────────────────┘
                          │
                          │ inherits from
                          ▼
┌─────────────────────────────────────────────────────────────┐
│ AttackSimulatedNode                                         │
│ - Sends malicious P2P messages                             │
│ - Bypasses normal validation for testing DoS protection    │
└─────────────────────────────────────────────────────────────┘
```

### Key Concepts

**Node ID vs Peer ID:**
- **Node ID**: Assigned when SimulatedNode is created (e.g., `SimulatedNode victim(1, &network)` → node_id = 1)
- **Peer ID**: Assigned by PeerManager when connection is established (usually 0, 1, 2, ... in order of connection)
- **⚠️ NEVER use node_id to query PeerManager!** Use `peer_manager.get_all_peers()` to get actual peer_id

**Ban vs Discourage:**
- **Ban**: Permanent or long-term ban (stored in `m_banned` map)
- **Discourage**: Temporary ~24h ban (stored in `m_discouraged` map)
- **⚠️ DoS protection calls `Discourage()`, not `Ban()`!** Check `IsDiscouraged()`, not `IsBanned()`

**PoW Bypass:**
- `TestChainstateManager` has `bypass_pow_validation_` flag (default: true)
- When true: blocks accepted without PoW validation (fast for testing)
- When false: real PoW validation (needed to test attack detection)

---

## Common Pitfalls

### 1. ❌ Using Node ID Instead of Peer ID

**Wrong:**
```cpp
auto& peer_manager = victim.GetNetworkManager().peer_manager();
int score = peer_manager.GetMisbehaviorScore(attacker.GetId());  // ❌ Using node_id!
```

**Correct:**
```cpp
auto& peer_manager = victim.GetNetworkManager().peer_manager();
auto peers = peer_manager.get_all_peers();
REQUIRE(peers.size() == 1);
int peer_id = peers[0]->id();  // ✅ Get actual peer_id from PeerManager
int score = peer_manager.GetMisbehaviorScore(peer_id);
```

---

### 2. ❌ Checking IsBanned() Instead of IsDiscouraged()

**Wrong:**
```cpp
// Send invalid PoW to trigger DoS protection
attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 1);
network.AdvanceTime(time_ms += 1000);

REQUIRE(victim.IsBanned(attacker.GetAddress()));  // ❌ DoS uses Discourage, not Ban!
```

**Correct:**
```cpp
// Send invalid PoW to trigger DoS protection
attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 1);
network.AdvanceTime(time_ms += 1000);

REQUIRE(victim.GetBanMan().IsDiscouraged(attacker.GetAddress()));  // ✅ Check discourage list
```

---

### 3. ❌ Disabling PoW Bypass Too Early

**Wrong:**
```cpp
// Mine victim's chain
for (int i = 0; i < 5; i++) {
    victim.MineBlock();
}

// Disable bypass BEFORE peers connect
victim.SetBypassPOWValidation(false);  // ❌ Breaks initial sync!

// Peer connects and syncs victim's chain
attacker.ConnectTo(1);
network.AdvanceTime(time_ms += 1000);
// ^ Peer tries to validate victim's bypass-mined blocks and fails!
```

**Correct:**
```cpp
// Mine victim's chain (WITH bypass enabled)
for (int i = 0; i < 5; i++) {
    victim.MineBlock();
    network.AdvanceTime(time_ms += 50);
}

// Peer connects and syncs (bypass still enabled)
attacker.ConnectTo(1);
for (int i = 0; i < 10; i++) {
    network.AdvanceTime(time_ms += 100);
}

// NOW disable bypass (AFTER sync, BEFORE attack)
victim.SetBypassPOWValidation(false);  // ✅ Only affects future validation

// Send attack
attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 1);
```

**Rule of Thumb:**
- Mine blocks WITH bypass enabled (fast)
- Let peers sync WITH bypass enabled (fast)
- Disable bypass ONLY when you need to validate attack messages

---

### 4. ❌ Handshake Race Conditions with Multiple Peers

**Wrong:**
```cpp
// Connect two peers quickly
normal_peer.ConnectTo(1);
network.AdvanceTime(time_ms += 100);  // Not enough time!

noban_peer.ConnectTo(1);  // ❌ Second connection too fast!
network.AdvanceTime(time_ms += 100);

REQUIRE(victim.GetPeerCount() == 2);  // Fails! One peer disconnected due to protocol violation
```

**Correct:**
```cpp
// Connect first peer
normal_peer.ConnectTo(1);
for (int i = 0; i < 20; i++) {
    network.AdvanceTime(time_ms += 100);
}

// Verify first connection fully established
REQUIRE(victim.GetPeerCount() == 1);  // ✅ Check before proceeding

// Delay before second connection
for (int i = 0; i < 10; i++) {
    network.AdvanceTime(time_ms += 100);
}

// Connect second peer
noban_peer.ConnectTo(1);
for (int i = 0; i < 20; i++) {
    network.AdvanceTime(time_ms += 100);
}

REQUIRE(victim.GetPeerCount() == 2);  // ✅ Both connected successfully
```

**Why:** VERSION/VERACK handshake messages can arrive out of order if connections overlap. Always verify first connection before starting second.

---

## Best Practices

### 1. ✅ Give Ample Time for Async Operations

```cpp
// After connection
attacker.ConnectTo(1);
for (int i = 0; i < 15; i++) {  // At least 10-20 iterations
    time_ms += 100;
    network.AdvanceTime(time_ms);
}

// After attack
attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 1);
for (int i = 0; i < 20; i++) {  // Give time for validation & disconnect
    time_ms += 100;
    network.AdvanceTime(time_ms);
}
```

**Why:** Even though time is simulated, async operations need multiple event processing cycles to complete.

---

### 2. ✅ Check Intermediate State

```cpp
// Verify peer connected
REQUIRE(victim.GetPeerCount() == 1);

// Send attack
attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 1);
network.AdvanceTime(time_ms += 1000);

// Verify peer disconnected
REQUIRE(victim.GetPeerCount() == 0);  // ✅ Explicit state check

// Verify discouraged
REQUIRE(victim.GetBanMan().IsDiscouraged(attacker.GetAddress()));
```

**Why:** If test fails, you know exactly where it failed. Makes debugging much easier.

---

### 3. ✅ Use Consistent Time Increments

```cpp
uint64_t time_ms = 1000000;  // Start with reasonable base time

// Mining
for (int i = 0; i < 5; i++) {
    victim.MineBlock();
    time_ms += 50;  // Small increment for fast operations
    network.AdvanceTime(time_ms);
}

// Network operations
for (int i = 0; i < 10; i++) {
    time_ms += 100;  // Larger increment for network round-trips
    network.AdvanceTime(time_ms);
}
```

**Why:** Consistent increments make tests more readable and maintainable.

---

### 4. ✅ Test One Thing at a Time

```cpp
TEST_CASE("NoBan peer survives invalid PoW", "[network][permissions]") {
    SECTION("Normal peer gets discouraged") {
        // Test normal peer behavior
    }

    SECTION("NoBan peer survives") {
        // Test NoBan peer behavior
    }
}
```

**Why:** Sections share setup but test different scenarios. Easier to understand and debug.

---

## Debugging Tips

### 1. Enable Verbose Logging

```bash
# Info level (default)
./coinbasechain_tests "[network][permissions]"

# Warning level (less noise)
SPDLOG_LEVEL=warn ./coinbasechain_tests "[network][permissions]"

# Trace level (all details)
SPDLOG_LEVEL=trace ./coinbasechain_tests "[network][permissions]"
```

### 2. Run Single Test with -s Flag

```bash
# Show stdout/stderr
./coinbasechain_tests "NoBan peer survives invalid PoW attack" -s
```

### 3. Grep for Specific Events

```bash
# Check if PoW validation failing
./coinbasechain_tests "test name" -s 2>&1 | grep -E "(CheckProofOfWork|Headers failed PoW)"

# Check peer connection state
./coinbasechain_tests "test name" -s 2>&1 | grep -E "(VERSION|VERACK|connected|disconnected)"

# Check misbehavior scoring
./coinbasechain_tests "test name" -s 2>&1 | grep -E "(Misbehaving|marked for disconnect|score)"
```

### 4. Add Debug Prints

```cpp
// Temporary debugging
printf("DEBUG: victim peer count = %zu\n", victim.GetPeerCount());
printf("DEBUG: attacker node_id = %d\n", attacker.GetId());

auto peers = victim.GetNetworkManager().peer_manager().get_all_peers();
for (const auto& peer : peers) {
    printf("DEBUG: peer_id=%d, address=%s\n", peer->id(), peer->address().c_str());
}
```

---

## Example Patterns

### Pattern 1: Basic DoS Test

```cpp
TEST_CASE("Peer gets discouraged for invalid PoW", "[network][dos]") {
    SimulatedNetwork network(12345);
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(100, &network);

    uint64_t time_ms = 1000000;

    // Step 1: Victim mines chain (with bypass)
    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
        time_ms += 50;
        network.AdvanceTime(time_ms);
    }

    // Step 2: Attacker connects
    REQUIRE(attacker.ConnectTo(1));
    for (int i = 0; i < 15; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }
    REQUIRE(victim.GetPeerCount() == 1);

    // Step 3: Disable bypass (AFTER sync, BEFORE attack)
    victim.SetBypassPOWValidation(false);

    // Step 4: Send attack
    attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 1);
    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Step 5: Verify protection worked
    REQUIRE(victim.GetPeerCount() == 0);
    REQUIRE(victim.GetBanMan().IsDiscouraged(attacker.GetAddress()));
}
```

### Pattern 2: Permission Test

```cpp
TEST_CASE("NoBan peer survives attack", "[network][permissions]") {
    SimulatedNetwork network(12345);
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(100, &network);

    uint64_t time_ms = 1000000;

    // Setup: Mine chain
    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
        time_ms += 50;
        network.AdvanceTime(time_ms);
    }

    // Configure NoBan permission
    victim.SetInboundPermissions(NetPermissionFlags::NoBan);

    // Connect peer (will get NoBan permission)
    REQUIRE(attacker.ConnectTo(1));
    for (int i = 0; i < 15; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }
    REQUIRE(victim.GetPeerCount() == 1);

    // Disable bypass
    victim.SetBypassPOWValidation(false);

    // Attack
    attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 1);
    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Verify NoBan peer STAYS connected
    REQUIRE(victim.GetPeerCount() == 1);
    REQUIRE_FALSE(victim.GetBanMan().IsDiscouraged(attacker.GetAddress()));

    // Verify score still tracked
    auto& peer_manager = victim.GetNetworkManager().peer_manager();
    auto peers = peer_manager.get_all_peers();
    REQUIRE(peers.size() == 1);
    int peer_id = peers[0]->id();  // ✅ Get actual peer_id
    int score = peer_manager.GetMisbehaviorScore(peer_id);
    REQUIRE(score >= 100);
}
```

### Pattern 3: Multi-Peer Test

```cpp
TEST_CASE("Score tracking for multiple peers", "[network][permissions]") {
    SimulatedNetwork network(12345);
    SimulatedNode victim(1, &network);
    AttackSimulatedNode normal_attacker(100, &network);
    AttackSimulatedNode noban_attacker(101, &network);

    uint64_t time_ms = 1000000;

    // Mine chain
    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
        time_ms += 50;
        network.AdvanceTime(time_ms);
    }

    // Connect first peer (normal)
    REQUIRE(normal_attacker.ConnectTo(1));
    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }
    REQUIRE(victim.GetPeerCount() == 1);  // ✅ Verify before second connection

    // Configure NoBan for next connection
    victim.SetInboundPermissions(NetPermissionFlags::NoBan);

    // Delay before second connection (avoid handshake race)
    for (int i = 0; i < 10; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Connect second peer (NoBan)
    REQUIRE(noban_attacker.ConnectTo(1));
    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }
    REQUIRE(victim.GetPeerCount() == 2);

    // Disable bypass
    victim.SetBypassPOWValidation(false);

    // Both attack
    normal_attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 1);
    noban_attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 1);
    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Verify: normal disconnected, NoBan stays
    REQUIRE(victim.GetPeerCount() == 1);
    REQUIRE(victim.GetBanMan().IsDiscouraged(normal_attacker.GetAddress()));
    REQUIRE_FALSE(victim.GetBanMan().IsDiscouraged(noban_attacker.GetAddress()));

    // Verify NoBan peer score tracked
    auto& peer_manager = victim.GetNetworkManager().peer_manager();
    auto peers = peer_manager.get_all_peers();
    REQUIRE(peers.size() == 1);  // Only NoBan peer remains
    int peer_id = peers[0]->id();
    int score = peer_manager.GetMisbehaviorScore(peer_id);
    REQUIRE(score >= 100);
}
```

---

## Summary

**Remember the Golden Rules:**

1. ✅ Use `peer_manager.get_all_peers()[i]->id()` to get peer_id, never use node_id
2. ✅ Check `IsDiscouraged()` for DoS protection, not `IsBanned()`
3. ✅ Disable PoW bypass AFTER sync, BEFORE attacks
4. ✅ Verify first peer connected before starting second connection
5. ✅ Give ample time (15-20 iterations) for async operations
6. ✅ Check intermediate state to make debugging easier

**When Tests Fail:**

1. Run with `-s` flag to see output
2. Use `SPDLOG_LEVEL=info` or `trace` for more detail
3. Grep for specific events (PoW, connections, misbehavior)
4. Add temporary debug prints
5. Check the patterns in this guide

**When Writing New Tests:**

1. Copy an existing pattern that's similar
2. Follow the step-by-step structure (setup → connect → attack → verify)
3. Add comments explaining each step
4. Test with and without `-s` flag to ensure clean output

---

## Questions?

If you encounter issues not covered in this guide, please:
1. Document the problem and solution
2. Update this guide with a new section
3. Help future developers avoid the same pitfall!
