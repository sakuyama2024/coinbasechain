// Copyright (c) 2024 Coinbase Chain
// Fuzz target for chain reorganization logic
//
// This fuzz target exercises the DEEP validation logic in ChainstateManager:
// - Chain reorganizations (competing forks with different work levels)
// - Orphan header processing (out-of-order arrival)
// - InvalidateBlock cascades (marking descendants invalid)
// - Fork selection (choosing highest work chain)
// - Suspicious reorg depth limits
//
// Unlike shallow parsing fuzz targets, this tests the 316 conditional branches
// in chainstate_manager.cpp that handle complex state transitions.

#include "validation/chainstate_manager.hpp"
#include "chain/chainparams.hpp"
#include "consensus/pow.hpp"
#include "util/time.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <cstring>

using namespace coinbasechain;
using namespace coinbasechain::chain;
using namespace coinbasechain::validation;
using namespace coinbasechain::consensus;

// Testable ChainstateManager that skips expensive PoW checks
class FuzzChainstateManager : public ChainstateManager {
public:
    FuzzChainstateManager(const ChainParams& params, int suspicious_reorg_depth)
        : ChainstateManager(params, suspicious_reorg_depth) {}

    // Override PoW check to always pass (we're fuzzing chain logic, not RandomX)
    bool CheckProofOfWork(const CBlockHeader& header, crypto::POWVerifyMode mode) const override {
        return true;
    }

    bool CheckBlockHeaderWrapper(const CBlockHeader& header, ValidationState& state) const override {
        // Skip PoW check, just validate structure
        if (header.IsNull()) {
            return state.Invalid("bad-header-null", "header is null");
        }
        return true;
    }

    bool ContextualCheckBlockHeaderWrapper(const CBlockHeader& header,
                                           const CBlockIndex* pindexPrev,
                                           int64_t adjusted_time,
                                           ValidationState& state) const override {
        // Simplified contextual checks (skip difficulty validation for fuzzing)
        if (pindexPrev && header.nTime <= pindexPrev->GetMedianTimePast()) {
            return state.Invalid("time-too-old", "block timestamp too early");
        }
        return true;
    }
};

// Fuzz input parser
class FuzzInput {
public:
    FuzzInput(const uint8_t* data, size_t size)
        : data_(data), size_(size), offset_(0) {}

    // Read single byte
    uint8_t ReadByte() {
        if (offset_ >= size_) return 0;
        return data_[offset_++];
    }

    // Read uint32
    uint32_t ReadUInt32() {
        uint32_t val = 0;
        for (int i = 0; i < 4; i++) {
            val |= static_cast<uint32_t>(ReadByte()) << (i * 8);
        }
        return val;
    }

    // Read uint256 (for hashes)
    uint256 ReadUInt256() {
        uint256 val;
        for (size_t i = 0; i < val.size(); i++) {
            val.data()[i] = ReadByte();
        }
        return val;
    }

    // Read bool
    bool ReadBool() {
        return (ReadByte() & 1) != 0;
    }

    bool HasMoreData() const {
        return offset_ < size_;
    }

