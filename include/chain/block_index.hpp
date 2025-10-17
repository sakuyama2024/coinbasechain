// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_CHAIN_BLOCK_INDEX_HPP
#define COINBASECHAIN_CHAIN_BLOCK_INDEX_HPP

#include "primitives/block.h"
#include "uint.hpp"
#include "arith_uint256.h"
#include <cstdint>
#include <algorithm>
#include <cassert>

namespace coinbasechain {
namespace chain {

/**
 * Median Time Past calculation span
 * Number of previous blocks to consider when calculating median time
 * Used by GetMedianTimePast() and also defined in validation.hpp
 */
static constexpr int MEDIAN_TIME_SPAN = 11;

/**
 * Block validation status
 * Tracks how far a block header has been validated
 *
 * Headers-only chain - no transaction/script validation levels
 */
enum BlockStatus : uint32_t {
    //! Unused/unknown
    BLOCK_VALID_UNKNOWN      = 0,

    //! Parsed, has valid POW, valid difficulty, valid timestamp
    BLOCK_VALID_HEADER       = 1,

    //! All parent headers found, difficulty matches, timestamp >= median previous
    //! Implies all parents are also at least TREE
    //! This is the highest validation level for headers-only chain
    BLOCK_VALID_TREE         = 2,

    //! All validity bits
    BLOCK_VALID_MASK         = BLOCK_VALID_HEADER | BLOCK_VALID_TREE,

    BLOCK_FAILED_VALID       = 32,  //! Stage after last reached validity failed
    BLOCK_FAILED_CHILD       = 64,  //! Descends from failed block
    BLOCK_FAILED_MASK        = BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD,
};

/**
 * CBlockIndex - Metadata for a single block header
 *
 * Simplified from Bitcoin Core's CBlockIndex for headers-only chain.
 *
 * Key simplifications:
 * - No transaction counts (nTx, nChainTx)
 * - No file positions (nFile, nDataPos, nUndoPos)
 * - No skip list pointer (pskip) - can add later for performance
 * - No sequence ID (nSequenceId)
 * - No cached nTimeMax (calculate on demand)
 *
 * The header data is stored inline (headers-only chain, no separate transaction data).
 */
class CBlockIndex {
public:
    //! Validation status of this block header
    uint32_t nStatus{0};

    /**
     * Pointer to the block's hash (DOES NOT OWN).
     *
     * Points to the key of BlockManager::m_block_index map entry.
     * Lifetime: Valid as long as the block remains in BlockManager's map.
     *
     * MUST be set after insertion via: pindex->phashBlock = &map_iterator->first
     * NEVER null after proper initialization (GetBlockHash() asserts non-null).
     */
    const uint256* phashBlock{nullptr};

    /**
     * Pointer to previous block in chain (DOES NOT OWN).
     *
     * Forms the blockchain tree structure by linking to parent.
     * Lifetime: Points to CBlockIndex owned by BlockManager's map.
     *
     * nullptr for genesis block, otherwise points to parent block's CBlockIndex.
     * All CBlockIndex instances share the same lifetime (owned by BlockManager).
     */
    CBlockIndex* pprev{nullptr};

    // Height of this block in the chain (genesis = 0)
    int nHeight{0};

    // Cumulative work up to and including this block
    arith_uint256 nChainWork{};

    // Block header fields (stored inline)
    int32_t nVersion{0};
    uint160 minerAddress{};  // Default-initialized (SetNull())
    uint32_t nTime{0};
    uint32_t nBits{0};
    uint32_t nNonce{0};
    uint256 hashRandomX{};  // Default-initialized (SetNull())

    // Constructor
    CBlockIndex() = default;

    explicit CBlockIndex(const CBlockHeader& block)
        : nVersion{block.nVersion},
          minerAddress{block.minerAddress},
          nTime{block.nTime},
          nBits{block.nBits},
          nNonce{block.nNonce},
          hashRandomX{block.hashRandomX}
    {
    }

