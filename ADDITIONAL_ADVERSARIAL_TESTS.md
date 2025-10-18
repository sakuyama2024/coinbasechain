# Additional Adversarial Test Ideas

## Tests We Could Add (Prioritized)

---

## 🔴 P1 - High Priority (Should Add Soon)

### 1. Unknown Message Flooding
**Attack**: Flood with unrecognized message types to cause log spam or resource exhaustion

```cpp
TEST_CASE("Adversarial - UnknownMessageFlooding", "[adversarial][flood][dos]") {
    // Complete handshake first
    // Send 1000 messages with unknown commands:
    //   "FAKECMD1", "FAKECMD2", "XYZABC", etc.
    // Currently: Each logs a warning, continues
    // Potential issue: Log spam, CPU from parsing
    // Expected: Should disconnect after N unknown messages?
}
```

**Current behavior**: Just logs warning, keeps processing
**Risk**: Medium - Could spam logs, waste CPU
**Test value**: Helps decide if we need rate limiting

---

### 2. Partial Message Timeout
**Attack**: Send message header, never send payload, hold connection

```cpp
TEST_CASE("Adversarial - PartialMessageHolding", "[adversarial][dos][timeout]") {
    // Send valid header claiming 1MB payload
    // Never send the payload
    // Wait for timeout
    // Currently: Inactivity timer (20 min) eventually disconnects
    // Question: Should we have faster "partial message timeout"?
    // Expected: Disconnect after some timeout
}
```

**Current behavior**: Waits indefinitely (until inactivity timeout)
**Risk**: Medium - Ties up connection slot, wastes memory
**Test value**: Determines if we need partial message timeout

---

### 3. Deserialization Failure Flooding
**Attack**: Send messages that deserialize() returns false

```cpp
TEST_CASE("Adversarial - MalformedPayloadFlooding", "[adversarial][malformed]") {
    // Complete handshake
    // Send 100 PING messages with invalid payloads:
    //   - Payload too short for PING
    //   - Payload has extra garbage bytes
    //   - Payload is completely random
    // Currently: Disconnects on first failure
    // Expected: Verify disconnect happens immediately
}
```

**Current behavior**: Disconnects on deserialization failure (peer.cpp:356-359)
**Risk**: Low - Already protected
**Test value**: Verifies existing protection works

---

### 4. PONG Nonce Mismatch
**Attack**: Respond to PING with wrong nonce to prevent timeout clearing

```cpp
TEST_CASE("Adversarial - PongNonceMismatch", "[adversarial][protocol]") {
    // Complete handshake
    // Wait for peer to send PING (or trigger manually)
    // Respond with PONG but wrong nonce (not matching PING)
    // Verify peer ignores wrong-nonce PONG
    // Verify last_ping_nonce_ is NOT cleared
    // Eventually should timeout (PING_TIMEOUT_SEC)
    // Expected: Wrong nonce ignored, timeout eventually triggers
}
```

**Current behavior**: Checks `if (msg.nonce == last_ping_nonce_)` (peer.cpp:422)
**Risk**: Low - Already handled correctly
**Test value**: Verifies PONG validation works

---

### 5. Receive Buffer Cycling Attack
**Attack**: Fill buffer just below limit repeatedly to test fragmentation

```cpp
TEST_CASE("Adversarial - ReceiveBufferCycling", "[adversarial][resource]") {
    // Complete handshake
    // Send 4.9MB PING message (just below 5MB limit)
    // Wait for processing
    // Send another 4.9MB PING
    // Repeat 10 times
    // Expected: Buffer should handle repeated large messages
    // Test for: Memory leaks, buffer fragmentation issues
}
```

**Current behavior**: Should process each message and clear buffer
**Risk**: Low - But could reveal buffer management issues
**Test value**: Stress tests buffer management

---

## 🟡 P2 - Medium Priority (Nice to Have)

### 6. Message Handler Blocking
**Attack**: Handler takes too long, blocks message processing

