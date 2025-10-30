#include "catch_amalgamated.hpp"
#include "chain/block_index.hpp"
#include "chain/block.hpp"
#include <memory>
#include <vector>

using namespace coinbasechain;
using namespace coinbasechain::chain;

// Helper function to create a test block header
CBlockHeader CreateTestHeader(uint32_t nTime = 1234567890, uint32_t nBits = 0x1d00ffff) {
    CBlockHeader header;
    header.nVersion = 1;
    header.hashPrevBlock.SetNull();
    header.minerAddress.SetNull();
    header.nTime = nTime;
    header.nBits = nBits;
    header.nNonce = 0;
    header.hashRandomX.SetNull();
    return header;
}

TEST_CASE("CBlockIndex - Construction and initialization", "[block_index]") {
    SECTION("Default constructor initializes all fields") {
        CBlockIndex index;

        REQUIRE(index.nStatus == 0);
        REQUIRE(index.phashBlock == nullptr);
        REQUIRE(index.pprev == nullptr);
        REQUIRE(index.nHeight == 0);
        REQUIRE(index.nChainWork == 0);
        REQUIRE(index.nVersion == 0);
        REQUIRE(index.minerAddress.IsNull());
        REQUIRE(index.nTime == 0);
        REQUIRE(index.nBits == 0);
        REQUIRE(index.nNonce == 0);
        REQUIRE(index.hashRandomX.IsNull());
    }

    SECTION("Constructor from CBlockHeader copies header fields") {
        CBlockHeader header = CreateTestHeader(1000, 0x1d00ffff);
        header.nVersion = 2;
        header.nNonce = 12345;
        header.minerAddress.SetHex("0102030405060708090a0b0c0d0e0f1011121314");

        CBlockIndex index(header);

        REQUIRE(index.nVersion == 2);
        REQUIRE(index.nTime == 1000);
        REQUIRE(index.nBits == 0x1d00ffff);
        REQUIRE(index.nNonce == 12345);
        REQUIRE(index.minerAddress == header.minerAddress);
        REQUIRE(index.hashRandomX == header.hashRandomX);

        // Metadata fields should be default-initialized
        REQUIRE(index.nStatus == 0);
        REQUIRE(index.phashBlock == nullptr);
        REQUIRE(index.pprev == nullptr);
        REQUIRE(index.nHeight == 0);
        REQUIRE(index.nChainWork == 0);
    }

    SECTION("Copy/move constructors are deleted") {
        // This test verifies the design decision is enforced at compile time
        // If this compiles, the test would fail, but it shouldn't compile
        STATIC_REQUIRE(std::is_copy_constructible_v<CBlockIndex> == false);
        STATIC_REQUIRE(std::is_copy_assignable_v<CBlockIndex> == false);
        STATIC_REQUIRE(std::is_move_constructible_v<CBlockIndex> == false);
        STATIC_REQUIRE(std::is_move_assignable_v<CBlockIndex> == false);
    }
}

TEST_CASE("CBlockIndex - GetBlockHash", "[block_index]") {
    SECTION("GetBlockHash returns the hash when phashBlock is set") {
        CBlockIndex index;
        uint256 hash;
        hash.SetHex("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20");

        index.phashBlock = &hash;

        REQUIRE(index.GetBlockHash() == hash);
    }

    SECTION("GetBlockHash with real header hash") {
        CBlockHeader header = CreateTestHeader();
        uint256 hash = header.GetHash();

        CBlockIndex index(header);
        index.phashBlock = &hash;

        REQUIRE(index.GetBlockHash() == hash);
        REQUIRE(index.GetBlockHash() == header.GetHash());
    }
}