    /**
     * Get the hash of this block
     * @return Block hash (by value copy)
     * @note Asserts phashBlock is non-null (must be initialized by BlockManager)
     */
    [[nodiscard]] uint256 GetBlockHash() const noexcept
    {
        assert(phashBlock != nullptr);
        return *phashBlock;
    }

    /**
     * Reconstruct the full block header
     * @return Self-contained CBlockHeader with all fields copied
     * @note Safe to use even if this CBlockIndex is later destroyed
     */
    [[nodiscard]] CBlockHeader GetBlockHeader() const
    {
        CBlockHeader block;
        block.nVersion = nVersion;
        if (pprev)
            block.hashPrevBlock = pprev->GetBlockHash();
        block.minerAddress = minerAddress;
        block.nTime = nTime;
        block.nBits = nBits;
        block.nNonce = nNonce;
        block.hashRandomX = hashRandomX;
        return block;
    }

    /**
     * Get block timestamp as signed 64-bit Unix time
     * @return Block timestamp
     */
    [[nodiscard]] int64_t GetBlockTime() const noexcept
    {
        return static_cast<int64_t>(nTime);
    }

    /**
     * Calculate Median Time Past (MTP)
     * Used for timestamp validation (new block time must be > MTP)
     *
     * Takes median of last MEDIAN_TIME_SPAN blocks (11) or fewer if near genesis
     *
     * @return Median timestamp of up to last 11 blocks
     * @note Complexity: O(MEDIAN_TIME_SPAN log MEDIAN_TIME_SPAN) = O(11 log 11) ≈ O(1)
     */
    [[nodiscard]] int64_t GetMedianTimePast() const
    {
        int64_t pmedian[MEDIAN_TIME_SPAN];
        int64_t* pbegin = &pmedian[MEDIAN_TIME_SPAN];
        int64_t* pend = &pmedian[MEDIAN_TIME_SPAN];

        const CBlockIndex* pindex = this;
        for (int i = 0; i < MEDIAN_TIME_SPAN && pindex; i++, pindex = pindex->pprev)
            *(--pbegin) = pindex->GetBlockTime();

        std::sort(pbegin, pend);
        return pbegin[(pend - pbegin) / 2];
    }

    /**
     * Get an ancestor at a given height
     *
     * Simple implementation that walks pprev pointers.
     * TODO: Add skip list (pskip) for O(log n) instead of O(n) performance.
     *
     * @param height Target height to find (must be <= nHeight)
     * @return Pointer to ancestor at target height, or nullptr if invalid height
     * @note Complexity: O(n) where n = (nHeight - height). Can be O(log n) with skip list.
     */
    [[nodiscard]] const CBlockIndex* GetAncestor(int height) const
    {
        if (height > nHeight || height < 0)
            return nullptr;

        const CBlockIndex* pindex = this;
        while (pindex && pindex->nHeight > height)
            pindex = pindex->pprev;

        return pindex;
    }

    /**
     * Get an ancestor at a given height (non-const overload)
     * @param height Target height to find
     * @return Mutable pointer to ancestor, or nullptr if invalid height
     */
    [[nodiscard]] CBlockIndex* GetAncestor(int height)
    {
        return const_cast<CBlockIndex*>(
            static_cast<const CBlockIndex*>(this)->GetAncestor(height));
    }

    /**
     * Check if this block is valid up to a certain level
     * @param nUpTo Validation level to check (default: BLOCK_VALID_TREE for headers)
     * @return true if block has been validated to at least nUpTo level
     */
    [[nodiscard]] bool IsValid(enum BlockStatus nUpTo = BLOCK_VALID_TREE) const noexcept
    {
        assert(!(nUpTo & ~BLOCK_VALID_MASK)); // Only validity flags allowed
        if (nStatus & BLOCK_FAILED_MASK)
            return false;
        return ((nStatus & BLOCK_VALID_MASK) >= nUpTo);
    }

