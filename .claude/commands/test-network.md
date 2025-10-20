---
description: Write or fix networking tests with Quick Reference guide
---

You are writing or fixing networking tests for the CoinbaseChain project.

## CRITICAL RULES - READ FIRST

**RULE 1: node_id ≠ peer_id**
- `node_id`: Assigned in SimulatedNode constructor (e.g., 100, 101, 1, 2)
- `peer_id`: Assigned by PeerManager when connection is established (starts at 0)
- **Action:** NEVER use `GetId()` (returns node_id) to query PeerManager
- **Correct pattern:**
  ```cpp
  auto peers = peer_manager.get_all_peers();
  REQUIRE(peers.size() == 1);
  int peer_id = peers[0]->id();  // ✅ Get actual peer_id
  int score = peer_manager.GetMisbehaviorScore(peer_id);
  ```

**RULE 2: Discourage ≠ Ban**
- `Ban`: Permanent ban stored in `m_banned` map, checked by `IsBanned()`
- `Discourage`: Temporary 24h ban stored in `m_discouraged` map, checked by `IsDiscouraged()`
- **Action:** DoS protection uses Discourage, so check `IsDiscouraged()` not `IsBanned()`
- **Correct pattern:**
  ```cpp
  REQUIRE(victim.GetBanMan().IsDiscouraged(attacker.GetAddress()));  // ✅
  REQUIRE_FALSE(victim.IsBanned(attacker.GetAddress()));  // ❌ Wrong map
  ```

**RULE 3: PoW Bypass Timing**
- Enable bypass during: mining blocks, initial sync
- Disable bypass before: validating attack messages
- **Never disable before:** peer connection and handshake completion
- **Correct pattern:**
  ```cpp
  // 1. Mine blocks WITH bypass (fast)
  for (int i = 0; i < 5; i++) {
      victim.MineBlock();
  }

  // 2. Connect peers and sync (still with bypass)
  REQUIRE(attacker.ConnectTo(1));
  for (int i = 0; i < 10; i++) {
      network.AdvanceTime(++time_ms);
  }

  // 3. NOW disable bypass for attack validation
  victim.SetBypassPOWValidation(false);

  // 4. Send attack messages
  attacker.SendInvalidPoWHeaders(...);
  ```

**RULE 4: Handshake Race Conditions**
- When connecting multiple peers, VERSION/VERACK messages can arrive out of order
- **Action:** Verify first peer fully connected before starting second connection
- **Correct pattern:**
  ```cpp
  // First peer connects
  REQUIRE(peer1.ConnectTo(victim_id));

  // Wait for handshake to complete
  for (int i = 0; i < 20; i++) {
      network.AdvanceTime(++time_ms);
  }

  // VERIFY first peer is fully connected
  REQUIRE(victim.GetPeerCount() == 1);  // ✅ Critical check

  // Add delay before second connection
  for (int i = 0; i < 10; i++) {
      network.AdvanceTime(++time_ms);
  }

  // Now safe to connect second peer
  REQUIRE(peer2.ConnectTo(victim_id));
  ```

**RULE 5: Processing Events**
- After any network operation, call `network.AdvanceTime()` to process async events
- Use at least 10-20 iterations for complex operations (handshakes, disconnects)
- **Action:** Always advance time after: connections, disconnections, sending messages

## Error Lookup Table

| Symptom / Error Message | Root Cause | Fix |
|------------------------|------------|-----|
| `REQUIRE(score >= 100)` fails with `0 >= 100` | Using node_id instead of peer_id | Use `peer_manager.get_all_peers()[0]->id()` to get peer_id |
| `REQUIRE(IsBanned(...))` fails | Checking wrong ban list | Use `GetBanMan().IsDiscouraged()` instead of `IsBanned()` |
| `Received verack before VERSION` | Handshake race condition | Verify first peer connected (`GetPeerCount() == 1`) before starting second connection |
| Peer doesn't disconnect during attack | PoW bypass still enabled | Disable bypass AFTER sync, BEFORE attack: `victim.SetBypassPOWValidation(false)` |
| Initial sync fails after disabling bypass | Bypass disabled too early | Disable bypass AFTER peers sync, not before connection |
| Test hangs or times out | Not processing async events | Add `network.AdvanceTime()` loops after network operations |
| Misbehavior score not tracked for NoBan | Score tracking works (expected) | NoBan peers get scores tracked, just not disconnected |

## Code Pattern Recognition

**IF you see this pattern (❌ WRONG):**
```cpp
int score = peer_manager.GetMisbehaviorScore(attacker.GetId());
```
**REPLACE WITH (✅ CORRECT):**
```cpp
auto peers = peer_manager.get_all_peers();
REQUIRE(peers.size() == 1);
int peer_id = peers[0]->id();
int score = peer_manager.GetMisbehaviorScore(peer_id);
```

**IF you see this pattern (❌ WRONG):**
```cpp
REQUIRE(victim.IsBanned(attacker.GetAddress()));
```
**REPLACE WITH (✅ CORRECT):**
```cpp
REQUIRE(victim.GetBanMan().IsDiscouraged(attacker.GetAddress()));
```

**IF you see this pattern (❌ WRONG):**
```cpp
victim.SetBypassPOWValidation(false);  // Before connection
REQUIRE(attacker.ConnectTo(1));
```
**REPLACE WITH (✅ CORRECT):**
```cpp
REQUIRE(attacker.ConnectTo(1));
// Complete handshake and sync
for (int i = 0; i < 10; i++) {
    network.AdvanceTime(++time_ms);
}
REQUIRE(victim.GetPeerCount() == 1);
// NOW disable bypass
victim.SetBypassPOWValidation(false);
```

## Full Documentation

For complete testing guide with examples, see:
- **[test/network/TESTING_GUIDE.md](test/network/TESTING_GUIDE.md)** - Comprehensive guide with test templates and debugging tips

## Your Task

Based on the user's request, write or fix network tests following these rules. Always:
1. Check for node_id vs peer_id issues
2. Use IsDiscouraged() not IsBanned()
3. Set PoW bypass timing correctly (mine with bypass, validate without)
4. Avoid handshake race conditions
5. Process events with AdvanceTime() loops

Proceed with the user's specific request while adhering to these patterns.
