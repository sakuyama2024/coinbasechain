// Copyright (c) 2024 Coinbase Chain
// Unit tests for invalidateblock functionality

#include <catch_amalgamated.hpp>
#include "chain/chainstate_manager.hpp"
#include "chain/chainparams.hpp"
#include "chain/block.hpp"
#include "chain/pow.hpp"
#include "chain/randomx_pow.hpp"
#include "chain/time.hpp"
#include <memory>
#include <iostream>

using namespace coinbasechain;
using namespace coinbasechain::validation;
using namespace coinbasechain::chain;

// Test fixture for invalidateblock tests
class InvalidateBlockTestFixture {
public:
    InvalidateBlockTestFixture()
        : params(ChainParams::CreateRegTest())
    {
        // Initialize RandomX for mining
        crypto::InitRandomX();

        // Initialize with genesis block
        CBlockHeader genesis = params->GenesisBlock();
        chainstate.Initialize(genesis);

        genesis_hash = genesis.GetHash();
    }

    // Helper: Mine a block on top of current tip
    uint256 MineBlock() {
        auto* tip = chainstate.GetTip();
        REQUIRE(tip != nullptr);

        CBlockHeader header;
        header.nVersion = 1;
        header.hashPrevBlock = tip->GetBlockHash();
        header.minerAddress = uint160(); // Zero address is fine for tests
        header.nTime = tip->nTime + 120; // 2 minutes later
        header.nBits = consensus::GetNextWorkRequired(tip, *params);
        header.nNonce = 0;

        // Find valid nonce (easy difficulty in regtest)
        uint256 randomx_hash;
        while (!consensus::CheckProofOfWork(header, header.nBits, *params,
                                           crypto::POWVerifyMode::MINING, &randomx_hash)) {
            header.nNonce++;
            if (header.nNonce == 0) {
                header.nTime++;
            }
            if (header.nNonce > 100000) {
                FAIL("Failed to mine block within 100000 nonces");
            }
        }
        header.hashRandomX = randomx_hash;

        ValidationState state;
        bool accepted = chainstate.ProcessNewBlockHeader(header, state);
        REQUIRE(accepted);

        return header.GetHash();
    }

    // Helper: Get block index by hash
    const CBlockIndex* GetBlockIndex(const uint256& hash) {
        return chainstate.LookupBlockIndex(hash);
    }

    std::unique_ptr<ChainParams> params;
    ChainstateManager chainstate{*params, 100};
    uint256 genesis_hash;
};

TEST_CASE("InvalidateBlock - Basic invalidation", "[invalidateblock]") {
    InvalidateBlockTestFixture fixture;

    // Build a chain: genesis -> block1 -> block2 -> block3
    uint256 block1 = fixture.MineBlock();
    uint256 block2 = fixture.MineBlock();
    uint256 block3 = fixture.MineBlock();

    // Verify chain is built correctly
    auto* tip = fixture.chainstate.GetTip();
    REQUIRE(tip != nullptr);
    CHECK(tip->nHeight == 3);
    CHECK(tip->GetBlockHash() == block3);

    // Invalidate block2
    bool success = fixture.chainstate.InvalidateBlock(block2);
    REQUIRE(success);

    // Verify block2 is marked invalid
    auto* block2_index = fixture.GetBlockIndex(block2);
    REQUIRE(block2_index != nullptr);
    CHECK(block2_index->nStatus & BLOCK_FAILED_VALID);
    CHECK(!block2_index->IsValid());

    // Verify block3 is marked as BLOCK_FAILED_CHILD
    auto* block3_index = fixture.GetBlockIndex(block3);
    REQUIRE(block3_index != nullptr);
    CHECK(block3_index->nStatus & BLOCK_FAILED_CHILD);
    CHECK(!block3_index->IsValid());

    // Verify block1 is still valid
    auto* block1_index = fixture.GetBlockIndex(block1);
    REQUIRE(block1_index != nullptr);
    CHECK(block1_index->IsValid());

    // Verify tip was rewound to block1 (last valid block)
    tip = fixture.chainstate.GetTip();
    REQUIRE(tip != nullptr);
    CHECK(tip->nHeight == 1);
    CHECK(tip->GetBlockHash() == block1);
}

TEST_CASE("InvalidateBlock - Invalidate genesis", "[invalidateblock]") {
    InvalidateBlockTestFixture fixture;

    // Try to invalidate genesis block - should fail
    bool success = fixture.chainstate.InvalidateBlock(fixture.genesis_hash);
    CHECK(!success); // Bitcoin Core doesn't allow invalidating genesis

    // Genesis should still be valid
    auto* genesis = fixture.GetBlockIndex(fixture.genesis_hash);
    REQUIRE(genesis != nullptr);
    CHECK(genesis->IsValid());

    // Tip should still be genesis
    auto* tip = fixture.chainstate.GetTip();
    CHECK(tip == genesis);
}