TEST_CASE("CBlockIndex - GetBlockHeader", "[block_index]") {
    SECTION("GetBlockHeader reconstructs header without parent") {
        CBlockHeader original = CreateTestHeader(1000, 0x1d00ffff);
        original.nVersion = 2;
        original.nNonce = 54321;
        original.minerAddress.SetHex("0102030405060708090a0b0c0d0e0f1011121314");
        original.hashRandomX.SetHex("1111111111111111111111111111111111111111111111111111111111111111");

        CBlockIndex index(original);

        CBlockHeader reconstructed = index.GetBlockHeader();

        REQUIRE(reconstructed.nVersion == original.nVersion);
        REQUIRE(reconstructed.nTime == original.nTime);
        REQUIRE(reconstructed.nBits == original.nBits);
        REQUIRE(reconstructed.nNonce == original.nNonce);
        REQUIRE(reconstructed.minerAddress == original.minerAddress);
        REQUIRE(reconstructed.hashRandomX == original.hashRandomX);
        REQUIRE(reconstructed.hashPrevBlock.IsNull()); // No parent
    }

    SECTION("GetBlockHeader includes parent hash when pprev is set") {
        // Create parent block
        CBlockHeader parent_header = CreateTestHeader(900);
        uint256 parent_hash = parent_header.GetHash();
        CBlockIndex parent(parent_header);
        parent.phashBlock = &parent_hash;

        // Create child block
        CBlockHeader child_header = CreateTestHeader(1000);
        child_header.hashPrevBlock = parent_hash;
        CBlockIndex child(child_header);
        child.pprev = &parent;

        CBlockHeader reconstructed = child.GetBlockHeader();

        REQUIRE(reconstructed.hashPrevBlock == parent_hash);
        REQUIRE(reconstructed.hashPrevBlock == parent.GetBlockHash());
    }

    SECTION("GetBlockHeader returns self-contained copy") {
        CBlockHeader original = CreateTestHeader();
        uint256 hash = original.GetHash();

        CBlockIndex index(original);
        index.phashBlock = &hash;

        CBlockHeader copy = index.GetBlockHeader();

        // Modify the index
        index.nVersion = 999;
        index.nTime = 9999;

        // Copy should be unchanged
        REQUIRE(copy.nVersion == original.nVersion);
        REQUIRE(copy.nTime == original.nTime);
    }
}

TEST_CASE("CBlockIndex - GetBlockTime", "[block_index]") {
    SECTION("GetBlockTime returns nTime as int64_t") {
        CBlockIndex index;
        index.nTime = 1234567890;

        REQUIRE(index.GetBlockTime() == 1234567890);
    }

    SECTION("GetBlockTime handles maximum uint32_t value") {
        CBlockIndex index;
        index.nTime = 0xFFFFFFFF; // Max uint32_t

        int64_t time = index.GetBlockTime();
        REQUIRE(time == 0xFFFFFFFF);
        REQUIRE(time > 0); // Should be positive
    }
}