```cpp
TEST_CASE("Adversarial - SlowMessageHandler", "[adversarial][resource]") {
    // Set message handler that sleeps for 5 seconds
    // Complete handshake
    // Send VERACK (triggers handler)
    // Send PING while handler is sleeping
    // Test: Can PING be processed concurrently?
    // Or does slow handler block all processing?
    // Expected: Depends on threading model
}
```

**Current behavior**: Handlers called synchronously in `process_message()`
**Risk**: Medium - Slow handler blocks peer message processing
**Test value**: Identifies if we need async handler support

---

### 7. Concurrent Disconnect During Processing
**Attack**: Disconnect while message is being processed

```cpp
TEST_CASE("Adversarial - DisconnectDuringMessageProcessing", "[adversarial][race]") {
    // Complete handshake
    // Set handler that takes time to process
    // Send message (triggers handler)
    // Call disconnect() immediately
    // Test for: Race conditions, use-after-free, crashes
    // Expected: No crash, clean disconnect
}
```

**Current behavior**: State checks should prevent issues
**Risk**: Low - But race conditions are tricky
**Test value**: Verifies thread safety

---

### 8. Statistics Counter Manipulation
**Attack**: Try to overflow statistics counters

```cpp
TEST_CASE("Adversarial - StatisticsOverflow", "[adversarial][resource]") {
    // Set stats to near uint64_t max
    // peer.stats_.messages_sent = UINT64_MAX - 10;
    // peer.stats_.bytes_sent = UINT64_MAX - 1000;
    // Send messages to trigger overflow
    // Expected: Graceful wraparound or saturation
}
```

**Current behavior**: Likely wraps around (uint64_t overflow)
**Risk**: Very Low - Would take decades to naturally overflow
**Test value**: Documents overflow behavior

---

### 9. Network Time Poisoning (Multi-Peer)
**Attack**: Multiple peers send skewed timestamps

```cpp
TEST_CASE("Adversarial - NetworkTimePoisoning", "[adversarial][timing][integration]") {
    // Create 100 inbound peers
    // Each sends VERSION with timestamp = now + 7200 (2 hours future)
    // Test: Does AddTimeData() accept this skew?
    // Expected: timedata.cpp should have median filter protection
    // NOTE: This is integration-level test (multiple peers)
}
```

**Current behavior**: timedata.cpp has protections
**Risk**: Medium - Could affect block timestamp validation
**Test value**: Verifies time poisoning protection (integration test)

---

### 10. Self-Connection Edge Cases
**Attack**: Try to force self-connection in various ways

```cpp
TEST_CASE("Adversarial - SelfConnectionEdgeCases", "[adversarial][protocol]") {
    SECTION("Outbound self-connection") {
        // Outbound peer connects to itself
        // Currently: NetworkManager checks, not Peer
        // Expected: Should be prevented at network level
    }

    SECTION("Nonce collision (birthday paradox)") {
        // Extremely unlikely but possible
        // Two different peers have same nonce
        // Expected: Disconnect, even if not self-connection
    }
}
```

**Current behavior**: Inbound checks nonce, outbound doesn't
**Risk**: Low - NetworkManager prevents this
**Test value**: Documents self-connection prevention

---

## 🟢 P3 - Low Priority (Future)

### 11. Message Rate Limiting Per Type
**Attack**: Flood with specific message type

```cpp
TEST_CASE("Adversarial - PingRateLimiting", "[adversarial][flood]") {
    // Complete handshake
    // Send 1000 PING messages per second
    // Currently: All processed
    // Expected: Should we rate limit?
    // Note: Bitcoin doesn't rate-limit PINGs
}
```

**Current behavior**: No rate limiting per message type
**Risk**: Low - Legitimate use cases exist
**Test value**: Helps decide if rate limiting is needed

---

### 12. Nonce Randomness Quality
**Attack**: Check if nonces are predictable

```cpp
TEST_CASE("Adversarial - NonceRandomness", "[adversarial][crypto]") {
    // Generate 10000 nonces
    // Test for:
    //   - Duplicates (should be none)
    //   - Patterns (should be random)
    //   - Distribution (should be uniform)
    // Expected: High-quality randomness from std::mt19937_64
}
```

**Current behavior**: Uses std::random_device + std::mt19937_64
**Risk**: Very Low - Good random number generator
**Test value**: Documents randomness quality