TEST_CASE("InvalidateBlock - Invalidate with fork", "[invalidateblock]") {
    InvalidateBlockTestFixture fixture;

    // Build main chain: genesis -> A -> B -> C
    uint256 blockA = fixture.MineBlock();
    uint256 blockB = fixture.MineBlock();
    uint256 blockC = fixture.MineBlock();

    CHECK(fixture.chainstate.GetTip()->nHeight == 3);
    CHECK(fixture.chainstate.GetTip()->GetBlockHash() == blockC);

    // Now create a competing fork from A: A -> D -> E
    // We need to manually create these since we're already past them
    auto* blockA_index = fixture.chainstate.LookupBlockIndex(blockA);
    REQUIRE(blockA_index != nullptr);

    CBlockHeader headerD;
    headerD.nVersion = 1;
    headerD.hashPrevBlock = blockA;
    headerD.minerAddress = uint160();  // Zero address
    // CRITICAL: Use different time than blockB to get different hash!
    // blockB was mined at blockA_index->nTime + 120, so use +240 for blockD
    headerD.nTime = blockA_index->nTime + 240;
    headerD.nBits = consensus::GetNextWorkRequired(blockA_index, *fixture.params);
    headerD.nNonce = 0;

    uint256 randomx_hash_D;
    while (!consensus::CheckProofOfWork(headerD, headerD.nBits, *fixture.params,
                                       crypto::POWVerifyMode::MINING, &randomx_hash_D)) {
        headerD.nNonce++;
        if (headerD.nNonce == 0) headerD.nTime++;
    }
    headerD.hashRandomX = randomx_hash_D;

    ValidationState state;
    bool accepted_D = fixture.chainstate.ProcessNewBlockHeader(headerD, state);
    REQUIRE(accepted_D);
    uint256 blockD = headerD.GetHash();

    // Block E on top of D
    auto* blockD_index = fixture.chainstate.LookupBlockIndex(blockD);
    REQUIRE(blockD_index != nullptr);

    CBlockHeader headerE;
    headerE.nVersion = 1;
    headerE.hashPrevBlock = blockD;
    headerE.minerAddress = uint160();
    // Use blockD's time + 120 (which will be different from blockC's time)
    headerE.nTime = blockD_index->nTime + 120;
    headerE.nBits = consensus::GetNextWorkRequired(blockD_index, *fixture.params);
    headerE.nNonce = 0;

    uint256 randomx_hash_E;
    while (!consensus::CheckProofOfWork(headerE, headerE.nBits, *fixture.params,
                                       crypto::POWVerifyMode::MINING, &randomx_hash_E)) {
        headerE.nNonce++;
        if (headerE.nNonce == 0) headerE.nTime++;
    }
    headerE.hashRandomX = randomx_hash_E;

    bool accepted_E = fixture.chainstate.ProcessNewBlockHeader(headerE, state);
    REQUIRE(accepted_E);
    uint256 blockE = headerE.GetHash();

    // Main chain (A->B->C) should still be active (more work)
    CHECK(fixture.chainstate.GetTip()->GetBlockHash() == blockC);

    // Now invalidate block B (on main chain)
    bool success = fixture.chainstate.InvalidateBlock(blockB);
    REQUIRE(success);

    // Bitcoin Core behavior: InvalidateBlock does NOT immediately reorg
    // It just sets up the candidates. We need to call ActivateBestChain()
    // to actually perform the reorg
    bool activated = fixture.chainstate.ActivateBestChain(nullptr);
    REQUIRE(activated);

    // Should reorg to fork: A -> D -> E
    auto* tip = fixture.chainstate.GetTip();
    REQUIRE(tip != nullptr);
    CHECK(tip->GetBlockHash() == blockE);
    CHECK(tip->nHeight == 3);

    // Verify B and C are marked invalid
    auto* blockB_index = fixture.GetBlockIndex(blockB);
    auto* blockC_index = fixture.GetBlockIndex(blockC);
    CHECK(!blockB_index->IsValid());
    CHECK(!blockC_index->IsValid());
}

