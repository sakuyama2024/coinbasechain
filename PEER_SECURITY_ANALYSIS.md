# Peer Security Test Coverage Analysis

## Current Coverage ✅

### Unit Tests (peer_tests.cpp):
1. **Self-connection prevention** - Nonce-based detection
2. **Buffer flood protection** - 5MB receive buffer limit
3. **User agent validation** - 256-byte limit
4. **Invalid magic bytes** - Disconnects on wrong network magic
5. **Oversized messages** - Rejects messages > 4MB
6. **Checksum validation** - Disconnects on checksum mismatch
7. **Handshake timeout** - 60 second limit
8. **Inactivity timeout** - 20 minute limit

### Functional Tests (network_tests.cpp):
- Orphan spam attacks
- Invalid PoW attacks
- Oversized header messages
- Non-continuous headers
- Coordinated orphan storms
- Selfish mining
- Reorg spam
- Header flooding

## MISSING Security Tests (Peer-Level) ⚠️

### 1. **Protocol State Machine Vulnerabilities**

#### A. Out-of-Order Message Attacks
**Risk**: Attacker sends messages before handshake complete
- Send PING before VERACK
- Send HEADERS before VERSION/VERACK
- Send PONG without corresponding PING
- Multiple VERSION messages

**Test needed**: Verify peer disconnects on protocol violations

#### B. Duplicate Message Attacks
**Risk**: Peer sends same message multiple times
- Duplicate VERSION (could trigger double nonce check)
- Duplicate VERACK
- Rapid-fire PING flooding

**Test needed**: Rate limiting and duplicate detection

---

### 2. **Message Parsing Edge Cases**

#### A. Malformed Message Attacks
**Risk**: Crafted messages trigger parsing errors
- Header with length=0 but has payload
- Header claims length X, but payload is Y
- Payload shorter than expected for message type
- Empty command field (all nulls)
- Command field with non-printable characters

**Test needed**: Fuzz testing message parsing

#### B. Partial Message DoS
**Risk**: Attacker sends partial messages to fill receive buffer
- Send header only, never send payload
- Send 90% of message, stop, send another partial
- Fill buffer with 5MB of incomplete messages

**Test needed**: Verify buffer management and timeout

---

### 3. **Timing Attacks**

#### A. Slowloris-style Connection Holding
**Risk**: Attacker opens connections but sends data very slowly
- Complete handshake but send messages 1 byte per minute
- Keep connection alive with minimal traffic

**Test needed**: Inactivity detection with partial message timeout

#### B. Ping Timeout Evasion
**Risk**: Attacker responds to PING just before timeout (19m 59s)
- Keeps connection alive indefinitely with minimum traffic

**Test needed**: Already covered (PING_TIMEOUT_SEC = 20 min)

---

### 4. **Resource Exhaustion**

#### A. Receive Buffer Manipulation
**Risk**: Fill buffer just below limit repeatedly
- Send 4.9MB message, process, send another 4.9MB
- Test if buffer fragmentation causes issues

**Test needed**: Sustained high-volume traffic

#### B. Message Handler Blocking
**Risk**: Message handler takes too long, blocks message processing
- Handler does expensive computation
- Handler waits on I/O

**Test needed**: Handler timeout or async processing

---

### 5. **Time-Based Attacks**

#### A. VERSION Timestamp Manipulation
**Risk**: Peer sends extreme timestamps
- Timestamp = 0 (January 1970)
- Timestamp = MAX_INT64 (year 2038+)
- Timestamp far in future/past

**Test needed**: Verify timedata.cpp handles extreme values
**Note**: Already handled by AddTimeData, but should verify

#### B. Network Time Poisoning
**Risk**: Multiple malicious peers skew time
- 100 peers all report time +2 hours
- Affects block timestamp validation

**Test needed**: Integration test (not peer-level)

---

### 6. **Nonce-Related Attacks**

#### A. Nonce Collision Attack
**Risk**: Attacker brute-forces nonce to match ours
- Birthday paradox: 2^64 nonces, collision at ~2^32
- If successful, forces self-connection disconnect

**Test needed**: Verify nonce randomness quality
**Note**: Uses std::mt19937_64, should be fine

#### B. PONG Nonce Mismatch
**Risk**: Peer responds with wrong PONG nonce
- Ignores our PING, sends random PONG
- Never clears last_ping_nonce_, triggers timeout

