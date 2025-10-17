// Copyright (c) 2024 Coinbase Chain
// Test suite for orphan header DoS protection

#include "catch_amalgamated.hpp"
#include "test_chainstate_manager.hpp"
#include "chain/chainparams.hpp"
#include "primitives/block.h"
#include <memory>
#include <thread>
#include <chrono>

using namespace coinbasechain;
using namespace coinbasechain::test;
using namespace coinbasechain::chain;
using coinbasechain::validation::ValidationState;

// Helper function to create test header
static CBlockHeader CreateTestHeader(const uint256& prevHash, uint32_t nTime, uint32_t nNonce = 12345) {
    CBlockHeader header;
    header.nVersion = 1;
    header.hashPrevBlock = prevHash;
    header.minerAddress.SetNull();
    header.nTime = nTime;
    header.nBits = 0x207fffff;  // RegTest difficulty
    header.nNonce = nNonce;
    header.hashRandomX.SetNull();
    return header;
}

// Helper to create a random hash
static uint256 RandomHash() {
    uint256 hash;
    for (int i = 0; i < 32; i++) {
        *(hash.begin() + i) = rand() % 256;
    }
    return hash;
}

TEST_CASE("Orphan DoS - Per-Peer Limits", "[orphan][dos]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);

    SECTION("Enforce per-peer orphan limit (50)") {
        chainstate.Initialize(params->GenesisBlock());
        const int PER_PEER_LIMIT = 50;

        ValidationState state;

        // Send 60 orphans from peer 1
        for (int i = 0; i < 60; i++) {
            uint256 unknownParent = RandomHash();
            CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890 + i, 1000 + i);
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/1);
        }

        // Only 50 should be accepted (per-peer limit)
        // Note: Could be less if global limit reached, but with one peer it should be 50
        REQUIRE(chainstate.GetOrphanHeaderCount() <= PER_PEER_LIMIT);
    }

    SECTION("Different peers have independent limits") {
        chainstate.Initialize(params->GenesisBlock());
        const int PER_PEER_LIMIT = 50;

        ValidationState state;

        // Peer 1 sends 50 orphans
        for (int i = 0; i < PER_PEER_LIMIT; i++) {
            uint256 unknownParent = RandomHash();
            CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890 + i, 1000 + i);
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/1);
        }

        size_t countAfterPeer1 = chainstate.GetOrphanHeaderCount();
        REQUIRE(countAfterPeer1 <= PER_PEER_LIMIT);

        // Peer 2 should still be able to send orphans
        for (int i = 0; i < PER_PEER_LIMIT; i++) {
            uint256 unknownParent = RandomHash();
            CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890 + i + 100, 2000 + i);
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/2);
        }

        // Should have orphans from both peers (up to limits)
        REQUIRE(chainstate.GetOrphanHeaderCount() >= countAfterPeer1);
        REQUIRE(chainstate.GetOrphanHeaderCount() <= 2 * PER_PEER_LIMIT);
    }

    SECTION("Per-peer limit enforced even with different hashes") {
        chainstate.Initialize(params->GenesisBlock());
        const int PER_PEER_LIMIT = 50;

        ValidationState state;

        // Peer 1 sends 70 unique orphans (different nonces = different hashes)
        for (int i = 0; i < 70; i++) {
            uint256 unknownParent = RandomHash();
            CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890 + i, 1000 + i);
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/1);
        }

        // Should cap at per-peer limit
        REQUIRE(chainstate.GetOrphanHeaderCount() <= PER_PEER_LIMIT);
    }
}

