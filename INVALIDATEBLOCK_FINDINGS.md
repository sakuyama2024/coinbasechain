# InvalidateBlock Test Failure Analysis

## What I Learned from Bitcoin Core

### 1. Bitcoin Core DOES NOT call ActivateBestChain() after InvalidateBlock()
- Confirmed by reading Bitcoin Core's implementation (validation.cpp)
- InvalidateBlock() only:
  - Disconnects blocks back to parent of invalidated block
  - Marks blocks as invalid
  - Updates candidate set with competing forks
  - Updates mempool (via MaybeUpdateMempoolForReorg)
  - Sends blockTip notification

### 2. Bitcoin Core Test Behavior (feature_bip68_sequence.py line 324)
```python
self.nodes[0].invalidateblock(self.nodes[0].getbestblockhash())
assert tx4.hash not in self.nodes[0].getrawmempool()
assert tx3.hash in self.nodes[0].getrawmempool()
```
- After invalidateblock(), mempool IS updated
- But this is due to MaybeUpdateMempoolForReorg(), not ActivateBestChain()
- **The tip stays at the rewound state** (parent of invalidated block)
- **No automatic switch to competing fork happens**

### 3. Our Implementation
- Correctly follows Bitcoin Core pattern:
  - Pre-builds candidate map ✅
  - Adds competing forks during disconnect loop ✅
  - Final cleanup to catch new blocks ✅
  - Does NOT call ActivateBestChain() ✅

### 4. Our Test Failures

#### Test 1: "Invalidate with fork"
- **Setup**: Main chain A->B->C, fork A->D->E
- **Action**: InvalidateBlock(B), then ActivateBestChain()
- **Expected**: Tip switches to blockE (height 3)
- **Actual**: Tip stays at blockA (height 1)
- **Issue**: ActivateBestChain() is not finding/switching to blockE

#### Test 2: "Invalidate then mine new chain"
- **Setup**: Chain A->B->C
- **Action**: InvalidateBlock(B), then mine new blocks D, E, F on top of A
- **Expected**: New chain A->D->E->F becomes active
- **Actual**: ProcessNewBlockHeader() fails when mining blockD
- **Issue**: Something is rejecting the first new block after invalidation

#### Test 3: "Deep fork invalidation"
- **Setup**: Main chain 1->2->3->4->5, fork from 2: F1->F2->F3->F4->F5->F6 (active)
- **Action**: InvalidateBlock(F3), then ActivateBestChain()
- **Expected**: Reorg back to main chain (height 5)
- **Actual**: Tip at height 4 instead of 5
- **Issue**: ActivateBestChain() not finding the full main chain

## Root Cause Hypothesis

The common issue across all failures: **ActivateBestChain() is not finding the right candidate**.

Possible causes:
1. **Competing fork blocks not added to candidates properly** during InvalidateBlock()
2. **Candidate set contains non-leaf blocks** (e.g., blockA which has blockD as a child)
3. **FindMostWorkChain() skipping valid candidates** due to some check
4. **Blocks on competing forks accidentally marked as invalid** during descendant marking

## Key Question

In our implementation, after InvalidateBlock(blockB):
- Is blockE in the candidate set?
- Is blockA also in the candidate set (incorrectly, since it's not a leaf)?
- Does FindMostWorkChain() return blockE or blockA?

## Next Steps (Awaiting Decision)

**Option A**: Add debug logging to InvalidateBlock() and ActivateBestChain() to see:
- What candidates are added during InvalidateBlock()
- What FindMostWorkChain() returns
- Why ActivateBestChain() isn't switching

**Option B**: Fix the candidate management issue:
- After adding competing forks, remove any non-leaf blocks from candidates
- Ensure only true leaf nodes are candidates

**Option C**: Review the final cleanup logic:
- Check if the criteria `block.nChainWork >= current_tip->nChainWork` is correct
- Bitcoin Core uses `!setBlockIndexCandidates.value_comp()(&block_index, m_chain.Tip())`

**Option D**: Look at Bitcoin Core's unit tests (not functional tests) to see how they test InvalidateBlock
