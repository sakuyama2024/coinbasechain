# Advanced Reorg Attack & Edge Case Scenarios

These scenarios can be tested with the SimulatedNetwork harness to ensure robust consensus behavior.

## 1. Selfish Mining / Block Withholding Attacks

### Selfish Mining Attack
**Scenario**: Attacker mines blocks privately then releases them strategically to orphan honest miners' blocks.

```
1. Attacker builds private chain (3 blocks ahead)
2. Honest miner finds block, broadcasts
3. Attacker immediately releases private chain (more work)
4. Honest block gets orphaned
5. Attacker gains unfair advantage
```

**Test**: Verify victim switches to longer chain but measure orphan rate increase.

### Block Withholding Race
**Scenario**: Two attackers race to release competing chains at exact same moment.

```
1. Attacker A builds private chain (10 blocks)
2. Attacker B builds different private chain (10 blocks, same height)
3. Both release simultaneously to victim
4. First-seen rule should apply (deterministic in simulation)
```

**Test**: Verify tie-breaking is consistent and predictable.

## 2. Time-Based Reorg Attacks

### Future Block Timestamps
**Scenario**: Attacker mines chain with timestamps 2 hours in the future (MAX_FUTURE_BLOCK_TIME limit).

```
1. Attacker mines 50 blocks with timestamps at edge of acceptance window
2. Victim sees longer chain but blocks are "in the future"
3. After 2+ hours, blocks become valid
4. Delayed reorg occurs
```

**Test**: Verify future blocks are rejected initially but accepted after time passes.

### Time Warp Attack
**Scenario**: Manipulate difficulty adjustment by mining blocks with manipulated timestamps.

```
1. Attacker mines blocks with minimum allowed timestamps
2. Difficulty adjusts downward due to "slow" block times
3. Attacker can mine faster on easier difficulty
4. Builds longer chain with less actual work
```

**Test**: Verify difficulty adjustment limits prevent this.

## 3. Memory Exhaustion via Reorgs

### Reorg Spam Attack
**Scenario**: Force victim to repeatedly reorg by alternating between two chains.

```
1. Attacker A announces chain with height 100
2. Victim reorgs to chain A
3. Attacker B announces different chain with height 101
4. Victim reorgs to chain B
5. Attacker A extends to height 102
6. Repeat 1000x to exhaust victim's resources
```

**Test**: Verify limits on reorg frequency or depth prevent DoS.

### Massive Reorg DoS
**Scenario**: Force reorg of 10,000+ blocks to consume CPU/memory.

```
1. Victim has chain at height 10,000
2. Attacker presents alternative chain from block 1
3. Alternative chain is 10,001 blocks (1 more)
4. Victim must validate all 10,001 blocks
5. Massive CPU/memory consumption
```

**Test**: Verify reorg depth limits or checkpoints prevent this.

## 4. Network Partition Exploits

### Double-Spend via Partition
**Scenario**: Create conflicting transactions on both sides of partition.

```
1. Network partitions: Group A (3 nodes) vs Group B (3 nodes)
2. Attacker mines tx1 (send to merchant) on Group A
3. Attacker mines tx2 (send to self) on Group B, same UTXO
4. Partition heals
5. Longer chain wins, one transaction gets orphaned
6. Double-spend succeeds
```

**Test**: Headers-only chain can't fully test this (needs UTXO), but can verify reorg mechanics.

### Partition Thrashing
**Scenario**: Rapidly partition and heal network to cause constant reorgs.

```
1. Split network 50/50
2. Both sides mine 5 blocks
3. Heal partition (reorg occurs)
4. Immediately partition again differently
5. Repeat 100x
6. Nodes constantly reorging, can't stabilize
```

**Test**: Verify nodes eventually stabilize and converge.

## 5. Edge Cases & Race Conditions

### Simultaneous Competing Blocks
**Scenario**: Two miners find blocks at same height at exact same time.

```
1. Miner A finds block at height 100, timestamp T
2. Miner B finds block at height 100, timestamp T (same second)
3. Different nodes see different blocks first
4. Network splits temporarily
5. Next block resolves tie
```

**Test**: Verify deterministic tie-breaking (first-seen) and eventual convergence.

### Reorg During Reorg
**Scenario**: Victim starts reorging, but receives even longer chain mid-reorg.

```
1. Victim at height 100 (chain A)
2. Receives chain B (height 105) - starts reorging
3. Mid-reorg, receives chain C (height 110)
4. Must abort reorg to chain B, start reorg to chain C
```

**Test**: Verify state consistency during nested reorgs.

### Circular Reorg Prevention
**Scenario**: Three competing chains create circular preference.

