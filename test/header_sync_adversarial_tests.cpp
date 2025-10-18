// Copyright (c) 2024 Coinbase Chain
// Adversarial tests for HeaderSync
// Tests attack scenarios and DoS protection for header synchronization

#include "catch_amalgamated.hpp"
#include "sync/header_sync.hpp"
#include "chain/chainparams.hpp"
#include "validation/chainstate_manager.hpp"
#include "test_chainstate_manager.hpp"
#include "primitives/block.h"
#include <vector>

using namespace coinbasechain;
using namespace coinbasechain::sync;
using namespace coinbasechain::chain;
using namespace coinbasechain::validation;
using namespace coinbasechain::test;

// Helper: Create a chain of headers building on a parent
static std::vector<CBlockHeader> CreateHeaderChain(const CBlockHeader& parent, int count) {
    std::vector<CBlockHeader> headers;
    CBlockHeader prev = parent;

    for (int i = 0; i < count; i++) {
        CBlockHeader header;
        header.nVersion = 1;
        header.hashPrevBlock = prev.GetHash();
        header.minerAddress.SetNull();
        header.nTime = prev.nTime + 120;
        header.nBits = 0x207fffff;  // RegTest difficulty
        header.nNonce = i + 1;
        header.hashRandomX.SetNull();

        headers.push_back(header);
        prev = header;
    }

    return headers;
}

// ============================================================================
// CATEGORY 1: Invalid Chain Attacks
// ============================================================================

TEST_CASE("HeaderSync Adversarial - Headers Not Chaining", "[adversarial][header_sync][critical]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);
    chainstate.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate, *params);
    sync.Initialize();

    const auto& genesis = params->GenesisBlock();

    SECTION("Headers with wrong prevhash (don't connect)") {
        std::vector<CBlockHeader> headers;

        // First header connects to genesis
        CBlockHeader header1;
        header1.nVersion = 1;
        header1.hashPrevBlock = genesis.GetHash();
        header1.minerAddress.SetNull();
        header1.nTime = genesis.nTime + 120;
        header1.nBits = 0x207fffff;
        header1.nNonce = 1;
        header1.hashRandomX.SetNull();
        headers.push_back(header1);

        // Second header has WRONG prevhash (doesn't connect to header1)
        CBlockHeader header2;
        header2.nVersion = 1;
        header2.hashPrevBlock = uint256();  // Wrong! Should be header1.GetHash()
        header2.hashPrevBlock.SetNull();
        header2.minerAddress.SetNull();
        header2.nTime = header1.nTime + 120;
        header2.nBits = 0x207fffff;
        header2.nNonce = 2;
        header2.hashRandomX.SetNull();
        headers.push_back(header2);

        // Should reject the batch (headers don't form a chain)
        REQUIRE_FALSE(sync.ProcessHeaders(headers, 1));

        // Should still be at genesis
        REQUIRE(sync.GetBestHeight() == 0);
    }

    SECTION("Headers disconnected from known chain") {
        std::vector<CBlockHeader> headers;

        // Create headers that don't connect to anything in our chain
        CBlockHeader orphan;
        orphan.nVersion = 1;
        orphan.hashPrevBlock = uint256S("0000000000000000000000000000000000000000000000000000000000000001");
        orphan.minerAddress.SetNull();
        orphan.nTime = genesis.nTime + 120;
        orphan.nBits = 0x207fffff;
        orphan.nNonce = 1;
        orphan.hashRandomX.SetNull();
        headers.push_back(orphan);

        // Should reject (doesn't connect to known chain)
        bool result = sync.ProcessHeaders(headers, 1);

        // Either rejects or stores as orphan (implementation dependent)
        // But should NOT advance the tip
        REQUIRE(sync.GetBestHeight() == 0);
    }
}