    /**
     * Raise the validity level of this block
     * @param nUpTo New validation level
     * @return true if validity was changed
     */
    [[nodiscard]] bool RaiseValidity(enum BlockStatus nUpTo) noexcept
    {
        assert(!(nUpTo & ~BLOCK_VALID_MASK)); // Only validity flags allowed
        if (nStatus & BLOCK_FAILED_MASK)
            return false;

        if ((nStatus & BLOCK_VALID_MASK) < nUpTo) {
            nStatus = (nStatus & ~BLOCK_VALID_MASK) | nUpTo;
            return true;
        }
        return false;
    }

    /**
     * Get human-readable string representation for debugging
     * @return String with key fields (pprev, nHeight, minerAddress, hashBlock)
     */
    [[nodiscard]] std::string ToString() const;

    /**
     * Copy/move operations are DELETED to prevent dangling pointer bugs.
     *
     * Rationale:
     * - phashBlock points to the std::map key that owns this CBlockIndex
     * - Copying would create a CBlockIndex with phashBlock pointing to the
     *   original map entry, which becomes dangling if the original is deleted
     * - pprev also points to map-owned memory with same lifetime concerns
     *
     * This is safe because:
     * - std::map::emplace constructs CBlockIndex in-place (no copy/move needed)
     * - CBlockIndex is always used by pointer/reference (never by value)
     * - BlockManager owns all CBlockIndex instances for their entire lifetime
     *
     * If you need to extract block data, use GetBlockHeader() which returns
     * a self-contained CBlockHeader with all fields copied.
     */
    CBlockIndex(const CBlockIndex&) = delete;
    CBlockIndex& operator=(const CBlockIndex&) = delete;
    CBlockIndex(CBlockIndex&&) = delete;
    CBlockIndex& operator=(CBlockIndex&&) = delete;
};

/**
 * Calculate proof-of-work for a block
 *
 * Returns the "work" represented by this block's nBits difficulty target.
 * Work is defined as: 2^256 / (target + 1)
 *
 * To avoid overflow (can't represent 2^256 in arith_uint256), we use:
 * work = ~target / (target + 1) + 1
 *
 * This is mathematically equivalent because:
 * 2^256 = ((2^256 - target - 1) / (target + 1)) + 1
 *       = (~target / (target + 1)) + 1
 *
 * Invalid targets (negative, overflow, or zero) return 0 work.
 *
 * @param block Block to calculate work for
 * @return Work as 256-bit unsigned integer (0 if invalid nBits)
 * @note Used to calculate cumulative nChainWork during block acceptance
 * @note Algorithm from Bitcoin Core (identical formula)
 */
[[nodiscard]] arith_uint256 GetBlockProof(const CBlockIndex& block);

/**
 * Find the last common ancestor of two blocks
 *
 * Algorithm:
 * 1. Align both chains to the same height using GetAncestor()
 * 2. Walk both chains backward (via pprev) until they meet
 * 3. All valid chains share the genesis block, so they must eventually meet
 *
 * Example:
 *   Genesis -> A -> B -> C -> D     (main chain)
 *                    \-> E -> F     (fork)
 *
 *   LastCommonAncestor(D, F) = B
 *   LastCommonAncestor(C, E) = B
 *   LastCommonAncestor(A, F) = A
 *
 * @param pa First block (can be nullptr)
 * @param pb Second block (can be nullptr)
 * @return Pointer to common ancestor, or nullptr if either input is nullptr
 * @note Asserts that both chains eventually meet (debugging check)
 * @note Complexity: O(height difference + distance to common ancestor)
 */
[[nodiscard]] const CBlockIndex* LastCommonAncestor(const CBlockIndex* pa, const CBlockIndex* pb);

} // namespace chain
} // namespace coinbasechain

#endif // COINBASECHAIN_CHAIN_BLOCK_INDEX_HPP
