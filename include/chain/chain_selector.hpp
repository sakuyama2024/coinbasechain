// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_VALIDATION_CHAIN_SELECTOR_HPP
#define COINBASECHAIN_VALIDATION_CHAIN_SELECTOR_HPP

#include "chain/block_index.hpp"
#include "chain/block_manager.hpp"
#include <set>

namespace coinbasechain {
namespace validation {

// Comparator for sorting block indices by chain work (strict weak ordering for
// std::set) Order: 1) Higher chain work, 2) Higher height, 3) Lexicographic
// hash (deterministic tie-breaker)
//
// CRITICAL INVARIANT: nChainWork and nHeight must NOT be modified after
// insertion into set These fields are set ONCE during creation and must remain
// immutable while in candidate set Thread-safe: comparator reads immutable
// fields only
struct CBlockIndexWorkComparator {
  bool operator()(const chain::CBlockIndex *pa,
                  const chain::CBlockIndex *pb) const;
};

// ChainSelector - Manages candidate tips and selects the best chain
// Maintains set of leaf nodes (validated to BLOCK_VALID_TREE) that could be
// chain tips Selects best chain by most accumulated work, prunes stale
// candidates
//
// THREAD SAFETY: No internal mutex - caller (ChainstateManager) must hold
// validation_mutex_
class ChainSelector {
public:
  ChainSelector() = default;

  // Find block with most work among candidates (first in sorted set)
  // Caller must hold validation_mutex_
  chain::CBlockIndex *FindMostWorkChain();

  // Try to add block index to candidate set
  // Added if: 1) validated to BLOCK_VALID_TREE, 2) is leaf node (no children)
  // When block extends candidate, parent is auto-removed (maintains leaf-only
  // invariant) Caller must hold validation_mutex_
  void TryAddBlockIndexCandidate(chain::CBlockIndex *pindex,
                                 const chain::BlockManager &block_manager);

  // Prune stale candidates (less work than tip, active tip itself, ancestors,
  // non-leaves) Caller must hold validation_mutex_
  void PruneBlockIndexCandidates(const chain::BlockManager &block_manager);

  // Add candidate without validation checks (used during Load)
  // Caller must hold validation_mutex_
  void AddCandidateUnchecked(chain::CBlockIndex *pindex);

  // Clear all candidates (used during Load)
  // Caller must hold validation_mutex_
  void ClearCandidates();

  // Get number of candidates (caller must hold validation_mutex_)
  size_t GetCandidateCount() const { return m_candidates.size(); }

  // Get best header seen (caller must hold validation_mutex_)
  chain::CBlockIndex *GetBestHeader() const { return m_best_header; }

  // Update best header if new block has more work (caller must hold
  // validation_mutex_)
  void UpdateBestHeader(chain::CBlockIndex *pindex);

  // Set best header (used during Load, caller must hold validation_mutex_)
  void SetBestHeader(chain::CBlockIndex *pindex) { m_best_header = pindex; }

  // Remove block from candidate set (caller must hold validation_mutex_)
  void RemoveCandidate(chain::CBlockIndex *pindex);

private:
  // Set of blocks that could be chain tips (sorted by descending chain work)
  std::set<chain::CBlockIndex *, CBlockIndexWorkComparator> m_candidates;

  // Best header we've seen (most chainwork, may not be on active chain)
  chain::CBlockIndex *m_best_header{nullptr};
};

} // namespace validation
} // namespace coinbasechain

#endif // COINBASECHAIN_VALIDATION_CHAIN_SELECTOR_HPP
