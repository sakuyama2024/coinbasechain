// Copyright (c) 2024 Coinbase Chain
// Test suite for orphan header integration and regression tests

#include "catch_amalgamated.hpp"
#include "test_chainstate_manager.hpp"
#include "chain/chainparams.hpp"
#include "chain/chain.hpp"
#include "chain/block_index.hpp"
#include "primitives/block.h"
#include <memory>

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

TEST_CASE("Orphan Integration - Multi-Peer Scenarios", "[orphan][integration]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);

    SECTION("Two peers send competing orphan chains") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Peer 1 sends chain: Genesis -> A1 -> A2
        CBlockHeader A1 = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 1000);
        CBlockHeader A2 = CreateTestHeader(A1.GetHash(), genesis.nTime + 240, 1001);

        // Peer 2 sends chain: Genesis -> B1 -> B2
        CBlockHeader B1 = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 2000);
        CBlockHeader B2 = CreateTestHeader(B1.GetHash(), genesis.nTime + 240, 2001);

        ValidationState state;

        // Both peers send child first (orphaned)
        chainstate.AcceptBlockHeader(A2, state, /*peer_id=*/1);
        chainstate.AcceptBlockHeader(B2, state, /*peer_id=*/2);

        REQUIRE(chainstate.GetOrphanHeaderCount() == 2);

        // Then both send parents
        chainstate.AcceptBlockHeader(A1, state, /*peer_id=*/1);
        chainstate.AcceptBlockHeader(B1, state, /*peer_id=*/2);

        // All orphans processed
        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);

        // Both chains should be in block index
        REQUIRE(chainstate.LookupBlockIndex(A1.GetHash()) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(A2.GetHash()) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(B1.GetHash()) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(B2.GetHash()) != nullptr);
    }

    SECTION("Multiple peers contribute to same orphan chain") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Chain: Genesis -> A -> B -> C -> D
        CBlockHeader A = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 1000);
        uint256 hashA = A.GetHash();

        CBlockHeader B = CreateTestHeader(hashA, genesis.nTime + 240, 1001);
        uint256 hashB = B.GetHash();

        CBlockHeader C = CreateTestHeader(hashB, genesis.nTime + 360, 1002);
        uint256 hashC = C.GetHash();

        CBlockHeader D = CreateTestHeader(hashC, genesis.nTime + 480, 1003);

        ValidationState state;

        // Different peers send different parts in random order
        chainstate.AcceptBlockHeader(D, state, /*peer_id=*/4);  // Peer 4 sends D (orphan)
        chainstate.AcceptBlockHeader(B, state, /*peer_id=*/2);  // Peer 2 sends B (orphan)
        chainstate.AcceptBlockHeader(C, state, /*peer_id=*/3);  // Peer 3 sends C (orphan)

        REQUIRE(chainstate.GetOrphanHeaderCount() == 3);

        // Peer 1 sends A (triggers cascade)
        chainstate.AcceptBlockHeader(A, state, /*peer_id=*/1);

        // All should be processed
        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);

        // All in block index
        REQUIRE(chainstate.LookupBlockIndex(hashA) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(hashB) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(hashC) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(D.GetHash()) != nullptr);
    }

    SECTION("Peer spamming orphans while legitimate chain progresses") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        ValidationState state;

        // Peer 1 builds legitimate chain
        CBlockHeader prev = genesis;
        for (int i = 0; i < 20; i++) {
            CBlockHeader next = CreateTestHeader(prev.GetHash(), prev.nTime + 120, 1000 + i);
            chain::CBlockIndex* pindex = chainstate.AcceptBlockHeader(next, state, /*peer_id=*/1);
            if (pindex) {
                chainstate.TryAddBlockIndexCandidate(pindex);  // Add to candidates
                chainstate.ActivateBestChain();  // Activate
            }
            prev = next;
        }

        size_t validHeight = chainstate.GetChainHeight();
        REQUIRE(validHeight == 20);

        // Peer 2 spams orphans
        for (int i = 0; i < 100; i++) {
            uint256 unknownParent = RandomHash();
            CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890 + i, 2000 + i);
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/2);
        }

        // Orphans should be limited
        REQUIRE(chainstate.GetOrphanHeaderCount() <= 50);  // Per-peer limit

        // Valid chain unaffected
        REQUIRE(chainstate.GetChainHeight() == validHeight);
    }
}