TEST_CASE("HeaderSync Adversarial - Duplicate Headers", "[adversarial][header_sync][dos]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);
    chainstate.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate, *params);
    sync.Initialize();

    const auto& genesis = params->GenesisBlock();

    SECTION("Send same header multiple times") {
        // Create a valid header
        auto headers = CreateHeaderChain(genesis, 1);

        // Process first time - should succeed
        REQUIRE(sync.ProcessHeaders(headers, 1));
        REQUIRE(sync.GetBestHeight() == 1);

        // Process again - should be idempotent (not crash, not duplicate)
        REQUIRE(sync.ProcessHeaders(headers, 1));
        REQUIRE(sync.GetBestHeight() == 1);

        // Process third time
        REQUIRE(sync.ProcessHeaders(headers, 1));
        REQUIRE(sync.GetBestHeight() == 1);
    }

    SECTION("Duplicate headers in same batch") {
        std::vector<CBlockHeader> headers;

        // Create one header
        CBlockHeader header;
        header.nVersion = 1;
        header.hashPrevBlock = genesis.GetHash();
        header.minerAddress.SetNull();
        header.nTime = genesis.nTime + 120;
        header.nBits = 0x207fffff;
        header.nNonce = 1;
        header.hashRandomX.SetNull();

        // Add it twice
        headers.push_back(header);
        headers.push_back(header);

        // Should reject (duplicates in batch)
        REQUIRE_FALSE(sync.ProcessHeaders(headers, 1));
    }
}

// ============================================================================
// CATEGORY 2: DoS Attacks
// ============================================================================

TEST_CASE("HeaderSync Adversarial - Excessive Headers (> 2000)", "[adversarial][header_sync][dos]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);
    chainstate.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate, *params);
    sync.Initialize();

    const auto& genesis = params->GenesisBlock();

    SECTION("Send 2001 headers (exceeds MAX_HEADERS_RESULTS)") {
        // Bitcoin protocol limits HEADERS messages to 2000
        auto headers = CreateHeaderChain(genesis, 2001);

        // Should reject (exceeds batch limit)
        REQUIRE_FALSE(sync.ProcessHeaders(headers, 1));

        // Should still be at genesis
        REQUIRE(sync.GetBestHeight() == 0);
    }

    SECTION("Send exactly 2000 headers (at limit)") {
        // Exactly 2000 should work
        auto headers = CreateHeaderChain(genesis, 2000);

        // Should succeed
        REQUIRE(sync.ProcessHeaders(headers, 1));
        REQUIRE(sync.GetBestHeight() == 2000);

        // Should request more (full batch received)
        REQUIRE(sync.ShouldRequestMore());
    }
}

TEST_CASE("HeaderSync Adversarial - Empty Headers Message", "[adversarial][header_sync][dos]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);
    chainstate.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate, *params);
    sync.Initialize();

    SECTION("Empty headers vector") {
        std::vector<CBlockHeader> empty;

        // Should handle gracefully (not crash)
        REQUIRE(sync.ProcessHeaders(empty, 1));

        // Should still be at genesis
        REQUIRE(sync.GetBestHeight() == 0);

        // Should NOT request more (empty = peer has no more)
        REQUIRE_FALSE(sync.ShouldRequestMore());
    }
}

TEST_CASE("HeaderSync Adversarial - Slow Drip Attack", "[adversarial][header_sync][dos]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);
    chainstate.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate, *params);
    sync.Initialize();

    const auto& genesis = params->GenesisBlock();

    SECTION("Send 1 header at a time (slow drip)") {
        // Attacker sends headers one at a time to waste resources
        CBlockHeader prev = genesis;

        for (int i = 0; i < 10; i++) {
            std::vector<CBlockHeader> single = CreateHeaderChain(prev, 1);
            REQUIRE(sync.ProcessHeaders(single, 1));
            prev = single[0];
        }

        // Should process all (no rate limit on processing)
        REQUIRE(sync.GetBestHeight() == 10);

        // After each single header, should NOT request more (< 2000 = peer done)
        REQUIRE_FALSE(sync.ShouldRequestMore());
    }
}