TEST_CASE("CBlockIndex - GetMedianTimePast", "[block_index]") {
    SECTION("Single block returns its own time") {
        CBlockIndex index;
        index.nTime = 1000;

        REQUIRE(index.GetMedianTimePast() == 1000);
    }

    SECTION("Two blocks returns median") {
        CBlockIndex index1;
        index1.nTime = 1000;

        CBlockIndex index2;
        index2.nTime = 2000;
        index2.pprev = &index1;

        int64_t median = index2.GetMedianTimePast();
        // Median of [1000, 2000] is one of them (depends on sort)
        REQUIRE((median == 1000 || median == 2000));
    }

    SECTION("Eleven blocks uses all for median") {
        // Create chain of 11 blocks with times: 1000, 1100, 1200, ..., 2000
        std::vector<CBlockIndex> chain(11);
        for (int i = 0; i < 11; i++) {
            chain[i].nTime = 1000 + i * 100;
            if (i > 0) {
                chain[i].pprev = &chain[i-1];
            }
        }

        int64_t median = chain[10].GetMedianTimePast();
        // Median of 11 values is the 6th value (index 5)
        REQUIRE(median == 1500); // 1000 + 5*100
    }

    SECTION("More than eleven blocks only uses last 11") {
        // Create chain of 20 blocks with times: 1000, 1100, 1200, ..., 2900
        std::vector<CBlockIndex> chain(20);
        for (int i = 0; i < 20; i++) {
            chain[i].nTime = 1000 + i * 100;
            if (i > 0) {
                chain[i].pprev = &chain[i-1];
            }
        }

        int64_t median = chain[19].GetMedianTimePast();
        // Should only consider blocks [9..19] (last 11)
        // Median of those is block 14: 1000 + 14*100 = 2400
        REQUIRE(median == 2400);
    }

    SECTION("Handles non-monotonic times correctly") {
        // Create blocks with intentionally unsorted times
        CBlockIndex index1;
        index1.nTime = 5000;

        CBlockIndex index2;
        index2.nTime = 3000; // Earlier!
        index2.pprev = &index1;

        CBlockIndex index3;
        index3.nTime = 4000; // Middle
        index3.pprev = &index2;

        int64_t median = index3.GetMedianTimePast();
        // Median of [3000, 4000, 5000] is 4000
        REQUIRE(median == 4000);
    }

    SECTION("Chain with duplicate timestamps") {
        std::vector<CBlockIndex> chain(5);
        chain[0].nTime = 1000;
        chain[1].nTime = 1000;
        chain[1].pprev = &chain[0];
        chain[2].nTime = 2000;
        chain[2].pprev = &chain[1];
        chain[3].nTime = 2000;
        chain[3].pprev = &chain[2];
        chain[4].nTime = 3000;
        chain[4].pprev = &chain[3];

        int64_t median = chain[4].GetMedianTimePast();
        // Sorted: [1000, 1000, 2000, 2000, 3000]
        // Median is index 2 = 2000
        REQUIRE(median == 2000);
    }
}

TEST_CASE("CBlockIndex - GetAncestor", "[block_index]") {
    SECTION("GetAncestor returns nullptr for invalid heights") {
        CBlockIndex index;
        index.nHeight = 5;

        REQUIRE(index.GetAncestor(-1) == nullptr);
        REQUIRE(index.GetAncestor(6) == nullptr);
        REQUIRE(index.GetAncestor(100) == nullptr);
    }

    SECTION("GetAncestor returns self for own height") {
        CBlockIndex index;
        index.nHeight = 5;

        REQUIRE(index.GetAncestor(5) == &index);
    }

    SECTION("GetAncestor walks chain correctly") {
        // Create chain: 0 -> 1 -> 2 -> 3 -> 4 -> 5
        std::vector<CBlockIndex> chain(6);
        for (int i = 0; i < 6; i++) {
            chain[i].nHeight = i;
            if (i > 0) {
                chain[i].pprev = &chain[i-1];
            }
        }

        // Test from tip (height 5)
        REQUIRE(chain[5].GetAncestor(5) == &chain[5]);
        REQUIRE(chain[5].GetAncestor(4) == &chain[4]);
        REQUIRE(chain[5].GetAncestor(3) == &chain[3]);
        REQUIRE(chain[5].GetAncestor(2) == &chain[2]);
        REQUIRE(chain[5].GetAncestor(1) == &chain[1]);
        REQUIRE(chain[5].GetAncestor(0) == &chain[0]);
    }

    SECTION("GetAncestor from middle of chain") {
        std::vector<CBlockIndex> chain(6);
        for (int i = 0; i < 6; i++) {
            chain[i].nHeight = i;
            if (i > 0) {
                chain[i].pprev = &chain[i-1];
            }
        }

        // Test from height 3
        REQUIRE(chain[3].GetAncestor(3) == &chain[3]);
        REQUIRE(chain[3].GetAncestor(2) == &chain[2]);
        REQUIRE(chain[3].GetAncestor(1) == &chain[1]);
        REQUIRE(chain[3].GetAncestor(0) == &chain[0]);
        REQUIRE(chain[3].GetAncestor(4) == nullptr); // Too high
    }

    SECTION("GetAncestor non-const overload") {
        std::vector<CBlockIndex> chain(3);
        for (int i = 0; i < 3; i++) {
            chain[i].nHeight = i;
            if (i > 0) {
                chain[i].pprev = &chain[i-1];
            }
        }

        CBlockIndex* ancestor = chain[2].GetAncestor(1);
        REQUIRE(ancestor == &chain[1]);

        // Verify we can modify through non-const pointer
        ancestor->nTime = 9999;
        REQUIRE(chain[1].nTime == 9999);
    }

    SECTION("GetAncestor on long chain") {
        // Test performance isn't terrible for longer chains
        const int chain_length = 1000;
        std::vector<CBlockIndex> chain(chain_length);
        for (int i = 0; i < chain_length; i++) {
            chain[i].nHeight = i;
            if (i > 0) {
                chain[i].pprev = &chain[i-1];
            }
        }

        REQUIRE(chain[999].GetAncestor(0) == &chain[0]);
        REQUIRE(chain[999].GetAncestor(500) == &chain[500]);
        REQUIRE(chain[999].GetAncestor(999) == &chain[999]);
    }
}