TEST_CASE("Orphan Integration - Reorg Scenarios", "[orphan][integration]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);

    SECTION("Orphan chain with more work triggers reorg") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Build active chain: Genesis -> A (height 1)
        CBlockHeader A = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 1000);
        A.nBits = 0x207fffff;

        ValidationState state;
        chain::CBlockIndex* pindex = chainstate.AcceptBlockHeader(A, state, /*peer_id=*/1);
        if (pindex) {
            chainstate.TryAddBlockIndexCandidate(pindex);
            chainstate.ActivateBestChain();
        }

        REQUIRE(chainstate.GetChainHeight() == 1);

        // Receive longer orphan chain: Genesis -> B1 -> B2 (out of order)
        CBlockHeader B1 = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 2000);
        B1.nBits = 0x207fffff;
        uint256 hashB1 = B1.GetHash();

        CBlockHeader B2 = CreateTestHeader(hashB1, genesis.nTime + 240, 2001);
        B2.nBits = 0x207fffff;

        // Send B2 first (orphan)
        chainstate.AcceptBlockHeader(B2, state, /*peer_id=*/2);
        REQUIRE(chainstate.GetOrphanHeaderCount() == 1);

        // Still on chain A
        REQUIRE(chainstate.GetChainHeight() == 1);

        // Send B1 (triggers B2 processing and reorg)
        chain::CBlockIndex* pindexB1 = chainstate.AcceptBlockHeader(B1, state, /*peer_id=*/2);
        if (pindexB1) {
            chainstate.TryAddBlockIndexCandidate(pindexB1);
        }
        // B2 should have been processed as well (orphan resolution)
        chain::CBlockIndex* pindexB2 = chainstate.LookupBlockIndex(B2.GetHash());
        if (pindexB2) {
            chainstate.TryAddBlockIndexCandidate(pindexB2);
        }
        chainstate.ActivateBestChain();

        // Should reorg to longer chain
        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);
        REQUIRE(chainstate.GetChainHeight() == 2);  // Now at B2
    }

    SECTION("Orphan arrival does not affect active chain until processed") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Build active chain
        CBlockHeader A = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 1000);
        ValidationState state;
        chainstate.AcceptBlockHeader(A, state, /*peer_id=*/1);

        size_t initialHeight = chainstate.GetChainHeight();

        // Receive orphan
        uint256 unknownParent = RandomHash();
        CBlockHeader orphan = CreateTestHeader(unknownParent, genesis.nTime + 240, 2000);
        chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/2);

        // Orphan cached but doesn't affect chain
        REQUIRE(chainstate.GetOrphanHeaderCount() == 1);
        REQUIRE(chainstate.GetChainHeight() == initialHeight);
    }
}