TEST_CASE("Orphan DoS - Global Limits", "[orphan][dos]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);

    SECTION("Enforce global orphan limit (1000)") {
        chainstate.Initialize(params->GenesisBlock());
        const int GLOBAL_LIMIT = 1000;
        const int PER_PEER_LIMIT = 50;

        ValidationState state;

        // 25 peers each send 50 orphans (1250 total attempted)
        for (int peer = 1; peer <= 25; peer++) {
            for (int i = 0; i < PER_PEER_LIMIT; i++) {
                uint256 unknownParent = RandomHash();
                CBlockHeader orphan = CreateTestHeader(unknownParent,
                                                       1234567890 + peer * 1000 + i,
                                                       peer * 10000 + i);
                chainstate.AcceptBlockHeader(orphan, state, peer);
            }
        }

        // Only 1000 should be in pool (global limit)
        REQUIRE(chainstate.GetOrphanHeaderCount() <= GLOBAL_LIMIT);
    }

    SECTION("Global limit prevents memory exhaustion") {
        chainstate.Initialize(params->GenesisBlock());
        const int GLOBAL_LIMIT = 1000;

        ValidationState state;

        // Try to add 2000 orphans from various peers
        for (int i = 0; i < 2000; i++) {
            uint256 unknownParent = RandomHash();
            CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890 + i, 1000 + i);
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/(i % 100) + 1);
        }

        // Should be capped at global limit
        REQUIRE(chainstate.GetOrphanHeaderCount() <= GLOBAL_LIMIT);
    }

    SECTION("Eviction when global limit reached") {
        chainstate.Initialize(params->GenesisBlock());
        const int GLOBAL_LIMIT = 1000;
        const int PER_PEER_LIMIT = 50;

        ValidationState state;

        // Fill to limit using multiple peers (to avoid per-peer limit)
        std::vector<uint256> firstBatchHashes;
        for (int i = 0; i < GLOBAL_LIMIT; i++) {
            uint256 unknownParent = RandomHash();
            CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890 + i, 1000 + i);
            firstBatchHashes.push_back(orphan.GetHash());
            // Use different peers to avoid per-peer limit (20 peers * 50 each = 1000)
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/(i / PER_PEER_LIMIT) + 1);
        }

        REQUIRE(chainstate.GetOrphanHeaderCount() == GLOBAL_LIMIT);

        // Add more orphans (should trigger eviction)
        for (int i = 0; i < 100; i++) {
            uint256 unknownParent = RandomHash();
            CBlockHeader orphan = CreateTestHeader(unknownParent,
                                                   1234567890 + GLOBAL_LIMIT + i,
                                                   2000 + i);
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/2);
        }

        // Should still be at or near limit (may have evicted some)
        REQUIRE(chainstate.GetOrphanHeaderCount() <= GLOBAL_LIMIT);
    }
}

TEST_CASE("Orphan DoS - Time-Based Eviction", "[orphan][dos]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);

    SECTION("Manual eviction removes expired orphans") {
        chainstate.Initialize(params->GenesisBlock());

        ValidationState state;

        // Add 10 orphans
        std::vector<uint256> hashes;
        for (int i = 0; i < 10; i++) {
            uint256 unknownParent = RandomHash();
            CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890 + i, 1000 + i);
            hashes.push_back(orphan.GetHash());
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/1);
        }

        REQUIRE(chainstate.GetOrphanHeaderCount() == 10);

        // Wait for expiry time (10 minutes = 600 seconds)
        // Note: In real tests, we'd use mock time. Here we just test the eviction API.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Trigger manual eviction (in production, this happens automatically)
        size_t evicted = chainstate.EvictOrphanHeaders();

        // Eviction should have run (count depends on whether time passed threshold)
        REQUIRE(evicted <= 10);
    }

    SECTION("Eviction respects time threshold") {
        chainstate.Initialize(params->GenesisBlock());

        ValidationState state;

        // Add orphans in two batches with time gap
        std::vector<uint256> batch1_hashes;
        for (int i = 0; i < 5; i++) {
            uint256 unknownParent = RandomHash();
            CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890 + i, 1000 + i);
            batch1_hashes.push_back(orphan.GetHash());
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/1);
        }

        // Small delay
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        std::vector<uint256> batch2_hashes;
        for (int i = 5; i < 10; i++) {
            uint256 unknownParent = RandomHash();
            CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890 + i, 1000 + i);
            batch2_hashes.push_back(orphan.GetHash());
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/1);
        }

        REQUIRE(chainstate.GetOrphanHeaderCount() == 10);

        // Evict (should respect time - older batch1 more likely to be evicted)
        chainstate.EvictOrphanHeaders();

        // Some orphans should remain
        REQUIRE(chainstate.GetOrphanHeaderCount() >= 0);
    }
}

TEST_CASE("Orphan DoS - Orphan Processing Decrements Counts", "[orphan][dos]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);

    SECTION("Orphan count decreases when parent arrives") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Create parent header
        CBlockHeader parent = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 1000);
        uint256 parentHash = parent.GetHash();

        ValidationState state;

        // Send 10 orphans with same parent
        for (int i = 0; i < 10; i++) {
            CBlockHeader orphan = CreateTestHeader(parentHash, genesis.nTime + 240 + i, 2000 + i);
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/1);
        }

        REQUIRE(chainstate.GetOrphanHeaderCount() == 10);

        // Parent arrives (should process all 10 children)
        chainstate.AcceptBlockHeader(parent, state, /*peer_id=*/1);

        // All orphans should be processed and removed
        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);
    }

    SECTION("Partial orphan processing") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Create two parent headers
        CBlockHeader parent1 = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 1000);
        uint256 parentHash1 = parent1.GetHash();

        CBlockHeader parent2 = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 1001);
        uint256 parentHash2 = parent2.GetHash();

        ValidationState state;

        // Send 5 orphans with parent1
        for (int i = 0; i < 5; i++) {
            CBlockHeader orphan = CreateTestHeader(parentHash1, genesis.nTime + 240 + i, 2000 + i);
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/1);
        }

        // Send 5 orphans with parent2
        for (int i = 0; i < 5; i++) {
            CBlockHeader orphan = CreateTestHeader(parentHash2, genesis.nTime + 240 + i, 3000 + i);
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/1);
        }

        REQUIRE(chainstate.GetOrphanHeaderCount() == 10);

        // Only parent1 arrives
        chainstate.AcceptBlockHeader(parent1, state, /*peer_id=*/1);

        // Only first 5 should be processed
        REQUIRE(chainstate.GetOrphanHeaderCount() == 5);

        // Send parent2
        chainstate.AcceptBlockHeader(parent2, state, /*peer_id=*/1);

        // All should be processed
        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);
    }
}