TEST_CASE("InvalidateBlock - Invalidate middle of long chain", "[invalidateblock]") {
    InvalidateBlockTestFixture fixture;

    // Build a long chain: genesis -> 1 -> 2 -> 3 -> 4 -> 5 -> 6 -> 7 -> 8 -> 9 -> 10
    std::vector<uint256> blocks;
    for (int i = 0; i < 10; i++) {
        blocks.push_back(fixture.MineBlock());
    }

    CHECK(fixture.chainstate.GetTip()->nHeight == 10);

    // Invalidate block 5 (middle of chain)
    bool success = fixture.chainstate.InvalidateBlock(blocks[4]); // 0-indexed, so blocks[4] = block 5
    REQUIRE(success);

    // Tip should rewind to block 4
    auto* tip = fixture.chainstate.GetTip();
    REQUIRE(tip != nullptr);
    CHECK(tip->nHeight == 4);
    CHECK(tip->GetBlockHash() == blocks[3]);

    // Verify blocks 5-10 are all marked invalid (descendants)
    for (int i = 4; i < 10; i++) {
        auto* index = fixture.GetBlockIndex(blocks[i]);
        REQUIRE(index != nullptr);
        CHECK(!index->IsValid());
        if (i == 4) {
            // Block 5 should have BLOCK_FAILED_VALID
            CHECK(index->nStatus & BLOCK_FAILED_VALID);
        } else {
            // Blocks 6-10 should have BLOCK_FAILED_CHILD
            CHECK(index->nStatus & BLOCK_FAILED_CHILD);
        }
    }

    // Verify blocks 1-4 are still valid
    for (int i = 0; i < 4; i++) {
        auto* index = fixture.GetBlockIndex(blocks[i]);
        REQUIRE(index != nullptr);
        CHECK(index->IsValid());
    }
}

TEST_CASE("InvalidateBlock - Invalidate tip", "[invalidateblock]") {
    InvalidateBlockTestFixture fixture;

    // Build chain: genesis -> block1 -> block2
    uint256 block1 = fixture.MineBlock();
    uint256 block2 = fixture.MineBlock();

    CHECK(fixture.chainstate.GetTip()->nHeight == 2);

    // Invalidate current tip (block2)
    bool success = fixture.chainstate.InvalidateBlock(block2);
    REQUIRE(success);

    // Should rewind to block1
    auto* tip = fixture.chainstate.GetTip();
    REQUIRE(tip != nullptr);
    CHECK(tip->nHeight == 1);
    CHECK(tip->GetBlockHash() == block1);

    // block2 should be invalid
    auto* block2_index = fixture.GetBlockIndex(block2);
    CHECK(!block2_index->IsValid());
}

TEST_CASE("InvalidateBlock - Nonexistent block", "[invalidateblock]") {
    InvalidateBlockTestFixture fixture;

    // Try to invalidate a block that doesn't exist
    uint256 fake_hash;
    fake_hash.SetHex("0000000000000000000000000000000000000000000000000000000000000042");

    bool success = fixture.chainstate.InvalidateBlock(fake_hash);
    CHECK(!success); // Should fail gracefully
}

TEST_CASE("InvalidateBlock - Multiple invalidations", "[invalidateblock]") {
    InvalidateBlockTestFixture fixture;

    // Build chain: genesis -> 1 -> 2 -> 3 -> 4 -> 5
    std::vector<uint256> blocks;
    for (int i = 0; i < 5; i++) {
        blocks.push_back(fixture.MineBlock());
    }

    CHECK(fixture.chainstate.GetTip()->nHeight == 5);

    // Invalidate block 3
    fixture.chainstate.InvalidateBlock(blocks[2]);
    CHECK(fixture.chainstate.GetTip()->nHeight == 2);

    // Invalidate block 2
    fixture.chainstate.InvalidateBlock(blocks[1]);
    CHECK(fixture.chainstate.GetTip()->nHeight == 1);

    // Invalidate block 1
    fixture.chainstate.InvalidateBlock(blocks[0]);

    // Should rewind to genesis
    auto* tip = fixture.chainstate.GetTip();
    REQUIRE(tip != nullptr);
    CHECK(tip->nHeight == 0);
    CHECK(tip->GetBlockHash() == fixture.genesis_hash);

    // All blocks 1-5 should be invalid
    for (const auto& hash : blocks) {
        auto* index = fixture.GetBlockIndex(hash);
        CHECK(!index->IsValid());
    }
}