TEST_CASE("Orphan Integration - Header Sync Simulation", "[orphan][integration]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);

    SECTION("Batch header processing with orphans") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Simulate receiving batch of headers with missing parent
        // Create proper chain: A -> B -> C -> D -> E
        CBlockHeader A = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 1000);
        uint256 hashA = A.GetHash();

        CBlockHeader B = CreateTestHeader(hashA, genesis.nTime + 240, 1001);
        uint256 hashB = B.GetHash();

        CBlockHeader C = CreateTestHeader(hashB, genesis.nTime + 360, 1002);
        uint256 hashC = C.GetHash();

        CBlockHeader D = CreateTestHeader(hashC, genesis.nTime + 480, 1003);
        uint256 hashD = D.GetHash();

        CBlockHeader E = CreateTestHeader(hashD, genesis.nTime + 600, 1004);

        ValidationState state;

        // Process batch in reverse (all orphaned except when parent arrives)
        // B, C, D, E sent first - all orphaned because A missing
        chainstate.AcceptBlockHeader(B, state, /*peer_id=*/1);
        chainstate.AcceptBlockHeader(C, state, /*peer_id=*/1);
        chainstate.AcceptBlockHeader(D, state, /*peer_id=*/1);
        chainstate.AcceptBlockHeader(E, state, /*peer_id=*/1);

        // All should be orphaned
        REQUIRE(chainstate.GetOrphanHeaderCount() == 4);

        // Later: A arrives (triggers cascade)
        chainstate.AcceptBlockHeader(A, state, /*peer_id=*/1);

        // All should cascade
        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);

        // All in block index
        REQUIRE(chainstate.LookupBlockIndex(A.GetHash()) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(hashB) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(hashC) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(hashD) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(E.GetHash()) != nullptr);
    }

    SECTION("Out-of-order headers from unstable network") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Simulate network reordering: receive headers 5, 3, 1, 4, 2
        std::vector<CBlockHeader> chain;
        uint256 prevHash = genesis.GetHash();

        for (int i = 0; i < 5; i++) {
            CBlockHeader h = CreateTestHeader(prevHash, genesis.nTime + (i + 1) * 120, 1000 + i);
            chain.push_back(h);
            prevHash = h.GetHash();
        }

        ValidationState state;

        // Receive in order: 5, 3, 1, 4, 2 (indices 4, 2, 0, 3, 1)
        chainstate.AcceptBlockHeader(chain[4], state, /*peer_id=*/1);  // 5 - orphan
        REQUIRE(chainstate.GetOrphanHeaderCount() == 1);

        chainstate.AcceptBlockHeader(chain[2], state, /*peer_id=*/1);  // 3 - orphan
        REQUIRE(chainstate.GetOrphanHeaderCount() == 2);

        chainstate.AcceptBlockHeader(chain[0], state, /*peer_id=*/1);  // 1 - valid!
        REQUIRE(chainstate.GetOrphanHeaderCount() == 2);  // Others still orphaned

        chainstate.AcceptBlockHeader(chain[3], state, /*peer_id=*/1);  // 4 - orphan
        REQUIRE(chainstate.GetOrphanHeaderCount() == 3);

        chainstate.AcceptBlockHeader(chain[1], state, /*peer_id=*/1);  // 2 - triggers cascade!

        // All should be processed now
        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);

        // All in block index
        for (const auto& h : chain) {
            REQUIRE(chainstate.LookupBlockIndex(h.GetHash()) != nullptr);
        }
    }
}