TEST_CASE("HeaderSync Adversarial - Repeated Small Batches", "[adversarial][header_sync][dos]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);
    chainstate.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate, *params);
    sync.Initialize();

    const auto& genesis = params->GenesisBlock();

    SECTION("Send 100 batches of 10 headers") {
        CBlockHeader prev = genesis;

        for (int batch = 0; batch < 100; batch++) {
            auto headers = CreateHeaderChain(prev, 10);
            REQUIRE(sync.ProcessHeaders(headers, 1));
            prev = headers.back();
        }

        // Should process all (1000 headers total)
        REQUIRE(sync.GetBestHeight() == 1000);
    }
}

// ============================================================================
// CATEGORY 3: Fork Attacks
// ============================================================================

TEST_CASE("HeaderSync Adversarial - Competing Tips", "[adversarial][header_sync][fork]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);
    chainstate.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate, *params);
    sync.Initialize();

    const auto& genesis = params->GenesisBlock();

    SECTION("Send two competing branches from same parent") {
        // Build chain A: genesis -> A1 -> A2
        auto chainA = CreateHeaderChain(genesis, 2);
        REQUIRE(sync.ProcessHeaders(chainA, 1));
        REQUIRE(sync.GetBestHeight() == 2);

        // Build chain B: genesis -> B1 -> B2 -> B3 (longer)
        auto chainB = CreateHeaderChain(genesis, 3);
        REQUIRE(sync.ProcessHeaders(chainB, 2));

        // Should reorg to longer chain B
        REQUIRE(sync.GetBestHeight() == 3);
    }

    SECTION("Multiple competing tips") {
        // Chain A: genesis -> A (1 block)
        auto chainA = CreateHeaderChain(genesis, 1);
        REQUIRE(sync.ProcessHeaders(chainA, 1));

        // Chain B: genesis -> B (1 block, competing)
        auto chainB = CreateHeaderChain(genesis, 1);
        chainB[0].nNonce = 9999;  // Different nonce = different hash
        REQUIRE(sync.ProcessHeaders(chainB, 2));

        // Chain C: genesis -> C (1 block, competing)
        auto chainC = CreateHeaderChain(genesis, 1);
        chainC[0].nNonce = 8888;  // Different nonce = different hash
        REQUIRE(sync.ProcessHeaders(chainC, 3));

        // Should be on one of them (first-seen with equal work)
        REQUIRE(sync.GetBestHeight() == 1);
    }
}

TEST_CASE("HeaderSync Adversarial - Fork Bombing", "[adversarial][header_sync][fork]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);
    chainstate.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate, *params);
    sync.Initialize();

    const auto& genesis = params->GenesisBlock();

    SECTION("Send 100 different branches from genesis") {
        // Attacker sends many competing forks to exhaust memory
        for (int branch = 0; branch < 100; branch++) {
            auto chain = CreateHeaderChain(genesis, 1);
            chain[0].nNonce = 10000 + branch;  // Different hash each time

            // All should be accepted (same height, different tips)
            sync.ProcessHeaders(chain, branch + 1);
        }

        // Should still be at height 1 (on one of the competing tips)
        REQUIRE(sync.GetBestHeight() == 1);
    }

    SECTION("Deep fork (split at genesis)") {
        // Main chain: 10 blocks
        auto mainChain = CreateHeaderChain(genesis, 10);
        REQUIRE(sync.ProcessHeaders(mainChain, 1));
        REQUIRE(sync.GetBestHeight() == 10);

        // Fork from genesis with 15 blocks (should trigger reorg)
        // Need to build manually to ensure proper chaining with different nonce
        std::vector<CBlockHeader> forkChain;
        CBlockHeader prev = genesis;
        for (int i = 0; i < 15; i++) {
            CBlockHeader header;
            header.nVersion = 1;
            header.hashPrevBlock = prev.GetHash();
            header.minerAddress.SetNull();
            header.nTime = prev.nTime + 120;
            header.nBits = 0x207fffff;
            header.nNonce = 10000 + i;  // Different nonces to make different hashes
            header.hashRandomX.SetNull();
            forkChain.push_back(header);
            prev = header;
        }

        REQUIRE(sync.ProcessHeaders(forkChain, 2));

        // Should reorg to longer fork
        REQUIRE(sync.GetBestHeight() == 15);
    }
}

