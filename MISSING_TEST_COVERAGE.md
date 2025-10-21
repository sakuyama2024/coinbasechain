# Critical Missing Test Coverage

Analysis performed: 2025-10-20
Current test status: 369 test cases, 4762 assertions

## Priority 1: Critical Gaps (High Impact, Common Bugs)

### 1. Duplicate Connection Prevention ❌ NO TESTS
**Files**: `network/peer_manager.cpp`, `network/network_manager.cpp`

Missing tests:
- [ ] Connecting to same peer twice (same IP, different ports)
- [ ] Reconnection after disconnect (should succeed)
- [ ] Self-connection prevention (connecting to own address)
- [ ] Multiple outbound connections to same peer
- [ ] Inbound + outbound to same peer handling

**Why critical**: Very common bug that wastes connection slots and resources. Bitcoin Core had multiple bugs in this area.

**Test location**: `test/network/duplicate_connection_tests.cpp`

---

### 2. PING/PONG Timeout Handling ❌ NO TESTS
**Files**: `network/peer.cpp`, `network/peer_manager.cpp`

Missing tests:
- [ ] Peer doesn't respond to PING (should timeout and disconnect)
- [ ] Very slow PONG responses
- [ ] Connection timeout enforcement (idle connections)
- [ ] Multiple PING/PONG rounds
- [ ] PONG with wrong nonce

**Why critical**: Detects dead connections. Without this, nodes accumulate zombie peers.

**Test location**: `test/network/ping_pong_tests.cpp`

---

### 3. Anchor Connections ❌ NO TESTS
**Files**: `network/anchor_manager.cpp`

Missing tests:
- [ ] Saving anchor peers to disk on shutdown
- [ ] Loading anchor peers on startup
- [ ] Priority connection to anchors before other peers
- [ ] Anchor peer rotation (updating anchor set)
- [ ] Corrupted anchor file handling

**Why critical**: Ensures nodes can reconnect to network after restart. Critical for network resilience.

**Test location**: `test/network/anchor_tests.cpp`

---

## Priority 2: Important Functionality Gaps

### 4. Connection Limits Enforcement ✅ COMPLETE
**Files**: `network/peer_manager.cpp`

Implemented:
- [x] Max inbound connection limit enforcement (125 default)
- [x] Max outbound connection limit enforcement (8 default)
- [x] Eviction policy documentation tests
- [x] Limit enforcement with 130+ peer test

**Test location**: `test/network/connection_limit_tests.cpp` (7 test cases)

**Implementation notes**:
- Uses SetZeroLatency() for deterministic behavior
- Creates 130 peers to test eviction enforcement (> 125 limit)
- Uses range assertions (>= N-2) instead of exact counts for robustness
- Follows pattern from existing MaxConnectionLimits test

---

### 5. VERSION Handshake Edge Cases ✅ COMPLETE
**Files**: `network/peer.cpp`, `protocol.hpp`

Implemented:
- [x] Incompatible protocol version (too old) - MIN_PROTOCOL_VERSION validation
- [x] Future protocol version (documented as correct behavior - forward compatibility)
- [x] Missing required fields (deserialization failure handling)
- [x] Timeout during handshake (60-second timeout)
- [x] VERACK before VERSION (already enforced)
- [x] Duplicate VERSION message (already ignored)

**Implementation**: Added MIN_PROTOCOL_VERSION constant and validation in peer.cpp:260-269

**Test location**: `test/network/handshake_edge_case_tests.cpp` (11 test cases, 3 assertions)

---

### 6. Address Relay Anti-Sybil ⚠️ PARTIAL COVERAGE
**Files**: `network/addr_manager.cpp`

Existing: Basic ADDR/GETADDR tests
Missing:
- [ ] Address bucketing (tried/new tables)
- [ ] Anti-Sybil protection (bucket limits)
- [ ] Stale address cleanup
- [ ] GETADDR rate limiting (prevent spam)
- [ ] Address announcement limits per peer

**Why critical**: Prevents Sybil attacks on peer discovery.

**Test location**: `test/network/addr_antisybil_tests.cpp`

---

## Priority 3: Consensus & Chain Management

### 7. Chain Selector Edge Cases ⚠️ PARTIAL COVERAGE
**Files**: `chain/chain_selector.cpp`, `chain/chainstate_manager.cpp`

Existing: Basic reorg tests
Missing:
- [ ] Choosing between multiple competing chains (3+ forks)
- [ ] Work calculation overflow/edge cases
- [ ] Invalid chain in the middle (valid tip, invalid ancestor)
- [ ] Extremely long chain with minimal work vs short chain with high work
- [ ] Tie-breaking rules when work is equal

**Why critical**: Core consensus logic. Incorrect selection can cause chain splits.

**Test location**: `test/unit/chain_selector_edge_cases.cpp`

---

### 8. Deep Reorgs ⚠️ PARTIAL COVERAGE
**Files**: `chain/chainstate_manager.cpp`

Existing: Reorgs up to ~20 blocks
Missing:
- [ ] Reorgs > 100 blocks
- [ ] Reorgs > 1000 blocks (stress test)
- [ ] Memory usage during deep reorg
- [ ] Performance implications
- [ ] Very long fork resolution (competing chains for hours)

**Why critical**: Stress test for reorg handling. Real networks see 100+ block reorgs.

**Test location**: `test/integration/deep_reorg_tests.cpp`

---

### 9. Parallel Download ❌ NO TESTS
**Files**: `network/header_sync_manager.cpp`

Missing tests:
- [ ] Downloading headers from 2 peers simultaneously
- [ ] Downloading from 3+ peers simultaneously
- [ ] Handling conflicting chains from different peers
- [ ] Peer prioritization (which peer to request from first?)
- [ ] One peer stalls, others continue

**Why critical**: IBD performance. Parallel download is 5-10x faster than serial.

**Test location**: `test/network/parallel_download_tests.cpp`

---

## Priority 4: Infrastructure & Tooling

### 10. RPC Server ❌ NO TESTS
**Files**: `network/rpc_server.cpp`, `network/rpc_client.cpp`

Missing tests:
- [ ] Basic RPC commands (getinfo, getblockcount, etc.)
- [ ] Authentication (valid/invalid credentials)
- [ ] Error handling (invalid params, method not found)
- [ ] Concurrent requests from multiple clients
- [ ] Request timeout handling
- [ ] Large response handling

**Why critical**: Primary interface for node control and monitoring.

**Test location**: `test/network/rpc_tests.cpp`

---

## Summary by Priority

**Priority 1 (Critical - Start Here)**:
1. Duplicate connection prevention
2. PING/PONG timeout
3. Anchor connections

**Priority 2 (Important)**:
4. Connection limits enforcement ✅ COMPLETE
5. VERSION handshake edge cases ✅ COMPLETE
6. Address relay anti-Sybil

**Priority 3 (Consensus)**:
7. Chain selector edge cases
8. Deep reorgs
9. Parallel download

**Priority 4 (Infrastructure)**:
10. RPC server

## Estimated Test Implementation Time

- Priority 1: ~4-6 hours total
- Priority 2: ~6-8 hours total
- Priority 3: ~8-10 hours total
- Priority 4: ~4-6 hours total

**Total**: ~22-30 hours for complete coverage

## Next Steps

1. Start with **duplicate connection prevention** (highest impact, easiest to implement)
2. Then **PING/PONG timeout** (critical for production)
3. Then **anchor connections** (network resilience)

After Priority 1 is complete, reassess based on which bugs are found in testing.