TEST_CASE("CBlockIndex - IsValid and RaiseValidity", "[block_index]") {
    SECTION("Default block is not valid") {
        CBlockIndex index;
        REQUIRE(index.nStatus == BLOCK_VALID_UNKNOWN);
        REQUIRE_FALSE(index.IsValid(BLOCK_VALID_HEADER));
        REQUIRE_FALSE(index.IsValid(BLOCK_VALID_TREE));
    }

    SECTION("RaiseValidity to HEADER") {
        CBlockIndex index;

        bool changed = index.RaiseValidity(BLOCK_VALID_HEADER);

        REQUIRE(changed);
        REQUIRE(index.IsValid(BLOCK_VALID_HEADER));
        REQUIRE_FALSE(index.IsValid(BLOCK_VALID_TREE));
    }

    SECTION("RaiseValidity to TREE") {
        CBlockIndex index;

        index.RaiseValidity(BLOCK_VALID_TREE);

        REQUIRE(index.IsValid(BLOCK_VALID_HEADER));
        REQUIRE(index.IsValid(BLOCK_VALID_TREE));
    }

    SECTION("RaiseValidity returns false if already at level") {
        CBlockIndex index;

        REQUIRE(index.RaiseValidity(BLOCK_VALID_HEADER) == true);
        REQUIRE(index.RaiseValidity(BLOCK_VALID_HEADER) == false); // No change
    }

    SECTION("RaiseValidity returns false if failed") {
        CBlockIndex index;
        index.nStatus = BLOCK_FAILED_VALID;

        REQUIRE(index.RaiseValidity(BLOCK_VALID_HEADER) == false);
        REQUIRE_FALSE(index.IsValid(BLOCK_VALID_HEADER));
    }

    SECTION("IsValid returns false for failed blocks") {
        CBlockIndex index;
        index.nStatus = BLOCK_VALID_HEADER | BLOCK_FAILED_VALID;

        REQUIRE_FALSE(index.IsValid(BLOCK_VALID_HEADER));
    }

    SECTION("Failed child also fails validation") {
        CBlockIndex index;
        index.nStatus = BLOCK_VALID_TREE | BLOCK_FAILED_CHILD;

        REQUIRE_FALSE(index.IsValid(BLOCK_VALID_TREE));
    }

    SECTION("Validity levels are hierarchical") {
        CBlockIndex index;

        index.RaiseValidity(BLOCK_VALID_TREE);

        // TREE implies HEADER
        REQUIRE(index.IsValid(BLOCK_VALID_HEADER));
        REQUIRE(index.IsValid(BLOCK_VALID_TREE));
    }
}