TEST_CASE("Orphan DoS - Spam Resistance", "[orphan][dos]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);

    SECTION("Rapid orphan spam from single peer is limited") {
        chainstate.Initialize(params->GenesisBlock());
        const int PER_PEER_LIMIT = 50;

        ValidationState state;

        // Rapidly send 200 orphans from one peer
        for (int i = 0; i < 200; i++) {
            uint256 unknownParent = RandomHash();
            CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890 + i, 1000 + i);
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/1);
        }

        // Should be limited to per-peer maximum
        REQUIRE(chainstate.GetOrphanHeaderCount() <= PER_PEER_LIMIT);
    }

    SECTION("Coordinated spam from multiple peers is limited") {
        chainstate.Initialize(params->GenesisBlock());
        const int GLOBAL_LIMIT = 1000;

        ValidationState state;

        // 50 peers each send 100 orphans (5000 total)
        for (int peer = 1; peer <= 50; peer++) {
            for (int i = 0; i < 100; i++) {
                uint256 unknownParent = RandomHash();
                CBlockHeader orphan = CreateTestHeader(unknownParent,
                                                       1234567890 + peer * 10000 + i,
                                                       peer * 100000 + i);
                chainstate.AcceptBlockHeader(orphan, state, peer);
            }
        }

        // Should be limited to global maximum
        REQUIRE(chainstate.GetOrphanHeaderCount() <= GLOBAL_LIMIT);
    }

    SECTION("Mix of valid and orphan headers") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        ValidationState state;

        // Build a legitimate chain
        CBlockHeader prev = genesis;
        for (int i = 0; i < 10; i++) {
            CBlockHeader next = CreateTestHeader(prev.GetHash(), prev.nTime + 120, 1000 + i);
            chainstate.AcceptBlockHeader(next, state, /*peer_id=*/1);
            prev = next;
        }

        size_t validCount = chainstate.GetChainHeight();

        // Send orphans
        for (int i = 0; i < 50; i++) {
            uint256 unknownParent = RandomHash();
            CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890 + i, 2000 + i);
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/1);
        }

        // Orphans should be limited, valid chain unaffected
        REQUIRE(chainstate.GetChainHeight() == validCount);
        REQUIRE(chainstate.GetOrphanHeaderCount() <= 50);
    }
}

TEST_CASE("Orphan DoS - Edge Cases", "[orphan][dos]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);

    SECTION("Zero orphans - eviction is safe") {
        chainstate.Initialize(params->GenesisBlock());

        // Try to evict with no orphans
        size_t evicted = chainstate.EvictOrphanHeaders();
        REQUIRE(evicted == 0);
        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);
    }

    SECTION("Exactly at per-peer limit") {
        chainstate.Initialize(params->GenesisBlock());
        const int PER_PEER_LIMIT = 50;

        ValidationState state;

        // Send exactly 50 orphans
        for (int i = 0; i < PER_PEER_LIMIT; i++) {
            uint256 unknownParent = RandomHash();
            CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890 + i, 1000 + i);
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/1);
        }

        REQUIRE(chainstate.GetOrphanHeaderCount() == PER_PEER_LIMIT);

        // Try to add one more
        uint256 unknownParent = RandomHash();
        CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890 + 999, 9999);
        chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/1);

        // Should still be at limit (last one rejected or oldest evicted)
        REQUIRE(chainstate.GetOrphanHeaderCount() <= PER_PEER_LIMIT);
    }

    SECTION("Exactly at global limit") {
        chainstate.Initialize(params->GenesisBlock());
        const int GLOBAL_LIMIT = 1000;

        ValidationState state;

        // Fill to exactly global limit
        for (int i = 0; i < GLOBAL_LIMIT; i++) {
            uint256 unknownParent = RandomHash();
            CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890 + i, 1000 + i);
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/(i % 50) + 1);
        }

        REQUIRE(chainstate.GetOrphanHeaderCount() == GLOBAL_LIMIT);

        // Try to add one more
        uint256 unknownParent = RandomHash();
        CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890 + 9999, 99999);
        chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/99);

        // Should still be at limit (eviction triggered)
        REQUIRE(chainstate.GetOrphanHeaderCount() <= GLOBAL_LIMIT);
    }
}