**Test needed**: Verify we ignore wrong-nonce PONGs ✅ (already implemented)

---

### 7. **Deserialization Attacks**

#### A. Unknown Message Types
**Risk**: Peer sends unrecognized command
- Current code: logs warning, continues
- Potential: attacker floods with unknown messages

**Test needed**: Rate limit unknown messages ⚠️

#### B. Deserialization Failure Handling
**Risk**: Message deserialize() returns false
- Already disconnects (line 329-332)
- But: no misbehavior tracking

**Test needed**: Verify disconnect, consider misbehavior score

---

### 8. **Connection State Races**

#### A. Rapid Connect/Disconnect
**Risk**: Attacker rapidly connects and disconnects
- Open 1000 connections per second
- Test connection slot exhaustion

**Test needed**: Connection rate limiting (PeerManager level)

#### B. Disconnect During Message Processing
**Risk**: Disconnect callback fires while processing message
- Already handled by state checks
- But: verify no use-after-free

**Test needed**: Concurrent disconnect stress test

---

### 9. **Statistics Counter Overflow**

#### A. Counter Wraparound
**Risk**: uint64_t counters overflow after 2^64 messages
- bytes_sent, bytes_received, messages_sent, messages_received
- Unlikely but possible on long-running nodes

**Test needed**: Counter overflow behavior (low priority)

---

### 10. **CRITICAL: Missing Tests**

#### A. **Multiple VERSION Messages** 🔴
**Risk**: HIGH - Peer sends VERSION twice
```cpp
// Currently: handle_version() doesn't check if already received
// Attack: Send VERSION twice to trigger double AddTimeData()
```

**Test needed**: MUST ADD - Reject second VERSION

#### B. **VERACK Before VERSION** 🔴
**Risk**: HIGH - Protocol state violation
```cpp
// Currently: handle_verack() doesn't verify VERSION received first
```

**Test needed**: MUST ADD - Verify state machine

#### C. **Unknown Message Flooding** 🟡
**Risk**: MEDIUM - 1000 unknown commands per second
```cpp
// Currently: Just logs warning, doesn't disconnect
```

**Test needed**: SHOULD ADD - Rate limit or disconnect

#### D. **Partial Message Timeout** 🟡
**Risk**: MEDIUM - Send header, never send payload
```cpp
// Currently: Waits indefinitely in buffer
// Only inactivity_timer would eventually disconnect
```

**Test needed**: SHOULD ADD - Partial message timeout

#### E. **Message Rate Limiting** 🟢
**Risk**: LOW - Legitimate use case exists
```cpp
// Currently: No rate limiting per message type
// Could be exploited but also affects honest nodes
```

**Test needed**: NICE TO HAVE - Per-message-type limits

---

## Recommendations Priority

### P0 - Critical (Add Immediately):
1. ✅ Multiple VERSION message rejection
2. ✅ Out-of-order protocol messages (VERACK before VERSION)
3. ✅ Duplicate VERSION/VERACK handling

### P1 - High (Add Soon):
4. Unknown message flooding limits
5. Partial message timeout
6. Malformed message fuzzing

### P2 - Medium (Add Eventually):
7. Slowloris-style connection holding
8. Message handler blocking detection
9. Extreme timestamp handling

### P3 - Low (Nice to Have):
10. Counter overflow tests
11. Nonce collision probability
12. Message rate limiting per type

---

## Test Implementation Plan

### Phase 1: Critical Protocol State Tests
```cpp
TEST_CASE("Peer - DuplicateVersionRejection", "[peer][security][critical]")
TEST_CASE("Peer - VerackBeforeVersion", "[peer][security][critical]")
TEST_CASE("Peer - MultipleVerackRejection", "[peer][security][critical]")
TEST_CASE("Peer - MessageBeforeHandshake", "[peer][security][critical]")
```

### Phase 2: DoS Protection Tests
```cpp
TEST_CASE("Peer - UnknownMessageFlooding", "[peer][security][dos]")
TEST_CASE("Peer - PartialMessageTimeout", "[peer][security][dos]")
TEST_CASE("Peer - SlowDataSendAttack", "[peer][security][dos]")
```

### Phase 3: Edge Case Tests
```cpp
TEST_CASE("Peer - MalformedHeaderLength", "[peer][security]")
TEST_CASE("Peer - EmptyCommandField", "[peer][security]")
TEST_CASE("Peer - ExtremeTimestamps", "[peer][security]")
```
