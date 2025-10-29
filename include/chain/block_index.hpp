// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_CHAIN_BLOCK_INDEX_HPP
#define COINBASECHAIN_CHAIN_BLOCK_INDEX_HPP

#include "chain/arith_uint256.hpp"
#include "chain/block.hpp"
#include "chain/uint.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>

namespace coinbasechain {
namespace chain {

// Median Time Past calculation span (number of previous blocks)
// Used by GetMedianTimePast() and also defined in validation.hpp
static constexpr int MEDIAN_TIME_SPAN = 11;

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

  //! All validity bits
  BLOCK_VALID_MASK = BLOCK_VALID_HEADER | BLOCK_VALID_TREE,

  BLOCK_FAILED_VALID = 32, //! Stage after last reached validity failed
  BLOCK_FAILED_CHILD = 64, //! Descends from failed block
  BLOCK_FAILED_MASK = BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD,
};

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

  // Reconstruct full block header (self-contained, safe to use if CBlockIndex
  // destroyed)
  [[nodiscard]] CBlockHeader GetBlockHeader() const {
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

  // Get ancestor at given height (walks pprev pointers, O(n)
  // list for O(log n))
  [[nodiscard]] const CBlockIndex *GetAncestor(int height) const {
    if (height > nHeight || height < 0)
      return nullptr;

    const CBlockIndex *pindex = this;
    while (pindex && pindex->nHeight > height)
      pindex = pindex->pprev;

    return pindex;
  }

  [[nodiscard]] CBlockIndex *GetAncestor(int height) {
    if (height > nHeight || height < 0)
      return nullptr;

    CBlockIndex *pindex = this;
    while (pindex && pindex->nHeight > height)
      pindex = pindex->pprev;

    return pindex;
  }

  [[nodiscard]] bool
  IsValid(enum BlockStatus nUpTo = BLOCK_VALID_TREE) const noexcept {
    assert(!(nUpTo & ~BLOCK_VALID_MASK)); // Only validity flags allowed
    if (nStatus & BLOCK_FAILED_MASK)
      return false;
    return ((nStatus & BLOCK_VALID_MASK) >= nUpTo);
  }

  // Raise validity level of this block, returns true if changed
  [[nodiscard]] bool RaiseValidity(enum BlockStatus nUpTo) noexcept {
    assert(!(nUpTo & ~BLOCK_VALID_MASK)); // Only validity flags allowed
    if (nStatus & BLOCK_FAILED_MASK)
      return false;

    if ((nStatus & BLOCK_VALID_MASK) < nUpTo) {
      nStatus = (nStatus & ~BLOCK_VALID_MASK) | nUpTo;
      return true;
    }
    return false;
  }

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

#endif // COINBASECHAIN_CHAIN_BLOCK_INDEX_HPP