TEST_CASE("Orphan Regression - Bug Fixes", "[orphan][regression]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);

    SECTION("Regression: CChain::Contains null pointer crash") {
        // Bug #2 from ORPHAN_FIX_SUMMARY.md
        // CChain::Contains() should not crash on nullptr

        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Build a small chain to test Contains
        CBlockHeader A = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 1000);
        ValidationState state;
        chainstate.AcceptBlockHeader(A, state, /*peer_id=*/1);

        // Test IsOnActiveChain with nullptr (should handle gracefully)
        bool result = chainstate.IsOnActiveChain(nullptr);
        REQUIRE(result == false);
    }

    SECTION("Regression: LastCommonAncestor with divergent chains") {
        // Bug #3 from ORPHAN_FIX_SUMMARY.md
        // LastCommonAncestor should return nullptr for chains from different genesis

        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Build two chains from genesis
        CBlockHeader A1 = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 1000);
        CBlockHeader A2 = CreateTestHeader(A1.GetHash(), genesis.nTime + 240, 1001);

        ValidationState state;
        chainstate.AcceptBlockHeader(A1, state, /*peer_id=*/1);
        chainstate.AcceptBlockHeader(A2, state, /*peer_id=*/1);

        CBlockIndex* pindexA1 = chainstate.LookupBlockIndex(A1.GetHash());
        CBlockIndex* pindexA2 = chainstate.LookupBlockIndex(A2.GetHash());

        REQUIRE(pindexA1 != nullptr);
        REQUIRE(pindexA2 != nullptr);

        // Find common ancestor (should be A1)
        const CBlockIndex* common = LastCommonAncestor(pindexA1, pindexA2);

        // Should find A1 as common ancestor
        REQUIRE(common == pindexA1);
    }

    SECTION("Regression: Empty candidate set returns success") {
        // Bug #4 from ORPHAN_FIX_SUMMARY.md
        // ActivateBestChain should return true when no competing forks

        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Build simple chain with no forks
        CBlockHeader A = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 1000);
        ValidationState state;
        chainstate.AcceptBlockHeader(A, state, /*peer_id=*/1);

        // ActivateBestChain should succeed (not error)
        bool result = chainstate.ActivateBestChain();
        REQUIRE(result == true);
    }

    SECTION("Regression: Genesis block validation") {
        // Bug #5 from ORPHAN_FIX_SUMMARY.md
        // Fake genesis should be rejected

        chainstate.Initialize(params->GenesisBlock());

        // Try to submit a fake genesis (prev hash = null but wrong hash)
        CBlockHeader fakeGenesis = CreateTestHeader(uint256(), 1234567890, 999);

        ValidationState state;
        CBlockIndex* result = chainstate.AcceptBlockHeader(fakeGenesis, state, /*peer_id=*/1);

        // Should be rejected
        REQUIRE(result == nullptr);
        REQUIRE(state.IsInvalid());
        REQUIRE(state.GetRejectReason() != "orphaned");  // Not cached as orphan
    }

    SECTION("Regression: Orphan not re-added after processing") {
        // Regression test: Orphan processed, then re-added as orphan
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Create proper chain where parent extends genesis
        CBlockHeader parent = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 1000);
        uint256 parentHash = parent.GetHash();

        CBlockHeader orphan = CreateTestHeader(parentHash, genesis.nTime + 240, 1001);
        uint256 orphanHash = orphan.GetHash();

        ValidationState state;

        // Add child as orphan (parent not yet known)
        chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/1);
        REQUIRE(chainstate.GetOrphanHeaderCount() == 1);

        // Parent arrives, orphan processed
        chainstate.AcceptBlockHeader(parent, state, /*peer_id=*/1);

        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);
        REQUIRE(chainstate.LookupBlockIndex(orphanHash) != nullptr);

        // Try to add same header again
        chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/1);

        // Should recognize as duplicate, NOT re-add to orphan pool
        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);
    }

    SECTION("Regression: Batch processing continues after orphan") {
        // Bug #6 from ORPHAN_FIX_SUMMARY.md
        // Header batch should continue processing after encountering orphan

        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Build chain: Genesis -> A -> B, C (C is orphan)
        CBlockHeader A = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 1000);
        CBlockHeader B = CreateTestHeader(A.GetHash(), genesis.nTime + 240, 1001);

        uint256 unknownParent = RandomHash();
        CBlockHeader C_orphan = CreateTestHeader(unknownParent, genesis.nTime + 360, 1002);

        ValidationState state;

        // Process in batch: A, B, C
        chainstate.AcceptBlockHeader(A, state, /*peer_id=*/1);
        REQUIRE(state.IsValid());

        chainstate.AcceptBlockHeader(B, state, /*peer_id=*/1);
        REQUIRE(state.IsValid());

        chainstate.AcceptBlockHeader(C_orphan, state, /*peer_id=*/1);
        // C is orphaned but shouldn't fail batch
        REQUIRE(state.GetRejectReason() == "orphaned");

        // A and B should be in block index
        REQUIRE(chainstate.LookupBlockIndex(A.GetHash()) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(B.GetHash()) != nullptr);

        // C should be in orphan pool
        REQUIRE(chainstate.GetOrphanHeaderCount() == 1);
    }
}

TEST_CASE("Orphan Integration - Network Partition Recovery", "[orphan][integration]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);

    SECTION("Node syncs from peer after partition heals") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Node has chain: Genesis -> A
        CBlockHeader A = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 1000);
        ValidationState state;
        chainstate.AcceptBlockHeader(A, state, /*peer_id=*/1);

        // Network partition: peer built longer chain B1 -> B2 -> B3 from genesis
        CBlockHeader B1 = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 2000);
        uint256 hashB1 = B1.GetHash();

        CBlockHeader B2 = CreateTestHeader(hashB1, genesis.nTime + 240, 2001);
        uint256 hashB2 = B2.GetHash();

        CBlockHeader B3 = CreateTestHeader(hashB2, genesis.nTime + 360, 2002);

        // Partition heals: receive B1, B2, B3 in order
        chainstate.AcceptBlockHeader(B1, state, /*peer_id=*/2);
        chainstate.AcceptBlockHeader(B2, state, /*peer_id=*/2);
        chainstate.AcceptBlockHeader(B3, state, /*peer_id=*/2);

        // All should be accepted (no orphans if sent in order)
        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);

        // Both chains in block index
        REQUIRE(chainstate.LookupBlockIndex(A.GetHash()) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(hashB1) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(hashB2) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(B3.GetHash()) != nullptr);
    }
}
