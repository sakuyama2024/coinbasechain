// Copyright (c) 2024 Coinbase Chain
// Test suite for orphan header basic functionality

#include "catch_amalgamated.hpp"
#include "test_chainstate_manager.hpp"
#include "chain/chainparams.hpp"
#include "chain/block.hpp"
#include "chain/sha256.hpp"
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

TEST_CASE("Orphan Headers - Basic Detection", "[orphan][basic]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);

    SECTION("Initialize with genesis") {
        REQUIRE(chainstate.Initialize(params->GenesisBlock()));
        REQUIRE(chainstate.GetChainHeight() == 0);
    }

    SECTION("Detect orphan when parent missing") {
        chainstate.Initialize(params->GenesisBlock());

        // Create header with unknown parent
        uint256 unknownParent = RandomHash();
        CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890);

        ValidationState state;
        chain::CBlockIndex* result = chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/1);

        // Should return nullptr and cache as orphan
        REQUIRE(result == nullptr);
        REQUIRE(state.GetRejectReason() == "orphaned");
        REQUIRE(chainstate.GetOrphanHeaderCount() == 1);
    }

    SECTION("Accept non-orphan when parent exists") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Create header extending genesis (parent exists)
        CBlockHeader valid = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120);

        ValidationState state;
        chain::CBlockIndex* result = chainstate.AcceptBlockHeader(valid, state, /*peer_id=*/1);

        // Should succeed
        REQUIRE(result != nullptr);
        REQUIRE(state.IsValid());
        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);  // Not orphaned
    }

    SECTION("Check orphan not added to block index") {
        chainstate.Initialize(params->GenesisBlock());

        uint256 unknownParent = RandomHash();
        CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890);
        uint256 orphanHash = orphan.GetHash();

        ValidationState state;
        chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/1);

        // Orphan should be in orphan pool, NOT in block index
        REQUIRE(chainstate.LookupBlockIndex(orphanHash) == nullptr);
        REQUIRE(chainstate.GetOrphanHeaderCount() == 1);
    }

    SECTION("Genesis block not cached as orphan") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Try to re-add genesis
        ValidationState state;
        chain::CBlockIndex* result = chainstate.AcceptBlockHeader(genesis, state, /*peer_id=*/1);

        // Should return existing genesis (duplicate detection at line 48 of chainstate_manager.cpp)
        // and NOT be cached as orphan
        REQUIRE(result != nullptr);  // Returns existing pindex
        REQUIRE(state.IsValid());     // Not an error, just a duplicate
        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);
    }
}

TEST_CASE("Orphan Headers - Orphan Processing", "[orphan][basic]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);

    SECTION("Process single orphan when parent arrives") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Step 1: Create and send child header (orphan, parent unknown)
        CBlockHeader parentHeader = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 1000);
        uint256 parentHash = parentHeader.GetHash();

        CBlockHeader childHeader = CreateTestHeader(parentHash, genesis.nTime + 240, 1001);

        // Send child first (becomes orphan)
        ValidationState childState;
        chainstate.AcceptBlockHeader(childHeader, childState, /*peer_id=*/1);
        REQUIRE(chainstate.GetOrphanHeaderCount() == 1);
        REQUIRE(childState.GetRejectReason() == "orphaned");

        // Step 2: Send parent (should trigger child processing)
        ValidationState parentState;
        chain::CBlockIndex* parentResult = chainstate.AcceptBlockHeader(parentHeader, parentState, /*peer_id=*/1);

        // Parent accepted
        REQUIRE(parentResult != nullptr);
        REQUIRE(parentState.IsValid());

        // Orphan should be automatically processed and removed from pool
        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);

        // Child should now be in block index
        REQUIRE(chainstate.LookupBlockIndex(childHeader.GetHash()) != nullptr);
    }

    SECTION("Process linear orphan chain") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Create chain: Genesis -> A -> B -> C
        CBlockHeader headerA = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 1000);
        uint256 hashA = headerA.GetHash();

        CBlockHeader headerB = CreateTestHeader(hashA, genesis.nTime + 240, 1001);
        uint256 hashB = headerB.GetHash();

        CBlockHeader headerC = CreateTestHeader(hashB, genesis.nTime + 360, 1002);

        // Send in reverse: C, B, A (all become orphans except A)
        ValidationState state;

        // Send C (orphan - parent B missing)
        chainstate.AcceptBlockHeader(headerC, state, /*peer_id=*/1);
        REQUIRE(chainstate.GetOrphanHeaderCount() == 1);

        // Send B (orphan - parent A missing)
        chainstate.AcceptBlockHeader(headerB, state, /*peer_id=*/1);
        REQUIRE(chainstate.GetOrphanHeaderCount() == 2);

        // Send A (parent = genesis, exists!)
        chainstate.AcceptBlockHeader(headerA, state, /*peer_id=*/1);

        // All orphans should cascade: A accepted -> triggers B -> B triggers C
        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);
        REQUIRE(chainstate.LookupBlockIndex(hashA) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(hashB) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(headerC.GetHash()) != nullptr);
    }

    SECTION("Process branching orphan chain") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Create tree:
        //     Genesis -> A -> B
        //                  \-> C
        //                  \-> D
        CBlockHeader headerA = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 1000);
        uint256 hashA = headerA.GetHash();

        CBlockHeader headerB = CreateTestHeader(hashA, genesis.nTime + 240, 1001);
        CBlockHeader headerC = CreateTestHeader(hashA, genesis.nTime + 240, 1002);
        CBlockHeader headerD = CreateTestHeader(hashA, genesis.nTime + 240, 1003);

        ValidationState state;

        // Send B, C, D (all orphaned - parent A missing)
        chainstate.AcceptBlockHeader(headerB, state, /*peer_id=*/1);
        chainstate.AcceptBlockHeader(headerC, state, /*peer_id=*/1);
        chainstate.AcceptBlockHeader(headerD, state, /*peer_id=*/1);
        REQUIRE(chainstate.GetOrphanHeaderCount() == 3);

        // Send parent A (should trigger all 3 children)
        chainstate.AcceptBlockHeader(headerA, state, /*peer_id=*/1);

        // All 3 children should be processed
        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);
        REQUIRE(chainstate.LookupBlockIndex(headerA.GetHash()) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(headerB.GetHash()) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(headerC.GetHash()) != nullptr);
        REQUIRE(chainstate.LookupBlockIndex(headerD.GetHash()) != nullptr);
    }

    SECTION("Deep orphan chain (20 levels)") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        const int DEPTH = 20;
        std::vector<CBlockHeader> headers;
        uint256 prevHash = genesis.GetHash();
        uint32_t baseTime = genesis.nTime;

        // Build chain of headers
        for (int i = 0; i < DEPTH; i++) {
            CBlockHeader h = CreateTestHeader(prevHash, baseTime + (i + 1) * 120, 1000 + i);
            headers.push_back(h);
            prevHash = h.GetHash();
        }

        ValidationState state;

        // Send in REVERSE order (all become orphans)
        for (int i = DEPTH - 1; i >= 1; i--) {
            chainstate.AcceptBlockHeader(headers[i], state, /*peer_id=*/1);
        }
        REQUIRE(chainstate.GetOrphanHeaderCount() == DEPTH - 1);

        // Send the first header (extends genesis)
        // Should trigger cascade processing of all orphans
        chainstate.AcceptBlockHeader(headers[0], state, /*peer_id=*/1);

        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);

        // All should be in block index
        for (const auto& h : headers) {
            REQUIRE(chainstate.LookupBlockIndex(h.GetHash()) != nullptr);
        }
    }
}

