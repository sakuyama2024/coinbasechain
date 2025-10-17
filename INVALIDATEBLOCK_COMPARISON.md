# InvalidateBlock Implementation Comparison: Bitcoin Core vs Our Implementation

## Critical Analysis - DO NOT PROCEED WITHOUT REVIEW

This document compares Bitcoin Core's `InvalidateBlock()` implementation against our current implementation to ensure correctness.

## Bitcoin Core Implementation (from ~/Code/alpha-release/src/validation.cpp:3468-3620)

### Key Steps:

1. **Pre-build candidate map** (lines 3493-3506)
   ```cpp
   std::multimap<const arith_uint256, CBlockIndex *> candidate_blocks_by_work;
   for (auto& entry : m_blockman.m_block_index) {
       CBlockIndex* candidate = &entry.second;
       if (!m_chain.Contains(candidate) &&
           !CBlockIndexWorkComparator()(candidate, pindex->pprev) &&
           candidate->IsValid(BLOCK_VALID_TRANSACTIONS) &&
           candidate->HaveNumChainTxs()) {
           candidate_blocks_by_work.insert(std::make_pair(candidate->nChainWork, candidate));
       }
   }
   ```
   **Purpose**: Find all competing fork blocks that have at least as much work as where we'll end up

2. **Disconnect loop** (lines 3508-3553)
   ```cpp
   while (true) {
       if (!m_chain.Contains(pindex)) break;

       CBlockIndex *invalid_walk_tip = m_chain.Tip();
       DisconnectTip(state, &disconnectpool);

       // Mark as invalid
       invalid_walk_tip->nStatus |= BLOCK_FAILED_VALID;

       // Update candidates
       setBlockIndexCandidates.erase(invalid_walk_tip);
       setBlockIndexCandidates.insert(invalid_walk_tip->pprev);

       // ADD COMPETING FORKS as they become viable
       auto candidate_it = candidate_blocks_by_work.lower_bound(invalid_walk_tip->pprev->nChainWork);
       while (candidate_it != candidate_blocks_by_work.end()) {
           if (!CBlockIndexWorkComparator()(candidate_it->second, invalid_walk_tip->pprev)) {
               setBlockIndexCandidates.insert(candidate_it->second);
               candidate_it = candidate_blocks_by_work.erase(candidate_it);
           } else {
               ++candidate_it;
           }
       }
   }
   ```
   **Critical**: As we disconnect, we add competing fork candidates that now have viable work

3. **Final cleanup** (lines 3565-3575)
   ```cpp
   for (auto& [_, block_index] : m_blockman.m_block_index) {
       if (block_index.IsValid(BLOCK_VALID_TRANSACTIONS) &&
           block_index.HaveNumChainTxs() &&
           !setBlockIndexCandidates.value_comp()(&block_index, m_chain.Tip())) {
           setBlockIndexCandidates.insert(&block_index);
       }
   }
   ```
   **Purpose**: Catch any blocks that arrived during invalidation

4. **NO ActivateBestChain() call**
   - The tip is already correct after disconnecting
   - Candidates have been properly added
   - The next `ProcessNewBlockHeader()` will naturally activate the best chain

## Our Current Implementation Issues

### What We're Doing Right:
✅ Genesis check (height == 0)
✅ Disconnect loop until pindex not in chain
✅ Mark blocks as BLOCK_FAILED_VALID during disconnect
✅ Add parent as candidate after each disconnect
✅ Mark descendants as BLOCK_FAILED_CHILD

### What We're Missing/Wrong:

❌ **CRITICAL**: We do NOT pre-build the candidate_blocks_by_work map
   - This means competing forks are NOT added during the disconnect loop
   - Bitcoin Core adds them incrementally as they become viable

❌ **CRITICAL**: We do NOT add competing fork candidates during the disconnect loop
   - Bitcoin Core does this at lines 3540-3548
   - This is essential for reorg to competing forks

❌ **WRONG**: Our final cleanup loop has wrong criteria
   - Bitcoin Core: `!setBlockIndexCandidates.value_comp()(&block_index, m_chain.Tip())`
   - Us: `block.nChainWork >= current_tip->nChainWork`
   - These are NOT equivalent!

❌ **MAJOR ERROR**: We added `ActivateBestChain()` call
   - Bitcoin Core does NOT do this
   - This could cause unexpected reorgs
   - **THIS MUST BE REMOVED**

## Why Bitcoin Core's Approach Works

After `InvalidateBlock()` completes:
1. Tip has been rewound to parent of invalidated block
2. All competing fork candidates have been added to `setBlockIndexCandidates`
3. The candidate set now contains all viable tips

When the next block arrives (or when we call ActivateBestChain from elsewhere):
- It will see the competing fork candidates
- It will naturally switch to the best one
- No explicit ActivateBestChain() needed in InvalidateBlock()

## Test Failures Analysis

Our tests expect immediate reorg to competing forks, but:
- Bitcoin Core does NOT immediately reorg
- It just sets up the candidates
- The reorg happens on the next block acceptance or manual ActivateBestChain()

**Question**: Should our tests be different, or should we add ActivateBestChain()?

## Recommendation

**STOP** - We need to decide:

Option A: **Follow Bitcoin Core exactly**
- Remove ActivateBestChain() call
- Implement the candidate_blocks_by_work map
- Add competing forks during disconnect loop
- Fix the final cleanup criteria
- **Adjust our tests** to not expect immediate reorg

Option B: **Diverge from Bitcoin Core**
- Keep ActivateBestChain() call
- Document why we diverge
- Understand the implications
- Risk: Different behavior from Bitcoin Core

**My recommendation**: Option A - Follow Bitcoin Core exactly. Their code has been battle-tested for years.

## Next Steps

1. Review this document
2. Decide on Option A or B
3. If Option A: Implement the missing pieces properly
4. If Option B: Document the divergence and risks
5. Run tests
6. Compare behavior against Bitcoin Core test cases

---

**DO NOT PROCEED WITH CODE CHANGES UNTIL THIS IS REVIEWED**
