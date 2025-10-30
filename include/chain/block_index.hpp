// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#pragma once

#include "chain/arith_uint256.hpp"
#include "chain/block.hpp"
#include "chain/uint.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>

namespace coinbasechain {
namespace chain {

// Median Time Past calculation span (number of previous blocks)
// Used by GetMedianTimePast()
static constexpr int MEDIAN_TIME_SPAN = 11;
static_assert(MEDIAN_TIME_SPAN % 2 == 1, "MEDIAN_TIME_SPAN must be odd for proper median calculation");

// Block validation status - tracks how far a block header has been validated
// Headers-only chain - no transaction/script validation levels
enum BlockStatus : uint32_t {
  //! Unused/unknown
  BLOCK_VALID_UNKNOWN = 0,

  //! Parsed, has valid POW, valid difficulty, valid timestamp
  BLOCK_VALID_HEADER = 1,

  //! All parent headers found, difficulty matches, timestamp >= median previous
  //! Implies all parents are also at least TREE
  //! This is the highest validation level for headers-only chain
  BLOCK_VALID_TREE = 2,

  BLOCK_FAILED_VALID = 32, //! Stage after last reached validity failed
  BLOCK_FAILED_CHILD = 64, //! Descends from failed block
  BLOCK_FAILED_MASK = BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD,
};

// Enforce validity levels are sequential integers, not bitflags
// IsValid() and RaiseValidity() use numeric comparison (< and >=)
static_assert(BLOCK_VALID_HEADER == 1 && BLOCK_VALID_TREE == 2,
              "Validity levels must be sequential integers (not bitflags) for comparison logic in IsValid()");

// Mask for extracting validity level from nStatus (low byte, values 0-2)
// Validity levels occupy low bits, failure flags use high bits (32, 64)
static constexpr uint32_t VALIDITY_LEVEL_MASK = 0xFF;

// CBlockIndex - Metadata for a single block header
// Simplified from Bitcoin Core for headers-only chain (no transaction counts,
// file positions, skip list, or sequence ID). Header data is stored inline.
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
   *
   * CRITICAL: Requires pointer stability - BlockManager MUST use std::map
   * (or equivalent node-based container). Do NOT change to std::unordered_map
   * as rehashing would invalidate all phashBlock pointers.
   */
  const uint256 *phashBlock{nullptr};

  /**
   * Pointer to previous block in chain (DOES NOT OWN).
   *
   * Forms the blockchain tree structure by linking to parent.
   * Lifetime: Points to CBlockIndex owned by BlockManager's map.
   *
   * nullptr for genesis block, otherwise points to parent block's CBlockIndex.
   * All CBlockIndex instances share the same lifetime (owned by BlockManager).
   */
  CBlockIndex *pprev{nullptr};

  /**
   * Pointer to ancestor for efficient chain traversal (DOES NOT OWN).
   *
   * Skip list pointer for O(log n) ancestor lookup (Bitcoin Core pattern).
   * Points to an ancestor at a strategically chosen height to enable
   * logarithmic-time traversal. The skip pattern ensures any ancestor
   * can be reached in O(log n) jumps instead of O(n) using pprev alone.
   *
   * Set by BuildSkip() when block is added to the chain.
   * Lifetime: Points to CBlockIndex owned by BlockManager's map.
   */
  CBlockIndex *pskip{nullptr};

  // Height of this block in the chain (genesis = 0)
  int nHeight{0};

  // Cumulative work up to and including this block
  arith_uint256 nChainWork{};

  // Block header fields (stored inline)
  int32_t nVersion{0};
  uint160 minerAddress{}; // Default-initialized (SetNull())
  uint32_t nTime{0};
  uint32_t nBits{0};
  uint32_t nNonce{0};
  uint256 hashRandomX{}; // Default-initialized (SetNull())

  // Block relay tracking (Bitcoin Core behavior)
  // Time when we first learned about this block (for relay decisions)
  // Blocks received recently (< MAX_BLOCK_RELAY_AGE) are relayed to peers
  // Old blocks (from disk/reorgs) are not relayed (peers already know them)
  int64_t nTimeReceived{0};

