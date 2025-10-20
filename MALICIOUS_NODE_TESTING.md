# Malicious Node Testing - How It Works

## The Problem

**You CANNOT use functional tests for malicious behavior** because:
- Functional tests spawn real `./bin/coinbasechain` binaries
- Those binaries implement the honest protocol
- You can't make them send invalid messages or attack

```python
# ❌ Can't do this in functional tests
node1 = spawn_real_node()
node1.send_invalid_pow_headers()  # Real binary won't do this!
```

## The Solution: Attack Node in C++ Network Tests

You use **C++ network tests** with a special `AttackSimulatedNode` class that can:
- Send invalid messages
- Send orphan headers
- Send non-continuous headers
- Spam oversized messages
- Stall responses
- Do selfish mining
- Violate protocol rules

---

## How It Works

### 1. Network Tests Use Simulated Transport

```cpp
// test/network/attack_simulation_tests.cpp

SimulatedNetwork network;              // In-memory message bus
SimulatedNode victim(1, &network);     // Honest node
AttackSimulatedNode attacker(2, &network);  // Malicious node

// Attacker connects to victim
attacker.ConnectTo(1);

// Attacker sends malicious messages
attacker.SendOrphanHeaders(1, 1000);   // Spam orphan headers
attacker.SendInvalidPoWHeaders(1, prev_hash, 100);  // Invalid PoW
```

### 2. AttackSimulatedNode Can Violate Protocol

The `AttackSimulatedNode` class extends `SimulatedNode` with malicious capabilities:

```cpp
class AttackSimulatedNode : public SimulatedNode {
public:
    // Send orphan headers (headers with unknown parents)
    void SendOrphanHeaders(int peer_node_id, size_t count);
    
    // Send headers with invalid PoW
    void SendInvalidPoWHeaders(int peer_node_id, const uint256& prev_hash, size_t count);
    
    // Send non-continuous headers (don't connect properly)
    void SendNonContinuousHeaders(int peer_node_id, const uint256& prev_hash);
    
    // Send oversized HEADERS message (>2000 headers)
    void SendOversizedHeaders(int peer_node_id, size_t count);
    
    // Enable stalling mode - don't respond to GETHEADERS
    void EnableStalling(bool enabled);
    
    // Mine a block privately (selfish mining)
    uint256 MineBlockPrivate(const std::string& miner_address);
    
    // Broadcast private block only to specific peer
    void BroadcastBlock(const uint256& block_hash, int peer_node_id);
};
```

---

## Example Attack Tests

### Test 1: Orphan Spam Attack

**Attack:** Flood victim with headers that have unknown parents (orphans)  
**Goal:** Exhaust victim's memory or CPU  
**Defense:** Orphan cache limits + misbehavior scoring

```cpp
TEST_CASE("OrphanSpamAttack", "[attacktest]") {
    SimulatedNetwork network;
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);
    
    // Victim has normal chain
    victim.MineBlock();  // Height 1
    victim.MineBlock();  // Height 2
    
    // Attacker connects
    attacker.ConnectTo(1);
    network.AdvanceTime(100);
    
    // ATTACK: Send 1000 orphan headers (unknown parents)
    attacker.SendOrphanHeaders(1, 1000);
    network.AdvanceTime(100);
    
    // VERIFY DEFENSE:
    CHECK(victim.GetTipHeight() == 2);  // Still functional
    CHECK(victim.IsBanned(attacker.GetAddress()));  // Attacker banned
}
```

### Test 2: Invalid PoW Attack

**Attack:** Send headers with invalid proof-of-work  
**Goal:** Make victim accept invalid blocks  
**Defense:** Reject invalid PoW + ban peer

```cpp
TEST_CASE("InvalidPoWAttack", "[attacktest]") {
    SimulatedNetwork network;
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);
    
    victim.MineBlock();
    attacker.ConnectTo(1);
    network.AdvanceTime(100);
    
    uint256 prev_hash = victim.GetTipHash();
    
    // ATTACK: Send headers with invalid PoW
    attacker.SendInvalidPoWHeaders(1, prev_hash, 10);
    network.AdvanceTime(100);
    
    // VERIFY DEFENSE:
    CHECK(victim.GetTipHeight() == 1);  // Didn't accept invalid blocks
    // Attacker should get high misbehavior score
}
```

### Test 3: Selfish Mining Attack

**Attack:** Mine blocks privately, then release strategically  
**Goal:** Waste other miners' work and get unfair advantage  
**Defense:** Honest nodes mine on longest valid chain

```cpp
TEST_CASE("SelfishMiningAttack", "[attacktest]") {
    SimulatedNetwork network;
    SimulatedNode honest(1, &network);
    AttackSimulatedNode selfish(2, &network);
    
    // Selfish miner finds a block but doesn't broadcast
    uint256 private_block = selfish.MineBlockPrivate();
    
    // Honest miner mines on old tip
    honest.MineBlock();
    
    // Selfish miner sees honest block, quickly broadcasts private block
    selfish.BroadcastBlock(private_block, 1);
    network.AdvanceTime(100);
    
    // Now both nodes should reorganize to selfish chain if it's longer
    // (This models the race condition in selfish mining)
}
```