```
1. Chain A beats Chain B (more work)
2. Chain B beats Chain C (more work)
3. Chain C beats Chain A (more work)
4. Impossible circular preference
```

**Test**: Verify this can't happen (total work ordering is transitive).

## 6. Checkpoint & Reorg Limit Tests

### Checkpoint Bypass Attempt
**Scenario**: Attacker tries to reorg past a hardcoded checkpoint.

```
1. Victim has chain with checkpoint at height 1000
2. Attacker presents alternative chain from genesis
3. Alternative chain is 2000 blocks (more work)
4. But conflicts with checkpoint at 1000
```

**Test**: Verify checkpoint prevents reorg past that point.

### Maximum Reorg Depth
**Scenario**: Test the maximum allowed reorg depth.

```
1. Victim at height 1000
2. Attacker presents chain from height 500 (500 block reorg)
3. If > MAX_REORG_DEPTH, should reject
```

**Test**: Verify configurable reorg depth limit works.

## 7. Resource Exhaustion via Headers

### Header Flooding Different Chains
**Scenario**: Attacker sends multiple competing header chains to exhaust memory.

```
1. Attacker sends chain A: 100 blocks from genesis
2. Attacker sends chain B: 101 blocks from genesis (different)
3. Attacker sends chain C: 102 blocks from genesis (different)
4. ... send 100 different chains
5. Victim stores all 10,000+ headers
```

**Test**: Verify limits on cached alternative chain headers.

### Deep Fork Tree
**Scenario**: Create tree of forks to exhaust block index memory.

```
1. Attacker creates fork at block 10
2. Then fork at block 11 (on original chain)
3. Then fork at block 12
4. ... 1000 forks at different heights
5. Block index tree explodes in size
```

**Test**: Verify limits on stored block index entries.

## 8. Stale Block Propagation

### Stale Tip Race
**Scenario**: Victim receives stale block announcement after already moving to new tip.

```
1. Miner A finds block 100
2. Miner B finds block 100 (different, at same time)
3. Victim sees A's block first, updates tip
4. Late announcement of B's block arrives
5. Victim should ignore (already have block at that height)
```

**Test**: Verify stale blocks don't cause unnecessary work.

### Delayed Block Announcement
**Scenario**: Block announcement arrives 30 seconds after it was mined.

```
1. Miner finds block at T=0
2. Network delay causes announcement at T=30s
3. By then, another block was found on top
4. Delayed block is now part of shorter chain
5. Should be ignored
```

**Test**: Verify old announcements don't disrupt current tip.

## Implementation Priority

### High Priority (Security Critical)
1. Selfish Mining Attack
2. Reorg Spam Attack
3. Massive Reorg DoS
4. Header Flooding Different Chains

### Medium Priority (Robustness)
5. Reorg During Reorg
6. Simultaneous Competing Blocks
7. Maximum Reorg Depth
8. Future Block Timestamps

### Low Priority (Edge Cases)
9. Block Withholding Race
10. Stale Tip Race
11. Deep Fork Tree
12. Circular Reorg Prevention (should be mathematically impossible)

## Testing Approach

Each test should verify:
- ✅ Victim doesn't crash
- ✅ Memory usage stays bounded
- ✅ Reorg completes successfully (or is rejected correctly)
- ✅ Attacker gets banned if behavior is malicious
- ✅ Honest nodes converge to same chain
- ✅ Performance is acceptable (reorg time, CPU usage)

## Example Test Structure

```cpp
TEST(AdvancedReorgTest, SelfishMiningAttack) {
    SimulatedNetwork network(seed);
    SetZeroLatency(network);

    SimulatedNode victim(1, &network);
    AttackSimulatedNode selfish_miner(2, &network);

    // Victim builds public chain
    for (int i = 0; i < 50; i++) {
        victim.MineBlock();
    }

    // Selfish miner connects and syncs
    selfish_miner.ConnectTo(1);
    network.AdvanceTime(1000);

    // Selfish miner builds PRIVATE chain (3 blocks ahead)
    // Don't broadcast these blocks yet
    uint256 private_chain[3];
    for (int i = 0; i < 3; i++) {
        private_chain[i] = selfish_miner.MineBlockPrivate();
    }

    // Victim mines public block
    victim.MineBlock();  // Block 51
    network.AdvanceTime(1000);

    // Selfish miner releases private chain (3 blocks)
    selfish_miner.BroadcastPrivateChain();
    network.AdvanceTime(2000);

    // Victim should reorg to selfish chain (54 blocks vs 51)
    EXPECT_EQ(victim.GetTipHeight(), 53);  // 50 + 3 private blocks

    // Honest block at 51 got orphaned
    EXPECT_FALSE(victim.HasBlock(victim's_block_51));
}
```