  // Monotonic maximum of nTime up to and including this block (Core: nTimeMax)
  // Ensures time is non-decreasing along the chain for binary searches.
  int64_t nTimeMax{0};

  // Constructor
  CBlockIndex() = default;

  explicit CBlockIndex(const CBlockHeader &block)
      : nVersion{block.nVersion}, minerAddress{block.minerAddress},
        nTime{block.nTime}, nBits{block.nBits}, nNonce{block.nNonce},
        hashRandomX{block.hashRandomX} {}

  // Returns block hash (asserts phashBlock is non-null)
  [[nodiscard]] uint256 GetBlockHash() const noexcept {
    assert(phashBlock != nullptr);
    return *phashBlock;
  }

  // Reconstruct full block header (self-contained, safe to use if CBlockIndex destroyed)
  [[nodiscard]] CBlockHeader GetBlockHeader() const noexcept {
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

  [[nodiscard]] int64_t GetBlockTime() const noexcept {
    return static_cast<int64_t>(nTime);
  }

  // CONSENSUS-CRITICAL: Calculate Median Time Past (MTP) for timestamp
  // validation Takes median of last MEDIAN_TIME_SPAN blocks (11) or fewer if
  // near genesis New block time must be > MTP
  [[nodiscard]] int64_t GetMedianTimePast() const {
    int64_t pmedian[MEDIAN_TIME_SPAN];
    int64_t *pbegin = &pmedian[MEDIAN_TIME_SPAN];
    int64_t *pend = &pmedian[MEDIAN_TIME_SPAN];

    const CBlockIndex *pindex = this;
    for (int i = 0; i < MEDIAN_TIME_SPAN && pindex; i++, pindex = pindex->pprev)
      *(--pbegin) = pindex->GetBlockTime();

    std::sort(pbegin, pend);
    return pbegin[(pend - pbegin) / 2];
  }

  // Build skip list pointer (Bitcoin Core algorithm)
  // Must be called when adding block to chain, after pprev and nHeight are set
  void BuildSkip();

  // Get ancestor at given height using skip list (O(log n) with skip list)
  [[nodiscard]] const CBlockIndex *GetAncestor(int height) const;
  [[nodiscard]] CBlockIndex *GetAncestor(int height);

  [[nodiscard]] bool
  IsValid(enum BlockStatus nUpTo = BLOCK_VALID_TREE) const noexcept {
    assert(nUpTo <= BLOCK_VALID_TREE); // Only validity levels (0-2) allowed
    if (nStatus & BLOCK_FAILED_MASK)
      return false;
    return ((nStatus & VALIDITY_LEVEL_MASK) >= nUpTo);
  }

  // Raise validity level of this block, returns true if changed
  [[nodiscard]] bool RaiseValidity(enum BlockStatus nUpTo) noexcept {
    assert(nUpTo <= BLOCK_VALID_TREE); // Only validity levels (0-2) allowed
    if (nStatus & BLOCK_FAILED_MASK)
      return false;

    if ((nStatus & VALIDITY_LEVEL_MASK) < nUpTo) {
      nStatus = (nStatus & ~VALIDITY_LEVEL_MASK) | nUpTo;
      return true;
    }
    return false;
  }

  // For debugging/testing only - produces human-readable representation
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
  CBlockIndex(const CBlockIndex &) = delete;
  CBlockIndex &operator=(const CBlockIndex &) = delete;
  CBlockIndex(CBlockIndex &&) = delete;
  CBlockIndex &operator=(CBlockIndex &&) = delete;
};

// CONSENSUS-CRITICAL: Calculate proof-of-work for a block
// Returns work = ~target / (target + 1) + 1 (mathematically equivalent to 2^256
// / (target + 1)) Invalid targets return 0 work. 
[[nodiscard]] arith_uint256 GetBlockProof(const CBlockIndex &block);

// Find last common ancestor of two blocks (aligns heights, then walks backward
// until they meet) Returns nullptr if either input is nullptr. All valid chains
// share genesis.
[[nodiscard]] const CBlockIndex *LastCommonAncestor(const CBlockIndex *pa,
                                                    const CBlockIndex *pb);

} // namespace chain
} // namespace coinbasechain