TEST_CASE("CBlockIndex - ToString", "[block_index]") {
    SECTION("ToString produces readable output") {
        CBlockIndex index;
        uint256 hash;
        hash.SetHex("1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");

        index.nHeight = 100;
        index.phashBlock = &hash;
        index.minerAddress.SetHex("0102030405060708090a0b0c0d0e0f1011121314");

        std::string str = index.ToString();

        REQUIRE(str.find("height=100") != std::string::npos);
        REQUIRE(str.find("CBlockIndex") != std::string::npos);
        REQUIRE_FALSE(str.empty());
    }

    SECTION("ToString handles null phashBlock") {
        CBlockIndex index;
        index.nHeight = 5;

        std::string str = index.ToString();

        REQUIRE(str.find("null") != std::string::npos);
    }
}

TEST_CASE("GetBlockProof - work calculation", "[block_index]") {
    SECTION("GetBlockProof returns zero for invalid nBits") {
        CBlockIndex index;

        // Negative target
        index.nBits = 0x00800000;
        REQUIRE(GetBlockProof(index) == 0);

        // Zero target
        index.nBits = 0x00000000;
        REQUIRE(GetBlockProof(index) == 0);

        // Zero mantissa
        index.nBits = 0x01000000;
        REQUIRE(GetBlockProof(index) == 0);
    }

    SECTION("GetBlockProof returns non-zero for valid nBits") {
        CBlockIndex index;
        index.nBits = 0x1d00ffff; // Bitcoin's initial difficulty

        arith_uint256 proof = GetBlockProof(index);

        REQUIRE(proof > 0);
    }

    SECTION("Higher difficulty produces more work") {
        CBlockIndex easy;
        easy.nBits = 0x1d00ffff; // Easier (larger target)

        CBlockIndex hard;
        hard.nBits = 0x1c00ffff; // Harder (smaller target)

        arith_uint256 easy_work = GetBlockProof(easy);
        arith_uint256 hard_work = GetBlockProof(hard);

        REQUIRE(hard_work > easy_work);
    }

    SECTION("GetBlockProof formula correctness") {
        CBlockIndex index;
        index.nBits = 0x1d00ffff;

        // Manual calculation
        arith_uint256 bnTarget;
        bool fNegative, fOverflow;
        bnTarget.SetCompact(index.nBits, &fNegative, &fOverflow);

        REQUIRE_FALSE(fNegative);
        REQUIRE_FALSE(fOverflow);
        REQUIRE(bnTarget != 0);

        // Expected: ~target / (target + 1) + 1
        arith_uint256 expected = (~bnTarget / (bnTarget + 1)) + 1;
        arith_uint256 actual = GetBlockProof(index);

        REQUIRE(actual == expected);
    }

    SECTION("GetBlockProof with RegTest difficulty") {
        CBlockIndex index;
        index.nBits = 0x207fffff; // RegTest (very easy)

        arith_uint256 proof = GetBlockProof(index);

        REQUIRE(proof > 0);
        REQUIRE(proof == 2); // RegTest has minimal work
    }

    SECTION("GetBlockProof consistency across multiple calls") {
        CBlockIndex index;
        index.nBits = 0x1d00ffff;

        arith_uint256 proof1 = GetBlockProof(index);
        arith_uint256 proof2 = GetBlockProof(index);

        REQUIRE(proof1 == proof2);
    }
}