### Test 4: Stalling Attack

**Attack:** Don't respond to GETHEADERS requests  
**Goal:** Prevent victim from syncing  
**Defense:** Timeout stalled peers + try other peers

```cpp
TEST_CASE("StallingAttack", "[attacktest]") {
    SimulatedNetwork network;
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);
    
    // Attacker has longer chain
    for (int i = 0; i < 10; i++) {
        attacker.MineBlock();
    }
    
    // Enable stalling mode
    attacker.EnableStalling(true);
    
    // Victim connects and tries to sync
    victim.ConnectTo(2);
    network.AdvanceTime(100);
    
    // Victim should timeout the stalled peer
    for (int i = 0; i < 100; i++) {
        network.AdvanceTime(100);
    }
    
    // VERIFY DEFENSE:
    CHECK(victim.GetPeerCount() == 0);  // Disconnected stalled peer
}
```

---

## Why This Approach Works

### ✅ Advantages of C++ Attack Tests

1. **Full control** - Can send any message, valid or invalid
2. **Deterministic** - Simulated network has no timing variance
3. **Fast** - No process spawning, runs in milliseconds
4. **Debuggable** - Can step through attack in debugger
5. **Comprehensive** - Can test attacks impossible in real network

### ❌ What You Can't Test

- Real binary behavior under attack (that's what functional tests do)
- Real network timing effects
- Real resource exhaustion (memory, CPU, network bandwidth)
- Cross-process attack scenarios

---

## Current Attack Test Coverage

```bash
test/network/attack_simulation_tests.cpp:
  ✓ OrphanSpamAttack
  ✓ OrphanChainGrinding
  ✓ InvalidPoWAttack
  ✓ OversizedHeadersAttack
  ✓ NonContinuousHeadersAttack
  ✓ SelfishMiningBasic

test/network/peer_adversarial_tests.cpp:
  ✓ MaliciousHandshake
  ✓ DoubleSendVersionMessage
  ✓ UnsolicitedPong
  ✓ InvalidMessageType
  ✓ OversizedMessage

test/network/misbehavior_penalty_tests.cpp:
  ✓ InvalidPoWPenalty (100 points = ban)
  ✓ OrphanHeaderPenalty (10 points per orphan)
  ✓ DuplicateHeaderPenalty
  ✓ MisbehaviorAccumulation
  ✓ BanThreshold
```

---

## Alternative: Python Attack Tool (Advanced)

For **functional-level attack testing**, you'd need a custom attack tool:

```python
# tools/attack_node/attack_node.py (hypothetical)

class AttackNode:
    """Malicious node that speaks P2P protocol but can violate rules"""
    
    def __init__(self, target_host, target_port):
        self.socket = socket.socket()
        self.socket.connect((target_host, target_port))
    
    def send_invalid_pow_header(self):
        # Craft a header with invalid PoW
        header = create_block_header(nBits=0x1d00ffff)
        header.nonce = 0  # Won't meet target
        msg = serialize_message("headers", [header])
        self.socket.send(msg)
    
    def send_orphan_spam(self, count):
        # Send many headers with unknown parents
        for i in range(count):
            header = create_orphan_header()
            msg = serialize_message("headers", [header])
            self.socket.send(msg)
```

**Problem:** This requires implementing P2P protocol serialization in Python - **very complex!**

---

## Best Practice: Layered Testing

```
┌─────────────────────────────────────────────┐
│ Python Attack Tool (future)                 │  ← Real binary under attack
│ - Test real node processes                  │
│ - Real network behavior                     │
└─────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────┐
│ C++ Attack Tests (current) ✓               │  ← Simulated attacks
│ - AttackSimulatedNode                       │
│ - Comprehensive attack coverage             │
└─────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────┐
│ C++ Unit Tests                              │  ← Individual defenses
│ - Test misbehavior scoring logic            │
│ - Test ban thresholds                       │
└─────────────────────────────────────────────┘
```

---

## Summary

**Question:** How do we test malicious nodes?

**Answer:**

1. ❌ **NOT functional tests** - They spawn honest binaries
2. ✅ **USE C++ network tests** - With `AttackSimulatedNode`
3. ✅ **Simulated network** - Allows protocol violations
4. ✅ **Fast and deterministic** - Test many attack scenarios
5. ⚠️ **Future:** Python attack tool for real binary testing

**Your current approach is correct and comprehensive!** You have:
- `AttackSimulatedNode` class for attacks
- Tests for orphan spam, invalid PoW, selfish mining, stalling
- Misbehavior penalty tests
- DoS protection verification

The C++ network tests give you **excellent coverage** of attack scenarios without needing real malicious binaries.
