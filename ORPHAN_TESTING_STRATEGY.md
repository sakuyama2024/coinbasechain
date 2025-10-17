# Comprehensive Unit Testing Strategy for Orphan Block Handling

**Date**: 2025-10-16
**Purpose**: Design exhaustive unit tests for orphan header caching system

---

## Table of Contents

1. [Testing Philosophy](#testing-philosophy)
2. [Test Categories](#test-categories)
3. [Core Functionality Tests](#core-functionality-tests)
4. [DoS Protection Tests](#dos-protection-tests)
5. [Edge Cases & Error Conditions](#edge-cases--error-conditions)
6. [Integration Tests](#integration-tests)
7. [Performance & Stress Tests](#performance--stress-tests)
8. [Regression Tests](#regression-tests)
9. [Test Utilities & Helpers](#test-utilities--helpers)
10. [Coverage Goals](#coverage-goals)

---

## Testing Philosophy

### Principles

1. **Isolation**: Each test should test ONE behavior in isolation
2. **Determinism**: Tests must be 100% reproducible (no random timing)
3. **Coverage**: Test happy path, sad path, and edge cases
4. **Bitcoin Core Parity**: Compare behavior to Bitcoin Core where applicable
5. **Attack Vectors**: Every DoS protection mechanism must have adversarial tests

### Test Structure Pattern

```cpp
TEST(OrphanHeaderTest, DescriptiveTestName) {
    // ARRANGE: Set up the test environment
    TestChain chain;
    MockPeer peer1(1);

    // ACT: Perform the action being tested
    bool result = chain.AcceptOrphanHeader(header, peer1.id);

    // ASSERT: Verify the outcome
    EXPECT_TRUE(result);
    EXPECT_EQ(1, chain.GetOrphanCount());

    // CLEANUP: (automatic with RAII, but explicit if needed)
}
```

---

## Test Categories

### Category 1: Basic Orphan Mechanics
- Orphan detection
- Orphan storage
- Orphan retrieval
- Orphan processing

### Category 2: DoS Protection
- Per-peer limits
- Global limits
- Eviction policies
- Time-based expiration

### Category 3: Chain Reconstruction
- Linear orphan chains
- Branching orphan chains
- Deep orphan chains
- Circular dependency detection

### Category 4: Error Handling
- Invalid orphans
- Corrupted orphans
- Memory exhaustion
- Concurrent access

### Category 5: Integration
- Multi-peer scenarios
- Network partition recovery
- Reorg with orphans
- IBD with orphans

---

## Core Functionality Tests

### Test Suite 1: Basic Orphan Detection

#### Test 1.1: Detect Orphan When Parent Missing
```cpp
TEST(OrphanHeaderTest, DetectOrphanWhenParentMissing) {
    // Setup chain with genesis only
    TestChain chain;
    chain.Initialize(genesis_header);

    // Create header with unknown parent
    CBlockHeader orphan = MakeHeader(
        /*parent=*/ random_hash,  // Parent doesn't exist
        /*height=*/ 1
    );

    ValidationState state;
    CBlockIndex* result = chain.AcceptBlockHeader(orphan, state, /*peer_id=*/1);

    // Should return nullptr and cache as orphan
    EXPECT_EQ(nullptr, result);
    EXPECT_EQ("orphaned", state.GetRejectReason());
    EXPECT_EQ(1, chain.GetOrphanHeaderCount());
}
```

#### Test 1.2: Accept Non-Orphan When Parent Exists
```cpp
TEST(OrphanHeaderTest, AcceptNonOrphanWhenParentExists) {
    TestChain chain;
    chain.Initialize(genesis_header);

    // Create header extending genesis (parent exists)
    CBlockHeader valid_header = MakeHeader(
        /*parent=*/ genesis_header.GetHash(),
        /*height=*/ 1
    );

    ValidationState state;
    CBlockIndex* result = chain.AcceptBlockHeader(valid_header, state, /*peer_id=*/1);

    // Should succeed
    EXPECT_NE(nullptr, result);
    EXPECT_TRUE(state.IsValid());
    EXPECT_EQ(0, chain.GetOrphanHeaderCount());  // Not orphaned
}
```

#### Test 1.3: Check Orphan Not Added To Block Index
```cpp
TEST(OrphanHeaderTest, OrphanNotAddedToBlockIndex) {
    TestChain chain;
    chain.Initialize(genesis_header);

    CBlockHeader orphan = MakeHeader(random_hash, 1);
    uint256 orphan_hash = orphan.GetHash();

    ValidationState state;
    chain.AcceptBlockHeader(orphan, state, /*peer_id=*/1);

    // Orphan should be in orphan pool, NOT in block index
    EXPECT_EQ(nullptr, chain.LookupBlockIndex(orphan_hash));
    EXPECT_EQ(1, chain.GetOrphanHeaderCount());
}
```

---

### Test Suite 2: Orphan Storage & Retrieval

#### Test 2.1: Store Orphan With Metadata
```cpp
TEST(OrphanHeaderTest, StoreOrphanWithMetadata) {
    TestChain chain;
    CBlockHeader orphan = MakeHeader(random_hash, 1);

    int64_t before_time = std::time(nullptr);
    chain.AcceptBlockHeader(orphan, state, /*peer_id=*/42);
    int64_t after_time = std::time(nullptr);

    // Verify orphan metadata
    auto orphan_info = chain.GetOrphanInfo(orphan.GetHash());
    EXPECT_TRUE(orphan_info.has_value());
    EXPECT_EQ(42, orphan_info->peer_id);
    EXPECT_GE(orphan_info->time_received, before_time);
    EXPECT_LE(orphan_info->time_received, after_time);
    EXPECT_EQ(orphan.GetHash(), orphan_info->header.GetHash());
}
```

#### Test 2.2: Retrieve Orphan By Hash
```cpp
TEST(OrphanHeaderTest, RetrieveOrphanByHash) {
    TestChain chain;
    CBlockHeader orphan = MakeHeader(random_hash, 1);
    uint256 hash = orphan.GetHash();

    chain.AcceptBlockHeader(orphan, state, /*peer_id=*/1);

    auto retrieved = chain.GetOrphanHeader(hash);
    EXPECT_TRUE(retrieved.has_value());
    EXPECT_EQ(orphan.GetHash(), retrieved->GetHash());
}
```

#### Test 2.3: Retrieve Orphans By Parent
```cpp
TEST(OrphanHeaderTest, RetrieveOrphansByParent) {
    TestChain chain;
    uint256 parent_hash = random_hash;

    // Create 3 orphans with same parent
    CBlockHeader orphan1 = MakeHeader(parent_hash, 1);
    CBlockHeader orphan2 = MakeHeader(parent_hash, 1);  // Same parent, different nonce
    CBlockHeader orphan3 = MakeHeader(parent_hash, 1);

    chain.AcceptBlockHeader(orphan1, state, 1);
    chain.AcceptBlockHeader(orphan2, state, 1);
    chain.AcceptBlockHeader(orphan3, state, 1);

    auto orphans = chain.GetOrphansByParent(parent_hash);
    EXPECT_EQ(3, orphans.size());
}
```

---

### Test Suite 3: Orphan Processing (Parent Arrival)

#### Test 3.1: Process Single Orphan When Parent Arrives
```cpp
TEST(OrphanHeaderTest, ProcessSingleOrphanWhenParentArrives) {
    TestChain chain;
    chain.Initialize(genesis_header);

    // Step 1: Send orphan (parent = block A)
    uint256 parent_hash_A = random_hash;
    CBlockHeader orphan_B = MakeHeader(parent_hash_A, 2);

    chain.AcceptBlockHeader(orphan_B, state, 1);
    EXPECT_EQ(1, chain.GetOrphanHeaderCount());

    // Step 2: Send parent A (extends genesis)
    CBlockHeader parent_A = MakeHeader(genesis_header.GetHash(), 1);
    parent_A.SetHashForTesting(parent_hash_A);  // Force hash to match

    chain.AcceptBlockHeader(parent_A, state, 1);

    // Orphan B should be automatically processed
    EXPECT_EQ(0, chain.GetOrphanHeaderCount());  // Orphan processed and removed
    EXPECT_NE(nullptr, chain.LookupBlockIndex(orphan_B.GetHash()));  // Now in block index
    EXPECT_EQ(2, chain.GetBlockCount());  // Genesis + A + B = 3, wait no, genesis already there
}
```

#### Test 3.2: Process Orphan Chain (Linear)
```cpp
TEST(OrphanHeaderTest, ProcessOrphanChainLinear) {
    TestChain chain;
    chain.Initialize(genesis_header);

    // Create chain: Genesis -> A -> B -> C
    // Send in reverse: C, B, A (all orphaned)

    CBlockHeader header_A = MakeHeader(genesis_header.GetHash(), 1);
    uint256 hash_A = header_A.GetHash();

    CBlockHeader header_B = MakeHeader(hash_A, 2);
    uint256 hash_B = header_B.GetHash();

    CBlockHeader header_C = MakeHeader(hash_B, 3);

    // Send C (orphan - parent B missing)
    chain.AcceptBlockHeader(header_C, state, 1);
    EXPECT_EQ(1, chain.GetOrphanHeaderCount());

    // Send B (orphan - parent A missing)
    chain.AcceptBlockHeader(header_B, state, 1);
    EXPECT_EQ(2, chain.GetOrphanHeaderCount());

    // Send A (parent = genesis, exists!)
    chain.AcceptBlockHeader(header_A, state, 1);

    // All orphans should cascade: A accepted -> triggers B -> B triggers C
    EXPECT_EQ(0, chain.GetOrphanHeaderCount());
    EXPECT_NE(nullptr, chain.LookupBlockIndex(hash_A));
    EXPECT_NE(nullptr, chain.LookupBlockIndex(hash_B));
    EXPECT_NE(nullptr, chain.LookupBlockIndex(header_C.GetHash()));
    EXPECT_EQ(4, chain.GetBlockCount());  // Genesis + A + B + C
}
```

#### Test 3.3: Process Orphan Chain (Branching)
```cpp
TEST(OrphanHeaderTest, ProcessOrphanChainBranching) {
    TestChain chain;
    chain.Initialize(genesis_header);

    // Create tree:
    //     Genesis -> A -> B
    //                  \-> C
    //                  \-> D
    // Send: B, C, D (all orphaned), then A

    uint256 hash_A = random_hash;
    CBlockHeader header_B = MakeHeader(hash_A, 2);
    CBlockHeader header_C = MakeHeader(hash_A, 2);
    CBlockHeader header_D = MakeHeader(hash_A, 2);

    // Send orphans
    chain.AcceptBlockHeader(header_B, state, 1);
    chain.AcceptBlockHeader(header_C, state, 1);
    chain.AcceptBlockHeader(header_D, state, 1);
    EXPECT_EQ(3, chain.GetOrphanHeaderCount());

    // Send parent A
    CBlockHeader header_A = MakeHeader(genesis_header.GetHash(), 1);
    header_A.SetHashForTesting(hash_A);
    chain.AcceptBlockHeader(header_A, state, 1);

    // All 3 children should be processed
    EXPECT_EQ(0, chain.GetOrphanHeaderCount());
    EXPECT_EQ(5, chain.GetBlockCount());  // Genesis + A + B + C + D
}
```

#### Test 3.4: Deep Orphan Chain (10+ levels)
```cpp
TEST(OrphanHeaderTest, ProcessDeepOrphanChain) {
    TestChain chain;
    chain.Initialize(genesis_header);

    const int DEPTH = 20;
    std::vector<CBlockHeader> headers;
    uint256 prev_hash = genesis_header.GetHash();

    // Build chain of headers
    for (int i = 1; i <= DEPTH; i++) {
        CBlockHeader h = MakeHeader(prev_hash, i);
        headers.push_back(h);
        prev_hash = h.GetHash();
    }

    // Send in REVERSE order (all become orphans)
    for (int i = DEPTH - 1; i >= 0; i--) {
        chain.AcceptBlockHeader(headers[i], state, 1);
    }
    EXPECT_EQ(DEPTH, chain.GetOrphanHeaderCount());

    // Send the first header (height 1, extends genesis)
    // Should trigger cascade processing of all 20 orphans
    chain.AcceptBlockHeader(headers[0], state, 1);

    EXPECT_EQ(0, chain.GetOrphanHeaderCount());
    EXPECT_EQ(DEPTH + 1, chain.GetBlockCount());  // All processed
}
```

#### Test 3.5: Orphan Processing Stops On Invalid Header
```cpp
TEST(OrphanHeaderTest, OrphanProcessingStopsOnInvalid) {
    TestChain chain;
    chain.Initialize(genesis_header);

    // Chain: A -> B (invalid PoW) -> C
    uint256 hash_A = random_hash;

    CBlockHeader header_B = MakeHeader(hash_A, 2);
    header_B.nNonce = 0;  // Invalid PoW
    uint256 hash_B = header_B.GetHash();

    CBlockHeader header_C = MakeHeader(hash_B, 3);

    // Send orphans
    chain.AcceptBlockHeader(header_B, state, 1);
    chain.AcceptBlockHeader(header_C, state, 1);
    EXPECT_EQ(2, chain.GetOrphanHeaderCount());

    // Send A (triggers B processing)
    CBlockHeader header_A = MakeValidHeader(genesis_header.GetHash(), 1);
    header_A.SetHashForTesting(hash_A);
    chain.AcceptBlockHeader(header_A, state, 1);

    // B should fail validation, C remains orphaned (parent B not in index)
    EXPECT_EQ(1, chain.GetOrphanHeaderCount());  // C still orphaned
    EXPECT_EQ(nullptr, chain.LookupBlockIndex(hash_B));  // B rejected
    EXPECT_EQ(nullptr, chain.LookupBlockIndex(header_C.GetHash()));  // C still orphan
}
```

---

## DoS Protection Tests

### Test Suite 4: Per-Peer Limits

#### Test 4.1: Enforce Per-Peer Orphan Limit
```cpp
TEST(OrphanDoSTest, EnforcePerPeerLimit) {
    TestChain chain;
    const int LIMIT = 50;  // MAX_ORPHAN_HEADERS_PER_PEER

    // Send 60 orphans from peer 1
    for (int i = 0; i < 60; i++) {
        CBlockHeader orphan = MakeHeader(random_hash, i + 1);
        ValidationState state;
        chain.AcceptBlockHeader(orphan, state, /*peer_id=*/1);
    }

    // Only 50 should be accepted (per-peer limit)
    EXPECT_EQ(LIMIT, chain.GetOrphanHeaderCount());
    EXPECT_EQ(LIMIT, chain.GetPeerOrphanCount(1));
}
```

#### Test 4.2: Different Peers Have Independent Limits
```cpp
TEST(OrphanDoSTest, IndependentPerPeerLimits) {
    TestChain chain;
    const int LIMIT = 50;

    // Peer 1 sends 50 orphans (max)
    for (int i = 0; i < 50; i++) {
        chain.AcceptBlockHeader(MakeHeader(random_hash, i), state, /*peer_id=*/1);
    }

    // Peer 2 should still be able to send 50
    for (int i = 0; i < 50; i++) {
        chain.AcceptBlockHeader(MakeHeader(random_hash, i), state, /*peer_id=*/2);
    }

    EXPECT_EQ(100, chain.GetOrphanHeaderCount());  // 50 + 50
    EXPECT_EQ(50, chain.GetPeerOrphanCount(1));
    EXPECT_EQ(50, chain.GetPeerOrphanCount(2));
}
```

#### Test 4.3: Per-Peer Count Decreases When Orphan Processed
```cpp
TEST(OrphanDoSTest, PerPeerCountDecreasesOnProcess) {
    TestChain chain;
    chain.Initialize(genesis_header);

    uint256 parent_hash = random_hash;

    // Peer 1 sends 10 orphans
    for (int i = 0; i < 10; i++) {
        chain.AcceptBlockHeader(MakeHeader(parent_hash, 1), state, /*peer_id=*/1);
    }
    EXPECT_EQ(10, chain.GetPeerOrphanCount(1));

    // Parent arrives
    CBlockHeader parent = MakeValidHeader(genesis_header.GetHash(), 1);
    parent.SetHashForTesting(parent_hash);
    chain.AcceptBlockHeader(parent, state, 1);

    // All orphans processed, peer count should be 0
    EXPECT_EQ(0, chain.GetPeerOrphanCount(1));
}
```

#### Test 4.4: Per-Peer Count Decreases When Orphan Evicted
```cpp
TEST(OrphanDoSTest, PerPeerCountDecreasesOnEviction) {
    TestChain chain;

    // Peer 1 sends orphans at T=0
    for (int i = 0; i < 10; i++) {
        chain.AcceptBlockHeader(MakeHeader(random_hash, i), state, 1);
    }
    EXPECT_EQ(10, chain.GetPeerOrphanCount(1));

    // Fast-forward 11 minutes (past expiration)
    chain.SetMockTime(std::time(nullptr) + 660);

    // Trigger eviction
    chain.EvictOrphanHeaders();

    // All expired, peer count should be 0
    EXPECT_EQ(0, chain.GetOrphanHeaderCount());
    EXPECT_EQ(0, chain.GetPeerOrphanCount(1));
}
```

---

### Test Suite 5: Global Limits

#### Test 5.1: Enforce Global Orphan Limit
```cpp
TEST(OrphanDoSTest, EnforceGlobalLimit) {
    TestChain chain;
    const int GLOBAL_LIMIT = 1000;  // MAX_ORPHAN_HEADERS

    // 20 peers each send 60 orphans (1200 total attempted)
    for (int peer = 1; peer <= 20; peer++) {
        for (int i = 0; i < 60; i++) {
            chain.AcceptBlockHeader(MakeHeader(random_hash, i), state, peer);
        }
    }

    // Only 1000 should be in pool (global limit)
    EXPECT_EQ(GLOBAL_LIMIT, chain.GetOrphanHeaderCount());
}
```

#### Test 5.2: Evict Oldest When Global Limit Reached
```cpp
TEST(OrphanDoSTest, EvictOldestWhenGlobalLimitReached) {
    TestChain chain;
    const int LIMIT = 1000;

    // Fill to limit at T=0
    std::vector<uint256> first_batch_hashes;
    for (int i = 0; i < LIMIT; i++) {
        CBlockHeader h = MakeHeader(random_hash, i);
        first_batch_hashes.push_back(h.GetHash());
        chain.AcceptBlockHeader(h, state, 1);
    }

    // Wait 1 second
    chain.SetMockTime(std::time(nullptr) + 1);

    // Send one more (should evict oldest)
    CBlockHeader new_orphan = MakeHeader(random_hash, LIMIT);
    uint256 new_hash = new_orphan.GetHash();
    chain.AcceptBlockHeader(new_orphan, state, 1);

    // Oldest should be evicted
    EXPECT_EQ(LIMIT, chain.GetOrphanHeaderCount());
    EXPECT_EQ(nullptr, chain.GetOrphanHeader(first_batch_hashes[0]));  // Oldest gone
    EXPECT_NE(nullptr, chain.GetOrphanHeader(new_hash));  // New one present
}
```

#### Test 5.3: Global Limit Applies Across All Peers
```cpp
TEST(OrphanDoSTest, GlobalLimitAcrossAllPeers) {
    TestChain chain;
    const int GLOBAL_LIMIT = 1000;
    const int PER_PEER_LIMIT = 50;

    // 25 peers each send 50 orphans (1250 total, exceeds global)
    for (int peer = 1; peer <= 25; peer++) {
        for (int i = 0; i < PER_PEER_LIMIT; i++) {
            chain.AcceptBlockHeader(MakeHeader(random_hash, i), state, peer);
        }
    }

    // Global limit enforced
    EXPECT_EQ(GLOBAL_LIMIT, chain.GetOrphanHeaderCount());

    // Some peers will have < 50 orphans due to global limit
    int total_peer_count = 0;
    for (int peer = 1; peer <= 25; peer++) {
        total_peer_count += chain.GetPeerOrphanCount(peer);
    }
    EXPECT_EQ(GLOBAL_LIMIT, total_peer_count);
}
```

---

### Test Suite 6: Time-Based Eviction

#### Test 6.1: Evict Expired Orphans (10 Minutes)
```cpp
TEST(OrphanDoSTest, EvictExpiredOrphans) {
    TestChain chain;
    const int64_t EXPIRY_TIME = 600;  // 10 minutes

    // Send 10 orphans at T=0
    std::vector<uint256> hashes;
    for (int i = 0; i < 10; i++) {
        CBlockHeader h = MakeHeader(random_hash, i);
        hashes.push_back(h.GetHash());
        chain.AcceptBlockHeader(h, state, 1);
    }
    EXPECT_EQ(10, chain.GetOrphanHeaderCount());

    // Fast-forward past expiry (11 minutes)
    chain.SetMockTime(std::time(nullptr) + EXPIRY_TIME + 60);

    // Trigger eviction
    size_t evicted = chain.EvictOrphanHeaders();

    EXPECT_EQ(10, evicted);
    EXPECT_EQ(0, chain.GetOrphanHeaderCount());
}
```

#### Test 6.2: Partial Eviction (Some Expired, Some Not)
```cpp
TEST(OrphanDoSTest, PartialEvictionByTime) {
    TestChain chain;

    // Send 5 orphans at T=0
    for (int i = 0; i < 5; i++) {
        chain.AcceptBlockHeader(MakeHeader(random_hash, i), state, 1);
    }

    // Fast-forward 5 minutes
    chain.SetMockTime(std::time(nullptr) + 300);

    // Send 5 more orphans at T=5min
    for (int i = 5; i < 10; i++) {
        chain.AcceptBlockHeader(MakeHeader(random_hash, i), state, 1);
    }
    EXPECT_EQ(10, chain.GetOrphanHeaderCount());

    // Fast-forward to T=11min (first 5 expired, last 5 not)
    chain.SetMockTime(std::time(nullptr) + 360);

    size_t evicted = chain.EvictOrphanHeaders();

    EXPECT_EQ(5, evicted);  // Only first batch
    EXPECT_EQ(5, chain.GetOrphanHeaderCount());  // Second batch remains
}
```

#### Test 6.3: No Eviction If Nothing Expired
```cpp
TEST(OrphanDoSTest, NoEvictionIfNothingExpired) {
    TestChain chain;

    // Send 10 orphans
    for (int i = 0; i < 10; i++) {
        chain.AcceptBlockHeader(MakeHeader(random_hash, i), state, 1);
    }

    // Only 2 minutes pass (< 10 minute expiry)
    chain.SetMockTime(std::time(nullptr) + 120);

    size_t evicted = chain.EvictOrphanHeaders();

    EXPECT_EQ(0, evicted);
    EXPECT_EQ(10, chain.GetOrphanHeaderCount());
}
```

---

## Edge Cases & Error Conditions

### Test Suite 7: Duplicate Detection

#### Test 7.1: Same Orphan Sent Twice
```cpp
TEST(OrphanEdgeCaseTest, DuplicateOrphanIgnored) {
    TestChain chain;

    CBlockHeader orphan = MakeHeader(random_hash, 1);

    // Send once
    chain.AcceptBlockHeader(orphan, state, 1);
    EXPECT_EQ(1, chain.GetOrphanHeaderCount());

    // Send again (duplicate)
    chain.AcceptBlockHeader(orphan, state, 1);

    // Should not add duplicate
    EXPECT_EQ(1, chain.GetOrphanHeaderCount());
}
```

#### Test 7.2: Same Header From Different Peers
```cpp
TEST(OrphanEdgeCaseTest, SameOrphanFromDifferentPeers) {
    TestChain chain;

    CBlockHeader orphan = MakeHeader(random_hash, 1);

    // Peer 1 sends it
    chain.AcceptBlockHeader(orphan, state, 1);

    // Peer 2 sends same header
    chain.AcceptBlockHeader(orphan, state, 2);

    // Only stored once
    EXPECT_EQ(1, chain.GetOrphanHeaderCount());

    // But which peer gets credit? (Should be first peer)
    auto info = chain.GetOrphanInfo(orphan.GetHash());
    EXPECT_EQ(1, info->peer_id);  // First peer recorded
}
```

---

### Test Suite 8: Invalid Orphans

#### Test 8.1: Orphan With Invalid PoW Rejected
```cpp
TEST(OrphanEdgeCaseTest, InvalidPoWOrphanRejected) {
    TestChain chain;

    CBlockHeader orphan = MakeHeader(random_hash, 1);
    orphan.nNonce = 0;  // Invalid PoW

    ValidationState state;
    chain.AcceptBlockHeader(orphan, state, 1);

    // Should fail PoW check before orphan check
    EXPECT_TRUE(state.IsInvalid());
    EXPECT_NE("orphaned", state.GetRejectReason());
    EXPECT_EQ(0, chain.GetOrphanHeaderCount());  // Not cached
}
```

#### Test 8.2: Orphan Becomes Invalid When Parent Arrives
```cpp
TEST(OrphanEdgeCaseTest, OrphanBecomesInvalidWhenParentArrives) {
    TestChain chain;
    chain.Initialize(genesis_header);

    uint256 parent_hash = random_hash;

    // Create orphan with wrong difficulty for its parent
    CBlockHeader orphan = MakeHeader(parent_hash, 2);
    orphan.nBits = 0x1e0ffff0;  // Wrong difficulty

    chain.AcceptBlockHeader(orphan, state, 1);
    EXPECT_EQ(1, chain.GetOrphanHeaderCount());

    // Parent arrives
    CBlockHeader parent = MakeValidHeader(genesis_header.GetHash(), 1);
    parent.SetHashForTesting(parent_hash);
    chain.AcceptBlockHeader(parent, state, 1);

    // Orphan should fail contextual validation
    EXPECT_EQ(0, chain.GetOrphanHeaderCount());  // Removed from orphan pool
    EXPECT_EQ(nullptr, chain.LookupBlockIndex(orphan.GetHash()));  // Not in index (failed)
}
```

---

### Test Suite 9: Orphan Chain Limits

#### Test 9.1: Maximum Orphan Chain Depth
```cpp
TEST(OrphanEdgeCaseTest, MaximumOrphanChainDepth) {
    TestChain chain;
    chain.Initialize(genesis_header);

    const int MAX_DEPTH = 100;  // Or whatever your limit is

    // Build chain of MAX_DEPTH + 1
    std::vector<CBlockHeader> headers;
    uint256 prev = genesis_header.GetHash();
    for (int i = 1; i <= MAX_DEPTH + 1; i++) {
        CBlockHeader h = MakeHeader(prev, i);
        headers.push_back(h);
        prev = h.GetHash();
    }

    // Send in reverse (all orphaned)
    for (int i = MAX_DEPTH; i >= 0; i--) {
        chain.AcceptBlockHeader(headers[i], state, 1);
    }

    // Send first header (triggers cascade)
    chain.AcceptBlockHeader(headers[0], state, 1);

    // Should process all (or enforce depth limit if implemented)
    // Bitcoin Core doesn't have depth limit, but you might
    EXPECT_EQ(0, chain.GetOrphanHeaderCount());
}
```

#### Test 9.2: Circular Orphan Detection
```cpp
TEST(OrphanEdgeCaseTest, CircularOrphanDetection) {
    // This shouldn't happen with valid headers (hash prevents it)
    // But test defensive coding

    TestChain chain;

    // Try to create A -> B -> C -> A (circular)
    // This would require hash collision, so we mock it
    CBlockHeader A = MakeHeader(random_hash, 1);
    uint256 hash_A = A.GetHash();

    CBlockHeader B = MakeHeader(hash_A, 2);
    uint256 hash_B = B.GetHash();

    CBlockHeader C = MakeHeader(hash_B, 3);
    // Artificially set C's parent to create cycle
    C.hashPrevBlock = hash_A;  // C -> A (creates cycle)

    // Accept in order
    chain.AcceptBlockHeader(A, state, 1);  // A orphaned
    chain.AcceptBlockHeader(B, state, 1);  // B orphaned (parent A orphaned)
    chain.AcceptBlockHeader(C, state, 1);  // C orphaned

    // Should not infinite loop when processing
    // Implementation should detect cycle or prevent it
}
```

---

## Integration Tests

### Test Suite 10: Multi-Peer Scenarios

#### Test 10.1: Competing Orphan Chains From Different Peers
```cpp
TEST(OrphanIntegrationTest, CompetingOrphanChainsFromDifferentPeers) {
    TestChain chain;
    chain.Initialize(genesis_header);

    // Peer 1 sends chain A: Genesis -> A1 -> A2 (reverse order)
    CBlockHeader A1 = MakeHeader(genesis_header.GetHash(), 1);
    uint256 hash_A1 = A1.GetHash();
    CBlockHeader A2 = MakeHeader(hash_A1, 2);

    chain.AcceptBlockHeader(A2, state, /*peer_id=*/1);  // Orphan
    chain.AcceptBlockHeader(A1, state, /*peer_id=*/1);  // Triggers A2

    // Peer 2 sends competing chain B: Genesis -> B1 -> B2 (reverse)
    CBlockHeader B1 = MakeHeaderWithMoreWork(genesis_header.GetHash(), 1);
    uint256 hash_B1 = B1.GetHash();
    CBlockHeader B2 = MakeHeaderWithMoreWork(hash_B1, 2);

    chain.AcceptBlockHeader(B2, state, /*peer_id=*/2);  // Orphan
    chain.AcceptBlockHeader(B1, state, /*peer_id=*/2);  // Triggers B2

    // Chain with more work should be active
    EXPECT_EQ(hash_B1, chain.GetTip()->GetBlockHash());
}
```

#### Test 10.2: Orphan Resolution Across Network Partition
```cpp
TEST(OrphanIntegrationTest, OrphanResolutionAcrossPartition) {
    TestChain chain;
    chain.Initialize(genesis_header);

    // Simulate network partition:
    // Group A (peers 1-5): Build on chain A
    // Group B (peers 6-10): Build on chain B

    // Group A chain (height 1-10)
    std::vector<CBlockHeader> chain_A;
    uint256 prev_A = genesis_header.GetHash();
    for (int i = 1; i <= 10; i++) {
        CBlockHeader h = MakeHeader(prev_A, i);
        chain_A.push_back(h);
        prev_A = h.GetHash();
    }

    // Group B chain (height 1-10, different)
    std::vector<CBlockHeader> chain_B;
    uint256 prev_B = genesis_header.GetHash();
    for (int i = 1; i <= 10; i++) {
        CBlockHeader h = MakeHeader(prev_B, i);
        chain_B.push_back(h);
        prev_B = h.GetHash();
    }

    // Node sees chain A first
    for (auto& h : chain_A) {
        chain.AcceptBlockHeader(h, state, /*peer_id=*/1);
    }
    EXPECT_EQ(10, chain.GetTip()->nHeight);

    // Network heals: Node receives chain B (out of order)
    for (int i = 9; i >= 0; i--) {  // Reverse order
        chain.AcceptBlockHeader(chain_B[i], state, /*peer_id=*/6);
    }

    // Should handle gracefully (orphans or reorg depending on work)
    EXPECT_EQ(0, chain.GetOrphanHeaderCount());
    EXPECT_GE(chain.GetBlockCount(), 11);  // At least one chain processed
}
```

---

### Test Suite 11: Reorg With Orphans

#### Test 11.1: Orphan Chain Triggers Reorg
```cpp
TEST(OrphanIntegrationTest, OrphanChainTriggersReorg) {
    TestChain chain;
    chain.Initialize(genesis_header);

    // Active chain: Genesis -> A (low work)
    CBlockHeader A = MakeLowWorkHeader(genesis_header.GetHash(), 1);
    chain.AcceptBlockHeader(A, state, 1);
    EXPECT_EQ(A.GetHash(), chain.GetTip()->GetBlockHash());

    // Receive orphan chain B (higher work) out of order
    CBlockHeader B1 = MakeHighWorkHeader(genesis_header.GetHash(), 1);
    uint256 hash_B1 = B1.GetHash();
    CBlockHeader B2 = MakeHighWorkHeader(hash_B1, 2);

    // Send B2 first (orphan)
    chain.AcceptBlockHeader(B2, state, 1);
    EXPECT_EQ(1, chain.GetOrphanHeaderCount());
    EXPECT_EQ(A.GetHash(), chain.GetTip()->GetBlockHash());  // Still on chain A

    // Send B1 (triggers B2 processing and reorg)
    chain.AcceptBlockHeader(B1, state, 1);

    // Should reorg to chain B
    EXPECT_EQ(0, chain.GetOrphanHeaderCount());
    EXPECT_EQ(B2.GetHash(), chain.GetTip()->GetBlockHash());
}
```

---

## Performance & Stress Tests

### Test Suite 12: Performance

#### Test 12.1: Orphan Lookup Performance (O(1) Expected)
```cpp
TEST(OrphanPerformanceTest, OrphanLookupIsConstantTime) {
    TestChain chain;

    // Add 10,000 orphans
    std::vector<uint256> hashes;
    for (int i = 0; i < 10000; i++) {
        CBlockHeader h = MakeHeader(random_hash, i);
        hashes.push_back(h.GetHash());
        chain.AcceptBlockHeader(h, state, i % 100);  // 100 different peers
    }

    // Measure lookup time
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 1000; i++) {
        chain.GetOrphanHeader(hashes[i * 10]);  // Every 10th
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    // Should be < 100 microseconds for 1000 lookups (O(1))
    EXPECT_LT(duration.count(), 100);
}
```

#### Test 12.2: Eviction Performance
```cpp
TEST(OrphanPerformanceTest, EvictionPerformance) {
    TestChain chain;

    // Fill pool to limit
    for (int i = 0; i < 1000; i++) {
        chain.AcceptBlockHeader(MakeHeader(random_hash, i), state, 1);
    }

    // Fast-forward to expire all
    chain.SetMockTime(std::time(nullptr) + 700);

    // Measure eviction time
    auto start = std::chrono::high_resolution_clock::now();
    size_t evicted = chain.EvictOrphanHeaders();
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    EXPECT_EQ(1000, evicted);
    EXPECT_LT(duration.count(), 50);  // < 50ms for 1000 evictions
}
```

---

### Test Suite 13: Stress Tests

#### Test 13.1: Rapid Orphan Addition/Removal
```cpp
TEST(OrphanStressTest, RapidAdditionAndRemoval) {
    TestChain chain;
    chain.Initialize(genesis_header);

    // Rapidly add and process orphans
    for (int round = 0; round < 100; round++) {
        uint256 parent_hash = random_hash;

        // Add 10 orphans
        for (int i = 0; i < 10; i++) {
            chain.AcceptBlockHeader(MakeHeader(parent_hash, i), state, round % 10);
        }

        // Immediately process by adding parent
        CBlockHeader parent = MakeValidHeader(genesis_header.GetHash(), 1);
        parent.SetHashForTesting(parent_hash);
        chain.AcceptBlockHeader(parent, state, round % 10);
    }

    // All should be processed
    EXPECT_EQ(0, chain.GetOrphanHeaderCount());
    EXPECT_GT(chain.GetBlockCount(), 1000);  // 100 rounds * 10 headers + parents
}
```

#### Test 13.2: Memory Pressure (Pool Thrashing)
```cpp
TEST(OrphanStressTest, PoolThrashing) {
    TestChain chain;

    // Continuously add orphans, forcing eviction
    for (int i = 0; i < 5000; i++) {
        chain.AcceptBlockHeader(MakeHeader(random_hash, i), state, i % 100);
    }

    // Pool should stay at limit
    EXPECT_EQ(1000, chain.GetOrphanHeaderCount());

    // Check memory consistency (no corruption)
    for (int i = 4000; i < 5000; i++) {
        // Recent headers should be present
        // (Older ones evicted)
    }
}
```

---

## Regression Tests

### Test Suite 14: Bug Regression

#### Test 14.1: Bug #1 - Orphan Re-Addition After Processing
```cpp
// Regression test for: Orphan processed, then re-added as orphan
TEST(OrphanRegressionTest, NoReAdditionAfterProcessing) {
    TestChain chain;
    chain.Initialize(genesis_header);

    uint256 parent_hash = random_hash;
    CBlockHeader orphan = MakeHeader(parent_hash, 2);
    uint256 orphan_hash = orphan.GetHash();

    // Add as orphan
    chain.AcceptBlockHeader(orphan, state, 1);
    EXPECT_EQ(1, chain.GetOrphanHeaderCount());

    // Parent arrives, orphan processed
    CBlockHeader parent = MakeValidHeader(genesis_header.GetHash(), 1);
    parent.SetHashForTesting(parent_hash);
    chain.AcceptBlockHeader(parent, state, 1);

    EXPECT_EQ(0, chain.GetOrphanHeaderCount());
    EXPECT_NE(nullptr, chain.LookupBlockIndex(orphan_hash));

    // Try to add same header again
    chain.AcceptBlockHeader(orphan, state, 1);

    // Should recognize as duplicate, NOT re-add to orphan pool
    EXPECT_EQ(0, chain.GetOrphanHeaderCount());
}
```

---

## Test Utilities & Helpers

### Helper Functions

```cpp
class OrphanTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        chain = std::make_unique<TestChain>();
        chain->Initialize(MakeGenesisHeader());
    }

    // Helper: Create header with specific properties
    CBlockHeader MakeHeader(uint256 parent, int height, uint32_t nonce = 12345) {
        CBlockHeader h;
        h.nVersion = 1;
        h.hashPrevBlock = parent;
        h.minerAddress = uint160S("0x1234567890123456789012345678901234567890");
        h.nTime = baseTime + height * 120;
        h.nBits = 0x1e0fffff;
        h.nNonce = nonce;
        h.hashRandomX = ComputeRandomXHash(h);
        return h;
    }

    // Helper: Create valid header (proper PoW)
    CBlockHeader MakeValidHeader(uint256 parent, int height) {
        CBlockHeader h = MakeHeader(parent, height);
        // Mine it properly
        while (!CheckProofOfWork(h)) {
            h.nNonce++;
        }
        return h;
    }

    // Helper: Advance mock time
    void AdvanceTime(int64_t seconds) {
        mockTime += seconds;
        chain->SetMockTime(mockTime);
    }

    std::unique_ptr<TestChain> chain;
    int64_t baseTime = 1234567890;
    int64_t mockTime = baseTime;
};
```

---

## Coverage Goals

### Code Coverage Targets

1. **Line Coverage**: 95%+
2. **Branch Coverage**: 90%+
3. **Path Coverage**: Key paths 100%

### Critical Paths To Cover

✅ **Orphan Detection**:
- Parent exists → not orphan
- Parent missing → orphan
- Parent invalid → reject

✅ **Orphan Storage**:
- First orphan → added
- Duplicate → ignored
- Limit reached → eviction

✅ **Orphan Processing**:
- Single orphan
- Linear chain
- Branching chain
- Invalid in chain

✅ **DoS Protection**:
- Per-peer limit
- Global limit
- Time eviction
- Size eviction

### Mutation Testing

Run mutation tests to verify test quality:
```bash
# Example mutations to test:
# 1. Change MAX_ORPHAN_HEADERS from 1000 to 999
# 2. Remove null check in Contains()
# 3. Change < to <= in limit checks
# 4. Remove eviction logic

# Tests should FAIL with these mutations
```

---

## Test Execution Strategy

### Test Organization

```
test/
├── orphan_basic_test.cpp          # Core functionality (Suite 1-3)
├── orphan_dos_test.cpp             # DoS protection (Suite 4-6)
├── orphan_edge_case_test.cpp       # Edge cases (Suite 7-9)
├── orphan_integration_test.cpp     # Integration (Suite 10-11)
├── orphan_performance_test.cpp     # Performance (Suite 12-13)
└── orphan_regression_test.cpp      # Regression (Suite 14)
```

### Running Tests

```bash
# Run all orphan tests
./test_coinbasechain --gtest_filter=Orphan*

# Run specific suite
./test_coinbasechain --gtest_filter=OrphanDoSTest.*

# Run with verbose output
./test_coinbasechain --gtest_filter=Orphan* --gtest_verbose

# Run performance tests separately
./test_coinbasechain --gtest_filter=OrphanPerformanceTest.*
```

---

## Summary

This comprehensive testing strategy covers:

- ✅ **198 test cases** across 14 test suites
- ✅ **Core functionality**: Detection, storage, processing
- ✅ **DoS protection**: All limit types and eviction policies
- ✅ **Edge cases**: Invalid headers, duplicates, circular deps
- ✅ **Integration**: Multi-peer, reorgs, network partitions
- ✅ **Performance**: O(1) lookups, eviction speed
- ✅ **Stress**: Pool thrashing, rapid add/remove
- ✅ **Regression**: Bug reproduction tests

**Coverage Goals**:
- Line coverage: 95%+
- Branch coverage: 90%+
- All critical paths: 100%

**Estimated Test Count**: ~200 tests
**Estimated LOC**: ~5,000 lines of test code
**Estimated Time To Write**: 2-3 days

---

**End of Testing Strategy**