TEST_CASE("LastCommonAncestor - fork detection", "[block_index]") {
    SECTION("Returns nullptr for null inputs") {
        CBlockIndex index;

        REQUIRE(LastCommonAncestor(nullptr, nullptr) == nullptr);
        REQUIRE(LastCommonAncestor(&index, nullptr) == nullptr);
        REQUIRE(LastCommonAncestor(nullptr, &index) == nullptr);
    }

    SECTION("Two identical blocks return self") {
        CBlockIndex index;

        const CBlockIndex* ancestor = LastCommonAncestor(&index, &index);

        REQUIRE(ancestor == &index);
    }

    SECTION("Parent and child return parent") {
        CBlockIndex parent;
        parent.nHeight = 0;

        CBlockIndex child;
        child.nHeight = 1;
        child.pprev = &parent;

        const CBlockIndex* ancestor = LastCommonAncestor(&parent, &child);

        REQUIRE(ancestor == &parent);
    }

    SECTION("Fork from common ancestor") {
        // Create: Genesis -> A -> B -> C (main)
        //                     \-> D -> E (fork)
        CBlockIndex genesis;
        genesis.nHeight = 0;

        CBlockIndex a;
        a.nHeight = 1;
        a.pprev = &genesis;

        CBlockIndex b;
        b.nHeight = 2;
        b.pprev = &a;

        CBlockIndex c;
        c.nHeight = 3;
        c.pprev = &b;

        CBlockIndex d;
        d.nHeight = 2;
        d.pprev = &a;

        CBlockIndex e;
        e.nHeight = 3;
        e.pprev = &d;

        // Test various combinations
        REQUIRE(LastCommonAncestor(&c, &e) == &a);
        REQUIRE(LastCommonAncestor(&b, &d) == &a);
        REQUIRE(LastCommonAncestor(&c, &d) == &a);
        REQUIRE(LastCommonAncestor(&b, &e) == &a);
    }

    SECTION("Fork with different heights") {
        // Genesis -> A -> B -> C -> D -> E (long chain)
        //         \-> F (short fork)
        CBlockIndex genesis;
        genesis.nHeight = 0;

        std::vector<CBlockIndex> main_chain(5);
        main_chain[0].nHeight = 1;
        main_chain[0].pprev = &genesis;
        for (int i = 1; i < 5; i++) {
            main_chain[i].nHeight = i + 1;
            main_chain[i].pprev = &main_chain[i-1];
        }

        CBlockIndex fork;
        fork.nHeight = 1;
        fork.pprev = &genesis;

        REQUIRE(LastCommonAncestor(&main_chain[4], &fork) == &genesis);
    }

    SECTION("Deep fork") {
        // Create a long common chain, then fork
        std::vector<CBlockIndex> common(10);
        common[0].nHeight = 0;
        for (int i = 1; i < 10; i++) {
            common[i].nHeight = i;
            common[i].pprev = &common[i-1];
        }

        // Branch A
        std::vector<CBlockIndex> branch_a(5);
        branch_a[0].nHeight = 10;
        branch_a[0].pprev = &common[9];
        for (int i = 1; i < 5; i++) {
            branch_a[i].nHeight = 10 + i;
            branch_a[i].pprev = &branch_a[i-1];
        }

        // Branch B
        std::vector<CBlockIndex> branch_b(3);
        branch_b[0].nHeight = 10;
        branch_b[0].pprev = &common[9];
        for (int i = 1; i < 3; i++) {
            branch_b[i].nHeight = 10 + i;
            branch_b[i].pprev = &branch_b[i-1];
        }

        REQUIRE(LastCommonAncestor(&branch_a[4], &branch_b[2]) == &common[9]);
    }

    SECTION("Ancestor is always at or below both heights") {
        std::vector<CBlockIndex> chain(10);
        chain[0].nHeight = 0;
        for (int i = 1; i < 10; i++) {
            chain[i].nHeight = i;
            chain[i].pprev = &chain[i-1];
        }

        const CBlockIndex* ancestor = LastCommonAncestor(&chain[7], &chain[3]);

        REQUIRE(ancestor == &chain[3]);
        REQUIRE(ancestor->nHeight <= chain[7].nHeight);
        REQUIRE(ancestor->nHeight <= chain[3].nHeight);
    }
}