    size_t Remaining() const {
        return (offset_ < size_) ? (size_ - offset_) : 0;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t offset_;
};

// Build a block header with fuzzer-controlled fields
CBlockHeader BuildFuzzHeader(FuzzInput& input, const uint256& prevHash, uint32_t baseTime) {
    CBlockHeader header;
    header.nVersion = 1;
    header.hashPrevBlock = prevHash;

    // Fuzz miner address (just use random bytes)
    for (size_t i = 0; i < 20; i++) {
        header.minerAddress.data()[i] = input.ReadByte();
    }

    // Time: base + small offset to keep roughly increasing
    uint32_t timeOffset = input.ReadByte();
    header.nTime = baseTime + timeOffset;

    // Difficulty: use easy target for fuzzing
    header.nBits = 0x207fffff; // Very easy target

    // Nonce and RandomX hash (not validated in fuzz mode)
    header.nNonce = input.ReadUInt32();
    header.hashRandomX = input.ReadUInt256();

    return header;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // Need at least some bytes to do anything interesting
    if (size < 10) return 0;

    FuzzInput input(data, size);

    // Create regtest params for fuzzing
    auto params = ChainParams::CreateRegTest();

    // Read fuzz configuration
    int suspicious_reorg_depth = 10 + (input.ReadByte() % 90); // 10-100
    bool test_orphans = input.ReadBool();
    bool test_invalidate = input.ReadBool();
    uint8_t num_chains = 1 + (input.ReadByte() % 4); // 1-4 competing chains

    // Create chainstate manager
    FuzzChainstateManager chainstate(*params, suspicious_reorg_depth);

    // Initialize with genesis
    const auto& genesis = params->GenesisBlock();
    if (!chainstate.Initialize(genesis)) {
        return 0; // Initialization failed, skip
    }

    // Base timestamp
    uint32_t baseTime = genesis.nTime + 120;

    // Track chain tips for building competing forks
    std::vector<uint256> chain_tips;
    chain_tips.push_back(genesis.GetHash());

    // Build and submit headers in various patterns
    while (input.Remaining() >= 30) { // Need enough bytes for a header
        uint8_t action = input.ReadByte() % 5;

        switch (action) {
        case 0: { // Extend main chain
            if (chain_tips.empty()) break;

            uint256 prevHash = chain_tips[0];
            CBlockHeader header = BuildFuzzHeader(input, prevHash, baseTime);
            baseTime = header.nTime + 120;

            ValidationState state;
            auto* pindex = chainstate.AcceptBlockHeader(header, state);
            if (pindex) {
                chain_tips[0] = header.GetHash();
            }
            break;
        }

        case 1: { // Create competing fork
            if (num_chains <= 1) break;

            // Fork from a random height on main chain
            const auto* tip = chainstate.GetTip();
            if (!tip || tip->nHeight < 1) break;

            int fork_height = input.ReadByte() % (tip->nHeight + 1);
            const auto* fork_point = tip->GetAncestor(fork_height);
            if (!fork_point) break;

            uint256 prevHash = fork_point->GetBlockHash();
            CBlockHeader header = BuildFuzzHeader(input, prevHash, baseTime);

            ValidationState state;
            auto* pindex = chainstate.AcceptBlockHeader(header, state);
            if (pindex && chain_tips.size() < num_chains) {
                chain_tips.push_back(header.GetHash());
            }
            break;
        }

        case 2: { // Extend random chain tip
            if (chain_tips.size() <= 1) break;

            size_t tip_idx = 1 + (input.ReadByte() % (chain_tips.size() - 1));
            if (tip_idx >= chain_tips.size()) break;

            uint256 prevHash = chain_tips[tip_idx];
            CBlockHeader header = BuildFuzzHeader(input, prevHash, baseTime);
            baseTime = header.nTime + 120;

            ValidationState state;
            auto* pindex = chainstate.AcceptBlockHeader(header, state);
            if (pindex) {
                chain_tips[tip_idx] = header.GetHash();
            }
            break;
        }

        case 3: { // Test orphan handling (submit block with missing parent)
            if (!test_orphans) break;

            // Create header with non-existent parent
            uint256 fakeParent = input.ReadUInt256();
            CBlockHeader header = BuildFuzzHeader(input, fakeParent, baseTime);

            ValidationState state;
            chainstate.AcceptBlockHeader(header, state, input.ReadByte());
            // Might be orphaned or rejected
            break;
        }

        case 4: { // Test InvalidateBlock
            if (!test_invalidate) break;

            const auto* tip = chainstate.GetTip();
            if (!tip || tip->nHeight < 2) break;

            // Invalidate a block at random height
            int invalidate_height = 1 + (input.ReadByte() % tip->nHeight);
            const auto* to_invalidate = tip->GetAncestor(invalidate_height);
            if (to_invalidate) {
                chainstate.InvalidateBlock(to_invalidate->GetBlockHash());
            }
            break;
        }
        }

        // Periodically try to activate best chain
        if ((input.ReadByte() & 0x0f) == 0) {
            chainstate.ActivateBestChain();
        }
    }

    // Final activation to ensure state is consistent
    chainstate.ActivateBestChain();

    // Test some query operations to ensure state is valid
    const auto* tip = chainstate.GetTip();
    if (tip) {
        (void)chainstate.IsOnActiveChain(tip);
        (void)chainstate.GetBlockAtHeight(tip->nHeight / 2);
        (void)chainstate.GetLocator(tip);
    }

    (void)chainstate.GetBlockCount();
    (void)chainstate.GetChainHeight();
    (void)chainstate.GetOrphanHeaderCount();

    // Cleanup orphans
    (void)chainstate.EvictOrphanHeaders();

    return 0;
}

#ifdef STANDALONE_FUZZ_TARGET_DRIVER
// LLVMFuzzerTestOneInput is already defined above, no need to redefine
#endif