---

### 13. Transport Callback Timing
**Attack**: Transport callbacks fire in unexpected order

```cpp
TEST_CASE("Adversarial - TransportCallbackOrdering", "[adversarial][race]") {
    // Simulate disconnect_callback() firing before receive_callback()
    // Or receive_callback() firing after disconnect
    // Test: Does Peer handle out-of-order callbacks?
    // Expected: State checks prevent issues
}
```

**Current behavior**: State checks in most places
**Risk**: Low - But transport layer could be weird
**Test value**: Verifies callback robustness

---

### 14. Maximum Message Size Edge Cases
**Attack**: Messages at exactly the limit

```cpp
TEST_CASE("Adversarial - MaxMessageSizeEdgeCases", "[adversarial][edge]") {
    SECTION("Exactly MAX_PROTOCOL_MESSAGE_LENGTH") {
        // Send message with exactly 4MB payload
        // Expected: Accepted
    }

    SECTION("MAX_PROTOCOL_MESSAGE_LENGTH + 1") {
        // Send message with 4MB + 1 byte payload
        // Expected: Rejected (header validation)
    }

    SECTION("Receive buffer at exactly limit") {
        // Fill buffer to exactly DEFAULT_RECV_FLOOD_SIZE
        // Expected: Accepted (at limit, not over)
    }
}
```

**Current behavior**: Should reject oversized messages
**Risk**: Very Low - Already tested
**Test value**: Tests exact boundary conditions

---

### 15. Command Field Padding
**Attack**: Command field with spaces or nulls at end

```cpp
TEST_CASE("Adversarial - CommandFieldPadding", "[adversarial][malformed]") {
    // Send "version\0\0\0\0\0" (command with null padding)
    // Send "version     " (command with space padding)
    // Expected: Should match "version" command
    // Test: Does get_command() handle padding correctly?
}
```

**Current behavior**: get_command() likely stops at first null
**Risk**: Very Low - Edge case
**Test value**: Tests command parsing edge cases

---

## Test Organization

### Suggested file structure:

```
test/network/
├── peer_tests.cpp                    // Unit tests (18 tests)
├── peer_adversarial_tests.cpp        // Current adversarial (13 tests)
├── peer_adversarial_advanced.cpp     // NEW: Advanced attacks (15 tests)
└── peer_integration_tests.cpp        // NEW: Multi-peer scenarios (5 tests)
```

---

## Quick Wins (Easy to Add)

These tests would be easiest to implement:

1. ✅ **PONG Nonce Mismatch** - Simple logic check
2. ✅ **Deserialization Failure Flooding** - Already have infrastructure
3. ✅ **Receive Buffer Cycling** - Just send large messages repeatedly
4. ✅ **Unknown Message Flooding** - Easy to craft unknown commands
5. ✅ **Statistics Overflow** - Simple counter manipulation

---

## High-Value Tests (Worth the Effort)

These would find the most interesting issues:

1. 🎯 **Message Handler Blocking** - Could reveal threading issues
2. 🎯 **Concurrent Disconnect** - Could find race conditions
3. 🎯 **Partial Message Timeout** - Helps decide timeout policy
4. 🎯 **Unknown Message Flooding** - Helps decide rate limiting policy
5. 🎯 **Network Time Poisoning** - Integration test for time security

---

## Summary

### Total Potential New Tests: ~15-20

**P1 (High)**: 5 tests - Unknown flooding, partial timeout, deser failures, PONG mismatch, buffer cycling
**P2 (Medium)**: 5 tests - Handler blocking, concurrent disconnect, stats overflow, time poisoning, self-connection edge cases
**P3 (Low)**: 5 tests - Rate limiting, nonce randomness, callback ordering, max size edge cases, command padding

### Estimated Effort:
- **Quick wins**: 1-2 hours (5 simple tests)
- **High-value tests**: 3-4 hours (5 complex tests)
- **Full suite**: 6-8 hours (all 15 tests)

### Recommendation:
Start with the **Quick Wins + High-Value** tests (10 tests total) for maximum ROI.