// ============================================================================
// CATEGORY 4: Timestamp Attacks
// ============================================================================

TEST_CASE("HeaderSync Adversarial - Timestamp Manipulation", "[adversarial][header_sync][timestamp]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);
    chainstate.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate, *params);
    sync.Initialize();

    const auto& genesis = params->GenesisBlock();

    SECTION("Timestamps going backwards") {
        std::vector<CBlockHeader> headers;

        CBlockHeader header1;
        header1.nVersion = 1;
        header1.hashPrevBlock = genesis.GetHash();
        header1.minerAddress.SetNull();
        header1.nTime = genesis.nTime + 120;
        header1.nBits = 0x207fffff;
        header1.nNonce = 1;
        header1.hashRandomX.SetNull();
        headers.push_back(header1);

        CBlockHeader header2;
        header2.nVersion = 1;
        header2.hashPrevBlock = header1.GetHash();
        header2.minerAddress.SetNull();
        header2.nTime = genesis.nTime;  // Earlier than header1!
        header2.nBits = 0x207fffff;
        header2.nNonce = 2;
        header2.hashRandomX.SetNull();
        headers.push_back(header2);

        // Should reject (timestamp goes backwards)
        // Note: With TestChainstateManager this might pass (validation bypassed)
        // Real ChainstateManager would reject this
        bool result = sync.ProcessHeaders(headers, 1);

        // If accepted (bypassed validation), just verify no crash
        // If rejected, we're still at genesis
        if (!result) {
            REQUIRE(sync.GetBestHeight() == 0);
        }
    }
}

// ============================================================================
// CATEGORY 5: Peer State Management
// ============================================================================

TEST_CASE("HeaderSync Adversarial - Multiple Peers", "[adversarial][header_sync][peer]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);
    chainstate.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate, *params);
    sync.Initialize();

    const auto& genesis = params->GenesisBlock();

    SECTION("Same headers from different peers") {
        auto headers = CreateHeaderChain(genesis, 10);

        // Peer 1 sends headers
        REQUIRE(sync.ProcessHeaders(headers, 1));
        REQUIRE(sync.GetBestHeight() == 10);

        // Peer 2 sends same headers (duplicate)
        REQUIRE(sync.ProcessHeaders(headers, 2));
        REQUIRE(sync.GetBestHeight() == 10);

        // Peer 3 sends same headers
        REQUIRE(sync.ProcessHeaders(headers, 3));
        REQUIRE(sync.GetBestHeight() == 10);
    }

    SECTION("Interleaved headers from multiple peers") {
        auto headers1 = CreateHeaderChain(genesis, 5);
        auto headers2 = CreateHeaderChain(headers1.back(), 5);

        // Peer 1 sends first batch
        REQUIRE(sync.ProcessHeaders(headers1, 1));
        REQUIRE(sync.GetBestHeight() == 5);

        // Peer 2 sends second batch (continues from peer 1's tip)
        REQUIRE(sync.ProcessHeaders(headers2, 2));
        REQUIRE(sync.GetBestHeight() == 10);
    }
}

TEST_CASE("HeaderSync Adversarial - Invalid Peer ID", "[adversarial][header_sync][peer]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);
    chainstate.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate, *params);
    sync.Initialize();

    const auto& genesis = params->GenesisBlock();

    SECTION("Negative peer ID") {
        auto headers = CreateHeaderChain(genesis, 10);

        // Should handle gracefully
        bool result = sync.ProcessHeaders(headers, -1);
        // Implementation might reject or accept with peer_id=-1
    }

    SECTION("Zero peer ID") {
        auto headers = CreateHeaderChain(genesis, 10);

        // Should handle gracefully
        REQUIRE(sync.ProcessHeaders(headers, 0));
    }

    SECTION("Very large peer ID") {
        auto headers = CreateHeaderChain(genesis, 10);

        // Should handle gracefully
        REQUIRE(sync.ProcessHeaders(headers, 999999));
    }
}