TEST_CASE("BlockStatus - flag operations", "[block_index]") {
    SECTION("BLOCK_FAILED_MASK includes all failure flags") {
        REQUIRE((BLOCK_FAILED_MASK & BLOCK_FAILED_VALID) == BLOCK_FAILED_VALID);
        REQUIRE((BLOCK_FAILED_MASK & BLOCK_FAILED_CHILD) == BLOCK_FAILED_CHILD);
    }

    SECTION("Validity levels are sequential integers") {
        // Validity levels use numeric comparison, not bitmasks
        REQUIRE(BLOCK_VALID_UNKNOWN == 0);
        REQUIRE(BLOCK_VALID_HEADER == 1);
        REQUIRE(BLOCK_VALID_TREE == 2);

        // Failure flags are bitflags (powers of 2)
        REQUIRE(BLOCK_FAILED_VALID == 32);
        REQUIRE(BLOCK_FAILED_CHILD == 64);

        // Failure flags don't overlap with validity levels
        REQUIRE((BLOCK_FAILED_MASK & 0xFF) > BLOCK_VALID_TREE);
    }

    SECTION("Status flag combinations") {
        CBlockIndex index;

        // Set both validity and failure (should fail validation)
        index.nStatus = BLOCK_VALID_HEADER | BLOCK_FAILED_VALID;

        REQUIRE_FALSE(index.IsValid(BLOCK_VALID_HEADER));
    }
}

TEST_CASE("CBlockIndex - Integration scenarios", "[block_index]") {
    SECTION("Simulate block chain building") {
        // Create a realistic chain scenario using std::map (like BlockManager does)
        std::map<uint256, CBlockIndex> block_index;
        std::vector<CBlockHeader> headers;
        std::vector<uint256> hashes;
        std::vector<CBlockIndex*> indices; // Pointers to map entries

        // Genesis
        headers.push_back(CreateTestHeader(1000000, 0x207fffff));
        hashes.push_back(headers[0].GetHash());

        auto [it0, _] = block_index.try_emplace(hashes[0], headers[0]);
        CBlockIndex* genesis = &it0->second;
        genesis->phashBlock = &it0->first;
        genesis->nHeight = 0;
        genesis->nChainWork = GetBlockProof(*genesis);
        [[maybe_unused]] bool raised0 = genesis->RaiseValidity(BLOCK_VALID_TREE);
        indices.push_back(genesis);

        // Build chain of 10 blocks
        for (int i = 1; i < 10; i++) {
            headers.push_back(CreateTestHeader(1000000 + i * 600, 0x207fffff));
            headers[i].hashPrevBlock = hashes[i-1];
            hashes.push_back(headers[i].GetHash());

            auto [it, inserted] = block_index.try_emplace(hashes[i], headers[i]);
            REQUIRE(inserted);

            CBlockIndex* pindex = &it->second;
            pindex->phashBlock = &it->first;
            pindex->pprev = indices[i-1];
            pindex->nHeight = i;
            pindex->nChainWork = indices[i-1]->nChainWork + GetBlockProof(*pindex);
            [[maybe_unused]] bool raised = pindex->RaiseValidity(BLOCK_VALID_TREE);
            indices.push_back(pindex);
        }

        // Verify chain properties
        REQUIRE(indices[9]->nHeight == 9);
        REQUIRE(indices[9]->pprev == indices[8]);
        REQUIRE(indices[9]->GetBlockHash() == hashes[9]);
        REQUIRE(indices[9]->IsValid(BLOCK_VALID_TREE));
        REQUIRE(indices[9]->nChainWork > indices[0]->nChainWork);

        // Verify we can reconstruct headers
        CBlockHeader reconstructed = indices[9]->GetBlockHeader();
        REQUIRE(reconstructed.hashPrevBlock == hashes[8]);
        REQUIRE(reconstructed.GetHash() == hashes[9]);

        // Verify ancestor lookup
        REQUIRE(indices[9]->GetAncestor(0) == indices[0]);
        REQUIRE(indices[9]->GetAncestor(5) == indices[5]);

        // Verify median time past
        int64_t mtp = indices[9]->GetMedianTimePast();
        REQUIRE(mtp > 0);
        REQUIRE(mtp >= indices[0]->nTime);
        REQUIRE(mtp <= indices[9]->nTime);
    }
}
