# Next Adversarial Testing Target - Analysis

## Current Status

✅ **Peer Protocol**: 26 adversarial tests, 103 assertions, 100% coverage

---

## Candidate Components for Adversarial Testing

### 1. 🎯 **Header Sync (src/sync/header_sync.cpp)** - TOP PRIORITY

**Why This Is Critical**:
- Header sync is the **most vulnerable** part of the system
- Handles untrusted data from remote peers
- Direct attack vector for consensus manipulation
- Bitcoin Core has extensive DoS protections here

**Attack Vectors**:
- Invalid PoW headers (fake difficulty)
- Headers not chaining correctly (wrong prevhash)
- Timestamp manipulation (too far future/past)
- Fork bombing (thousands of side chains)
- Checkpoint bypass attempts
- Slow header DoS (send 1 header at a time)
- Header spam (flood with headers on same chain)
- Nonce overflow headers (timestamp + nonce manipulation)

**Existing Tests**: `test/header_sync_tests.cpp` (likely functional only)

**Priority**: 🔴 **HIGHEST** - This is where most blockchain attacks happen

**Estimated Effort**: 4-6 hours (complex attack scenarios)

---

### 2. 🎯 **Network Manager (src/network/network_manager.cpp)** - HIGH PRIORITY

**Why Important**:
- Coordinates all peer interactions
- Handles INV/GETHEADERS/HEADERS flooding
- Manages peer selection for sync
- Connection management (inbound/outbound limits)

**Attack Vectors**:
- Peer starvation (disconnect all peers repeatedly)
- Sync peer hijacking (malicious peer becomes sync peer)
- INV flooding (thousands of fake block announcements)
- GETHEADERS spam
- Connection slot exhaustion
- Rapid connect/disconnect cycling
- Multiple peers claiming same blocks

**Existing Tests**: `test/network/network_tests.cpp`

**Priority**: 🟡 **HIGH** - Critical but less severe than header sync

**Estimated Effort**: 3-4 hours

---

### 3. 🎯 **Address Manager (src/network/addr_manager.cpp)** - MEDIUM PRIORITY

**Why Important**:
- Manages peer discovery
- Could be poisoned with fake addresses
- Eclipse attack vector (surround node with attacker IPs)

**Attack Vectors**:
- Address flooding (millions of fake IPs)
- Eclipse attack (only attacker addresses)
- Timestamp manipulation (make old addresses seem new)
- Self-address injection
- Duplicate address flooding
- Address ban evasion

**Existing Tests**: `test/addr_manager_tests.cpp`

**Priority**: 🟡 **MEDIUM** - Important for network health

**Estimated Effort**: 2-3 hours

---

### 4. **Peer Manager (src/network/peer_manager.cpp)** - MEDIUM PRIORITY

**Why Important**:
- Tracks peer statistics
- Manages peer limits (inbound/outbound)
- Peer selection logic

**Attack Vectors**:
- Exceed peer limits
- Peer stat manipulation
- Rapid peer churn

**Existing Tests**: Unknown

**Priority**: 🟢 **MEDIUM** - Less critical, peer.cpp is already tested

**Estimated Effort**: 2-3 hours

---

### 5. **Ban Manager (src/sync/banman.cpp)** - LOW PRIORITY

**Why Important**:
- Prevents repeated connections from misbehaving peers
- Could be evaded or abused

**Attack Vectors**:
- Ban evasion (changing IPs)
- Ban list exhaustion
- False positive bans

**Existing Tests**: Unknown

**Priority**: 🟢 **LOW** - Nice to have

**Estimated Effort**: 1-2 hours

---

## 🎯 Recommended Order

Based on **attack surface** and **severity**:

### Phase 1: Critical Security (Must Have)
1. **Header Sync** (4-6 hours) 🔴
   - Invalid PoW, chain validation, fork attacks
   - This is where most consensus attacks happen

### Phase 2: Network DoS (Should Have)
2. **Network Manager** (3-4 hours) 🟡
   - Peer coordination attacks, flooding, starvation
   - INV/GETHEADERS/HEADERS abuse

3. **Address Manager** (2-3 hours) 🟡
   - Eclipse attacks, address poisoning
   - Critical for network topology security

### Phase 3: Supporting Components (Nice to Have)
4. **Peer Manager** (2-3 hours) 🟢
   - Peer limit evasion, stat manipulation