TEST_CASE("Orphan Headers - Duplicate Detection", "[orphan][basic]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);

    SECTION("Same orphan sent twice is ignored") {
        chainstate.Initialize(params->GenesisBlock());

        uint256 unknownParent = RandomHash();
        CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890);

        ValidationState state1, state2;

        // Send once
        chainstate.AcceptBlockHeader(orphan, state1, /*peer_id=*/1);
        REQUIRE(chainstate.GetOrphanHeaderCount() == 1);

        // Send again (duplicate)
        chainstate.AcceptBlockHeader(orphan, state2, /*peer_id=*/1);

        // Should not add duplicate
        REQUIRE(chainstate.GetOrphanHeaderCount() == 1);
    }

    SECTION("Same orphan from different peers") {
        chainstate.Initialize(params->GenesisBlock());

        uint256 unknownParent = RandomHash();
        CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890);

        ValidationState state1, state2;

        // Peer 1 sends it
        chainstate.AcceptBlockHeader(orphan, state1, /*peer_id=*/1);
        REQUIRE(chainstate.GetOrphanHeaderCount() == 1);

        // Peer 2 sends same header
        chainstate.AcceptBlockHeader(orphan, state2, /*peer_id=*/2);

        // Only stored once
        REQUIRE(chainstate.GetOrphanHeaderCount() == 1);
    }

    SECTION("Orphan not re-added after processing") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis = params->GenesisBlock();

        // Create parent and child
        CBlockHeader parent = CreateTestHeader(genesis.GetHash(), genesis.nTime + 120, 1000);
        uint256 parentHash = parent.GetHash();

        CBlockHeader child = CreateTestHeader(parentHash, genesis.nTime + 240, 1001);

        ValidationState state;

        // Add as orphan
        chainstate.AcceptBlockHeader(child, state, /*peer_id=*/1);
        REQUIRE(chainstate.GetOrphanHeaderCount() == 1);

        // Parent arrives, child processed
        chainstate.AcceptBlockHeader(parent, state, /*peer_id=*/1);
        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);
        REQUIRE(chainstate.LookupBlockIndex(child.GetHash()) != nullptr);

        // Try to add same header again
        chainstate.AcceptBlockHeader(child, state, /*peer_id=*/1);

        // Should recognize as duplicate, NOT re-add to orphan pool
        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);
    }
}

TEST_CASE("Orphan Headers - Empty State", "[orphan][basic]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);

    SECTION("Fresh chainstate has no orphans") {
        chainstate.Initialize(params->GenesisBlock());
        REQUIRE(chainstate.GetOrphanHeaderCount() == 0);
    }

    SECTION("Orphan count accurate after additions") {
        chainstate.Initialize(params->GenesisBlock());

        ValidationState state;

        // Add 5 orphans
        for (int i = 0; i < 5; i++) {
            uint256 unknownParent = RandomHash();
            CBlockHeader orphan = CreateTestHeader(unknownParent, 1234567890 + i, 1000 + i);
            chainstate.AcceptBlockHeader(orphan, state, /*peer_id=*/1);
        }

        REQUIRE(chainstate.GetOrphanHeaderCount() == 5);
    }
}