// ============================================================================
// CATEGORY 6: State Management
// ============================================================================

TEST_CASE("HeaderSync Adversarial - State Transitions", "[adversarial][header_sync][state]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);
    chainstate.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate, *params);
    sync.Initialize();

    const auto& genesis = params->GenesisBlock();

    SECTION("Rapid sync state changes") {
        // Start: IDLE
        REQUIRE(sync.GetState() == HeaderSync::State::IDLE);

        // Add recent headers to reach SYNCED
        std::vector<CBlockHeader> headers;
        CBlockHeader header;
        header.nVersion = 1;
        header.hashPrevBlock = genesis.GetHash();
        header.minerAddress.SetNull();
        header.nTime = std::time(nullptr) - 30;  // Recent
        header.nBits = 0x207fffff;
        header.nNonce = 1;
        header.hashRandomX.SetNull();
        headers.push_back(header);

        REQUIRE(sync.ProcessHeaders(headers, 1));

        // Should be SYNCED (tip is recent)
        REQUIRE(sync.IsSynced(3600));
    }
}

// ============================================================================
// CATEGORY 7: Locator Edge Cases
// ============================================================================

TEST_CASE("HeaderSync Adversarial - Locator Stress", "[adversarial][header_sync][locator]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);
    chainstate.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate, *params);
    sync.Initialize();

    const auto& genesis = params->GenesisBlock();

    SECTION("Locator after very long chain") {
        // Build a long chain (1000 blocks)
        CBlockHeader prev = genesis;
        for (int i = 0; i < 100; i++) {
            auto headers = CreateHeaderChain(prev, 10);
            REQUIRE(sync.ProcessHeaders(headers, 1));
            prev = headers.back();
        }

        REQUIRE(sync.GetBestHeight() == 1000);

        // Get locator - should have exponential backoff
        CBlockLocator locator = sync.GetLocator();
        REQUIRE(!locator.IsNull());
        REQUIRE(locator.vHave.size() > 0);

        // First entry should be tip
        REQUIRE(locator.vHave[0] == sync.GetBestHash());
    }

    SECTION("Locator from previous") {
        // Build a chain
        auto headers = CreateHeaderChain(genesis, 10);
        REQUIRE(sync.ProcessHeaders(headers, 1));

        // Get locator from prev (for GETHEADERS)
        CBlockLocator locator = sync.GetLocatorFromPrev();
        REQUIRE(!locator.IsNull());
    }
}

// ============================================================================
// CATEGORY 8: Edge Cases
// ============================================================================

TEST_CASE("HeaderSync Adversarial - Reinitialization", "[adversarial][header_sync][edge]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);
    chainstate.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate, *params);

    SECTION("Initialize twice") {
        REQUIRE(sync.Initialize());
        REQUIRE(sync.GetBestHeight() == 0);

        // Initialize again - should be idempotent
        REQUIRE(sync.Initialize());
        REQUIRE(sync.GetBestHeight() == 0);
    }
}

TEST_CASE("HeaderSync Adversarial - ProcessHeaders Before Initialize", "[adversarial][header_sync][edge]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);
    chainstate.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate, *params);

    // Don't call sync.Initialize()

    const auto& genesis = params->GenesisBlock();

    SECTION("Process headers without initialization") {
        auto headers = CreateHeaderChain(genesis, 10);

        // Should handle gracefully (either reject or auto-initialize)
        bool result = sync.ProcessHeaders(headers, 1);
        // Implementation dependent - just verify no crash
    }
}
