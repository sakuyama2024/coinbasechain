// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_VALIDATION_CHAIN_SELECTOR_HPP
#define COINBASECHAIN_VALIDATION_CHAIN_SELECTOR_HPP

#include "chain/block_index.hpp"
#include "chain/block_manager.hpp"
#include <set>

namespace coinbasechain {
namespace validation {

/**
 * Comparator for sorting block indices by chain work
 *
 * Provides strict weak ordering for std::set<CBlockIndex*>:
 * 1. Primary: Higher chain work comes first (descending)
 * 2. Secondary: Higher height comes first (descending)
 * 3. Tertiary: Lexicographic hash order (ascending, deterministic tie-breaker)
 *
 * CRITICAL INVARIANT: CBlockIndex fields used in comparison (nChainWork, nHeight)
 * must NOT be modified after insertion into the set. Modifying these fields would
 * violate std::set ordering and cause undefined behavior.
 *
 * ENFORCEMENT: This invariant is maintained by design:
 * - BlockManager::AddToBlockIndex sets nHeight/nChainWork ONCE during creation
 * - BlockManager::Load restores these values BEFORE ChainSelector adds to candidates
 * - No code path modifies these fields after block is in the candidate set
 * - CBlockIndex has no public setters for these fields
 *
 * If you add code that modifies nHeight or nChainWork after block creation,
 * you MUST erase from the candidate set before modification and reinsert after.
 *
 * Thread Safety: Comparator is const and reads immutable fields, safe for concurrent
 * use as long as the CBlockIndex objects aren't being modified during comparison.
 */
struct CBlockIndexWorkComparator {
    bool operator()(const chain::CBlockIndex* pa, const chain::CBlockIndex* pb) const;
};

/**
 * ChainSelector - Manages candidate tips and selects the best chain
 *
 * Responsibilities:
 * - Maintain set of candidate tips (blocks that could be chain tips)
 * - Select best chain based on most accumulated work
 * - Prune stale candidates after chain activation
 * - Track best header seen across all chains
 *
 * The candidate set contains only leaf nodes (blocks with no children) that
 * have been validated to at least BLOCK_VALID_TREE. This ensures efficient
 * chain selection by only considering actual competing tips.
 *
 * THREAD SAFETY:
 * ChainSelector does NOT have its own mutex. The caller (ChainstateManager)
 * must hold validation_mutex_ when calling any ChainSelector methods.
 */
class ChainSelector {
public:
    ChainSelector() = default;

    /**
     * Find the block with the most work among candidates
     *
     * Searches the candidate set for the block with highest chainwork.
     * The set is sorted by CBlockIndexWorkComparator, so the first valid
     * candidate is the best.
     *
     * @return Block index with highest chainwork, or nullptr if no valid candidates
     * @note Caller must hold validation_mutex_
     */
    chain::CBlockIndex* FindMostWorkChain();

    /**
     * Try to add a block index to the candidate set
     *
     * A block is added as a candidate if:
     * 1. It has been validated to at least BLOCK_VALID_TREE
     * 2. It is a potential chain tip (leaf node with no known children)
     *
     * CRITICAL INVARIANT: The candidate set must only contain leaf nodes
     * (blocks with no known descendants). When a block extends a candidate,
     * the parent candidate is automatically removed.
     *
     * Example:
     *   Initial: candidates = {A}
     *   Add block B extending A: candidates = {B}  (A removed)
     *   Add block C extending B: candidates = {C}  (B removed)
     *   Add block D extending A: candidates = {C, D}  (fork from A)
     *
     * This ensures the candidate set stays small and only contains
     * actual tips, not intermediate blocks.
     *
     * @param pindex Block to consider adding
     * @param block_manager Reference to block manager (for checking children)
     * @note Caller must hold validation_mutex_
     */
    void TryAddBlockIndexCandidate(chain::CBlockIndex* pindex,
                                    const chain::BlockManager& block_manager);

    /**
     * Prune stale candidates from the candidate set
     *
     * Removes blocks from the candidate set that should no longer
     * be considered as potential tips:
     *
     * 1. Blocks with less chainwork than the active tip (lost competition)
     * 2. The active tip itself (no longer competing)
     * 3. Any ancestor of the active tip (interior of active chain)
     * 4. Any block with children (not a leaf - defensive check)
     *
     * This ensures the candidate set remains clean and only contains
     * actual competing fork tips, preventing stale candidates from being
     * reconsidered during chain selection.
     *
     * @param block_manager Reference to block manager (for active chain and children checks)
     * @note Caller must hold validation_mutex_
     */
    void PruneBlockIndexCandidates(const chain::BlockManager& block_manager);

    /**
     * Add a candidate without validation checks (used during Load)
     *
     * Directly inserts a block into the candidate set without checking
     * if it's a leaf or validated. This is used when loading from disk
     * where we've already verified these properties.
     *
     * @param pindex Block to add
     * @note Caller must hold validation_mutex_
     */
    void AddCandidateUnchecked(chain::CBlockIndex* pindex);

    /**
     * Clear all candidates (used during Load)
     *
     * @note Caller must hold validation_mutex_
     */
    void ClearCandidates();

    /**
     * Get number of candidates in the set
     *
     * @return Candidate count
     * @note Caller must hold validation_mutex_
     */
    size_t GetCandidateCount() const { return m_candidates.size(); }

    /**
     * Get best header seen (most chainwork)
     *
     * @return Best header pointer or nullptr if none
     * @note Caller must hold validation_mutex_
     */
    chain::CBlockIndex* GetBestHeader() const { return m_best_header; }

    /**
     * Update best header if new block has more work
     *
     * @param pindex Block to consider
     * @note Caller must hold validation_mutex_
     */
    void UpdateBestHeader(chain::CBlockIndex* pindex);

    /**
     * Set best header (used during Load)
     *
     * @param pindex New best header
     * @note Caller must hold validation_mutex_
     */
    void SetBestHeader(chain::CBlockIndex* pindex) { m_best_header = pindex; }

private:
    /**
     * Set of blocks that could be chain tips
     * Sorted by descending chain work (most work first)
     *
     * This is how we track competing chains and find the best one.
     * When a new block arrives:
     * - If it extends active chain -> it's the new tip candidate
     * - If it's on a side chain -> it's added to candidates
     * - When we call FindMostWorkChain(), we pick the best candidate
     */
    std::set<chain::CBlockIndex*, CBlockIndexWorkComparator> m_candidates;

    /**
     * Best header we've seen (may not be on active chain)
     * This is the header with the most chainwork that we know about
     */
    chain::CBlockIndex* m_best_header{nullptr};
};

} // namespace validation
} // namespace coinbasechain

#endif // COINBASECHAIN_VALIDATION_CHAIN_SELECTOR_HPP