5. **Ban Manager** (1-2 hours) 🟢
   - Ban evasion, list exhaustion

---

## 📊 Comparison Matrix

| Component | Attack Surface | Severity | Existing Tests | Priority | Effort |
|-----------|----------------|----------|----------------|----------|--------|
| **Header Sync** | Very High | Critical | Functional only | 🔴 Highest | 4-6h |
| **Network Manager** | High | High | Some tests | 🟡 High | 3-4h |
| **Address Manager** | Medium | High | Basic tests | 🟡 Medium | 2-3h |
| **Peer Manager** | Medium | Medium | Unknown | 🟢 Medium | 2-3h |
| **Ban Manager** | Low | Low | Unknown | 🟢 Low | 1-2h |

---

## 🎯 Recommendation: Start with Header Sync

**Why Header Sync First**:

1. **Highest Risk**: Header sync directly affects consensus
2. **Most Complex**: Needs PoW validation, chain validation, fork handling
3. **Bitcoin Core Priority**: Bitcoin has extensive header sync DoS protections
4. **Direct Attack Vector**: Malicious peers send fake headers

**What Bitcoin Core Tests**:
- Invalid PoW headers → Reject
- Headers not chaining → Disconnect peer
- Timestamp too far future → Reject (2 hours max)
- Timestamp too far past → Reject (below median of last 11)
- Fork bombing → Rate limit branches
- Slow drip headers → Timeout
- Duplicate headers → Ignore
- Checkpoint bypass → Reject

**Your Header Sync Tests Needed**:
1. Invalid PoW (header with wrong RandomX hash)
2. Invalid chain (prevhash doesn't match)
3. Timestamp attacks (future, past, manipulation)
4. Fork bombing (send hundreds of competing branches)
5. Slow header DoS (1 header per minute)
6. Header spam (same headers repeatedly)
7. Checkpoint violations
8. Excessive headers (> 2000 in one batch)

---

## 🚀 Quick Start: Header Sync Adversarial Tests

### File Structure:
```
test/network/header_sync_adversarial_tests.cpp
```

### Test Categories:
1. **Invalid PoW Attacks** (3-4 tests)
   - Wrong RandomX hash
   - Difficulty too low
   - Nonce manipulation

2. **Chain Validation Attacks** (3-4 tests)
   - Invalid prevhash
   - Height manipulation
   - Reorg to invalid chain

3. **Timestamp Attacks** (3-4 tests)
   - Too far future (> 2 hours)
   - Too far past (below median)
   - Timestamp going backwards

4. **Fork Attacks** (2-3 tests)
   - Fork bombing (hundreds of branches)
   - Deep reorg attempts
   - Competing tips

5. **DoS Attacks** (3-4 tests)
   - Slow header drip
   - Duplicate header spam
   - Excessive headers (> 2000)
   - Empty HEADERS messages

6. **Checkpoint Attacks** (2-3 tests)
   - Try to reorg before checkpoint
   - Invalid checkpoint headers

**Total**: ~20 tests, similar to peer adversarial testing

---

## 📝 Example Test Structure

```cpp
TEST_CASE("HeaderSync - InvalidPoW", "[adversarial][header_sync][critical]") {
    // Create header with invalid RandomX hash
    // Send to header sync
    // Expected: Reject header, disconnect peer, mark as misbehaving
}

TEST_CASE("HeaderSync - ForkBombing", "[adversarial][header_sync][dos]") {
    // Send 100 competing branches (same height, different hashes)
    // Expected: Rate limit accepted, excessive forks rejected
}

TEST_CASE("HeaderSync - TimestampTooFarFuture", "[adversarial][header_sync][critical]") {
    // Send header with timestamp = now + 3 hours (> 2 hour limit)
    // Expected: Reject, disconnect peer
}
```

---

## ✅ My Recommendation

**Start with Header Sync adversarial tests**

**Reasons**:
1. Highest security impact
2. Most complex attack surface
3. Directly protects consensus
4. Natural progression from peer testing (peer receives headers → header sync validates)

**Timeline**:
- Planning & test design: 1 hour
- Implementation: 3-4 hours
- Testing & documentation: 1-2 hours
- **Total**: 5-7 hours

**ROI**: Very high - protects most critical attack vector

---

Would you like me to start implementing Header Sync adversarial tests?