TEST_CASE("InvalidateBlock - Invalidate then mine new chain", "[invalidateblock]") {
    InvalidateBlockTestFixture fixture;

    // Build initial chain: genesis -> A -> B -> C
    uint256 blockA = fixture.MineBlock();
    uint256 blockB = fixture.MineBlock();
    uint256 blockC = fixture.MineBlock();

    CHECK(fixture.chainstate.GetTip()->GetBlockHash() == blockC);

    // Invalidate block B (and C as descendant)
    fixture.chainstate.InvalidateBlock(blockB);
    CHECK(fixture.chainstate.GetTip()->GetBlockHash() == blockA);

    // Now mine a new chain: A -> D -> E -> F
    // CRITICAL: Must use different time than blockB to avoid duplicate hash!
    // blockB was at blockA.nTime + 120, so use +240 for blockD
    auto* blockA_index = fixture.chainstate.LookupBlockIndex(blockA);

    // Mine blockD with different timestamp
    CBlockHeader headerD;
    headerD.nVersion = 1;
    headerD.hashPrevBlock = blockA;
    headerD.minerAddress = uint160();
    headerD.nTime = blockA_index->nTime + 240;  // Different from blockB's +120
    headerD.nBits = consensus::GetNextWorkRequired(blockA_index, *fixture.params);
    headerD.nNonce = 0;

    uint256 randomx_hash_D;
    while (!consensus::CheckProofOfWork(headerD, headerD.nBits, *fixture.params,
                                       crypto::POWVerifyMode::MINING, &randomx_hash_D)) {
        headerD.nNonce++;
        if (headerD.nNonce == 0) headerD.nTime++;
    }
    headerD.hashRandomX = randomx_hash_D;

    ValidationState state_D;
    bool accepted_D = fixture.chainstate.ProcessNewBlockHeader(headerD, state_D);
    REQUIRE(accepted_D);
    uint256 blockD = headerD.GetHash();

    // Mine E and F normally
    uint256 blockE = fixture.MineBlock();
    uint256 blockF = fixture.MineBlock();

    // New chain should be active
    auto* tip = fixture.chainstate.GetTip();
    REQUIRE(tip != nullptr);
    CHECK(tip->nHeight == 4);
    CHECK(tip->GetBlockHash() == blockF);

    // Old blocks B and C should still be invalid
    CHECK(!fixture.GetBlockIndex(blockB)->IsValid());
    CHECK(!fixture.GetBlockIndex(blockC)->IsValid());

    // New blocks should be valid
    CHECK(fixture.GetBlockIndex(blockD)->IsValid());
    CHECK(fixture.GetBlockIndex(blockE)->IsValid());
    CHECK(fixture.GetBlockIndex(blockF)->IsValid());
}

TEST_CASE("InvalidateBlock - Deep fork invalidation", "[invalidateblock]") {
    InvalidateBlockTestFixture fixture;

    // Build main chain: genesis -> 1 -> 2 -> 3 -> 4 -> 5
    std::vector<uint256> main_chain;
    for (int i = 0; i < 5; i++) {
        main_chain.push_back(fixture.MineBlock());
    }

    // Create fork at block 2: 2 -> F1 -> F2 -> F3 -> F4 -> F5 -> F6
    auto* block2 = fixture.chainstate.LookupBlockIndex(main_chain[1]);
    REQUIRE(block2 != nullptr);

    std::vector<uint256> fork_chain;
    auto* fork_parent = block2;

    for (int i = 0; i < 6; i++) {
        CBlockHeader header;
        header.nVersion = 1;
        header.hashPrevBlock = fork_parent->GetBlockHash();
        header.minerAddress = uint160();
        // CRITICAL: Use different time offset (+240) to avoid hash collisions with main chain
        // Main chain blocks use parent.time + 120, so fork uses parent.time + 240
        header.nTime = fork_parent->nTime + 240;
        header.nBits = consensus::GetNextWorkRequired(fork_parent, *fixture.params);
        header.nNonce = 0;

        uint256 randomx_hash;
        while (!consensus::CheckProofOfWork(header, header.nBits, *fixture.params,
                                           crypto::POWVerifyMode::MINING, &randomx_hash)) {
            header.nNonce++;
            if (header.nNonce == 0) header.nTime++;
        }
        header.hashRandomX = randomx_hash;

        ValidationState state;
        fixture.chainstate.ProcessNewBlockHeader(header, state);
        fork_chain.push_back(header.GetHash());

        fork_parent = fixture.chainstate.LookupBlockIndex(fork_chain.back());
    }

    // Fork chain should now be active (longer)
    auto* tip = fixture.chainstate.GetTip();
    CHECK(tip->nHeight == 8); // genesis + 2 + 6
    CHECK(tip->GetBlockHash() == fork_chain.back());

    // Invalidate the fork at F3 (3rd block in fork)
    fixture.chainstate.InvalidateBlock(fork_chain[2]);

    // Bitcoin Core behavior: Need to call ActivateBestChain() to trigger reorg
    ValidationState activate_state;
    fixture.chainstate.ActivateBestChain(nullptr);

    // Should reorg back to main chain (which goes to height 5)
    tip = fixture.chainstate.GetTip();
    REQUIRE(tip != nullptr);
    CHECK(tip->nHeight == 5);
    CHECK(tip->GetBlockHash() == main_chain.back());

    // Verify F3-F6 are invalid
    for (size_t i = 2; i < fork_chain.size(); i++) {
        auto* index = fixture.GetBlockIndex(fork_chain[i]);
        CHECK(!index->IsValid());
    }

    // Verify F1-F2 are still valid (before invalidation point)
    for (size_t i = 0; i < 2; i++) {
        auto* index = fixture.GetBlockIndex(fork_chain[i]);
        CHECK(index->IsValid());
    }
}
