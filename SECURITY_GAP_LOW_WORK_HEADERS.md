# Security Gap: Low-Work Header Defense Not Enforced

**Date Discovered**: 2025-10-19
**Severity**: MEDIUM
**Status**: UNPATCHED
**Discovered By**: Code audit during attack test review

## Summary

The codebase has **infrastructure for low-work header protection** but **never actually enforces the check**. This allows attackers to flood the node with valid-but-low-work headers without penalty.

## The Gap

### What EXISTS ✅

1. **Function defined** (`src/chain/validation.cpp:100`):
   ```cpp
   arith_uint256 GetAntiDoSWorkThreshold(const chain::CBlockIndex *tip,
                                         const chain::ChainParams &params,
                                         bool is_ibd) {
     // During IBD, disable anti-DoS checks to allow syncing from genesis
     if (is_ibd) {
       return 0;
     }

     // Calculate work of one block at current difficulty
     arith_uint256 block_proof = GetBlockProof(*tip);

     // Calculate work buffer (144 blocks worth)
     arith_uint256 buffer = block_proof * ANTI_DOS_WORK_BUFFER_BLOCKS;

     // Subtract buffer from tip work (but don't go negative)
     near_tip_work = tip->nChainWork - std::min(buffer, tip->nChainWork);

     return near_tip_work;
   }
   ```

2. **Penalty constant defined** (`include/network/peer_manager.hpp:22`):
   ```cpp
   static constexpr int LOW_WORK_HEADERS = 10;  // 10 violations = disconnect
   ```

3. **Unit test exists** (`test/dos_protection_tests.cpp:148-163`):
   ```cpp
   SECTION("Low-work headers spam (10x = disconnect)") {
       pm.AddPeer(1, "192.168.1.1");

       // Send 10 low-work headers (10 * 10 = 100)
       for (int i = 0; i < 9; i++) {
           pm.Misbehaving(1, MisbehaviorPenalty::LOW_WORK_HEADERS, "low-work-headers");
       }

       REQUIRE(pm.GetMisbehaviorScore(1) == 90);
       REQUIRE_FALSE(pm.ShouldDisconnect(1));

       bool should_disconnect = pm.Misbehaving(1, MisbehaviorPenalty::LOW_WORK_HEADERS,
                                              "low-work-headers");
       REQUIRE(should_disconnect);
       REQUIRE(pm.GetMisbehaviorScore(1) == 100);
   }
   ```

4. **Documented in functional test** (`test/functional/p2p_dos_headers.py:113-116`):
   ```python
   print("  4. Low-work headers (after IBD):")
   print("     - GetAntiDoSWorkThreshold() checks minimum work")
   print("     - Low work → Misbehaving(peer_id, 10, 'low-work-headers')")
   print("     - After 10 violations: score = 100 → disconnect")
   ```

### What's MISSING ❌

**The actual enforcement in `src/network/network_manager.cpp`:**

Looking at `handle_headers_message()` (lines 805-921), the DoS checks are:

```cpp
// Line 854: ✅ Oversized message check
if (headers.size() > protocol::MAX_HEADERS_SIZE) {
    LOG_ERROR("Rejecting oversized headers message...");
    peer_manager_->ReportOversizedMessage(peer_id);
    // ... disconnect logic
}

// Line 872: ✅ Unconnecting headers check
if (!prev_exists) {
    LOG_WARN("Headers don't connect to known chain...");
    peer_manager_->IncrementUnconnectingHeaders(peer_id);
    // ... disconnect logic
}

// Line 894: ✅ Invalid PoW check
bool pow_ok = chainstate_manager_.CheckHeadersPoW(headers);
if (!pow_ok) {
    LOG_ERROR("Headers failed PoW commitment check...");
    peer_manager_->ReportInvalidPoW(peer_id);
    // ... disconnect logic
}

// Line 909: ✅ Non-continuous headers check
bool continuous_ok = validation::CheckHeadersAreContinuous(headers);
if (!continuous_ok) {
    LOG_ERROR("Non-continuous headers...");
    peer_manager_->ReportNonContinuousHeaders(peer_id);
    // ... disconnect logic
}

// ❌❌❌ MISSING: Low-work header check
// Should be here but ISN'T!
// Expected code:
//
// arith_uint256 work_threshold = validation::GetAntiDoSWorkThreshold(
//     chainstate_manager_.GetTip(), params_, is_ibd);
//
// if (work_threshold > 0) {  // Skip during IBD
//     // Check if headers have sufficient total work
//     const chain::CBlockIndex* last_header_index =
//         chainstate_manager_.LookupBlockIndex(headers.back().GetHash());
//
//     if (last_header_index &&
//         last_header_index->nChainWork < work_threshold) {
//         LOG_WARN("Low-work headers from peer {} (work={}, threshold={})",
//                  peer_id,
//                  last_header_index->nChainWork.ToString(),
//                  work_threshold.ToString());
//         peer_manager_->ReportLowWorkHeaders(peer_id);
//         if (peer_manager_->ShouldDisconnect(peer_id)) {
//             if (ban_man_) {
//                 ban_man_->Discourage(peer->address());
//             }
//             peer_manager_->remove_peer(peer_id);
//         }
//         sync_peer_id_.store(0, std::memory_order_release);
//         return false;
//     }
// }
```

**Also missing**: `PeerManager::ReportLowWorkHeaders()` method to apply the penalty.

## The Vulnerability

### Attack Scenario:

1. **Attacker's chain state**:
   ```
   Genesis--[10 blocks at minimum difficulty]
   Total work: ~10 × difficulty
   ```

2. **Victim's chain state**:
   ```
   Genesis--[1000 blocks at current difficulty]
   Total work: ~1000 × difficulty
   ```

3. **Attack**:
   - Attacker sends their 10-block fork headers repeatedly
   - Headers have valid PoW (each block meets difficulty requirement)
   - Headers are continuous (properly linked)
   - Headers connect to genesis (known chain)
   - **But total work is much less than victim's chain**

4. **Impact**:
   - Victim wastes CPU processing low-work headers
   - Victim wastes memory storing block indices
   - Attacker never gets penalized (no misbehavior score increase)
   - Attack can continue indefinitely

### Why This Matters for Headers-Only Chain:

Unlike Bitcoin Core (which processes full blocks), this is a **headers-only chain**:
- Headers are the ONLY thing we process
- Header validation is our main CPU workload
- Making us process spam headers IS the DoS attack
- No full block validation to fall back on

**This makes header spam MORE impactful, not less.**

## Proof of Concept

### Test That Would Fail (if we wrote it):

```cpp
TEST_CASE("Low-work header spam rejected", "[network][dos]") {
    SimulatedNetwork network(12345);
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(100, &network);

    // Victim has high-work chain
    for (int i = 0; i < 100; i++) {
        victim.MineBlock();
    }

    // Attacker has low-work fork from genesis
    for (int i = 0; i < 10; i++) {
        attacker.MineBlockPrivate("attacker_address");
    }

    attacker.ConnectTo(1);
    network.ProcessAllMessages();

    // Send low-work headers repeatedly
    for (int i = 0; i < 15; i++) {
        attacker.SendHeaders(1, attacker.GetPrivateChain());
        network.ProcessAllMessages();
    }

    // EXPECTED: Attacker penalized and disconnected after 10 violations
    // ACTUAL: Attacker not penalized at all ❌
    REQUIRE(attacker.IsDisconnected());  // Would FAIL
}
```

## Code References

### Where the check SHOULD be added:

**File**: `src/network/network_manager.cpp`
**Function**: `NetworkManager::handle_headers_message()`
**Line**: After line 921 (after non-continuous check, before accepting headers)

### Dependencies needed:

1. **Add method to PeerManager** (`include/network/peer_manager.hpp`):
   ```cpp
   void ReportLowWorkHeaders(int peer_id);
   ```

2. **Implement in PeerManager** (`src/network/peer_manager.cpp`):
   ```cpp
   void PeerManager::ReportLowWorkHeaders(int peer_id) {
       Misbehaving(peer_id, MisbehaviorPenalty::LOW_WORK_HEADERS,
                   "low-work-headers");
   }
   ```

3. **Add the check in NetworkManager** (see code above)

4. **Skip during skip_dos_checks** (line 817-826):
   ```cpp
   if (skip_dos_checks) {
       // Skip all DoS checks including low-work check
   }
   ```

## Impact Assessment

### Severity: MEDIUM

**Why not HIGH?**
- Doesn't allow chain corruption (headers still validated for PoW)
- Doesn't allow stealing funds (no transaction layer)
- Doesn't crash the node
- Headers-only means storage impact is limited (~80 bytes per header)

**Why not LOW?**
- Headers are our primary workload (this IS the attack surface)
- Can waste significant CPU on repeated spam
- Can fill up block index with useless entries
- No rate limiting without this check

### Attack Cost:

**Low** - Attacker only needs:
- Valid PoW headers (can mine at minimum difficulty)
- A few KB of bandwidth per spam wave
- No stake, no proof of work at high difficulty

### Defense Cost:

**Low** - Fix requires:
- ~50 lines of code
- Already have the threshold calculation function
- Already have the penalty constant
- Already have test coverage for penalty

## Recommendations

### Immediate Actions:

1. **Implement the missing check** (see code above)
2. **Add `ReportLowWorkHeaders()` to PeerManager**
3. **Write integration test** proving low-work spam is rejected
4. **Verify existing unit test still passes** (`test/dos_protection_tests.cpp:148`)

### Future Enhancements:

1. **Rate limit GETHEADERS requests** - Prevent spam of header requests
2. **Track chainwork per peer** - Disconnect peers that consistently send low-work
3. **Prioritize high-work peers** - Prefer syncing from peers with more work
4. **Add telemetry** - Log when low-work headers are rejected

## Related Issues

- Function exists but never called: Classic "dead code" scenario
- Tests pass because they test PeerManager in isolation, not integration
- Functional test documents behavior but doesn't actually test it (Python can't inject P2P messages)
- Code review missed this because infrastructure exists (looks complete)

## Comparison to Bitcoin Core

**Bitcoin Core** (`src/net_processing.cpp`):
```cpp
// Bitcoin Core checks this in ProcessHeadersMessage()
if (pindex->nChainWork < nMinimumChainWork) {
    // Headers from a low-work chain are ignored
    LogPrint(BCLog::NET, "Ignoring low-work chain from peer=%d\n", pfrom.GetId());
    return;
}
```

**Our code**: Has the infrastructure but missing the actual check.

## Timeline

- **Function added**: Unknown (present in codebase)
- **Gap discovered**: 2025-10-19 during attack test code review
- **Fix status**: NOT IMPLEMENTED
- **Test status**: Unit test exists (passes), integration test MISSING

## Notes

- This was discovered while reviewing whether to include attack test code in the repository
- The irony: We have tests for the penalty system but not for triggering it
- Classic example of "infrastructure without integration"
- Good news: Easy fix, all pieces exist

---

## Action Items

- [ ] Implement low-work header check in `network_manager.cpp`
- [ ] Add `ReportLowWorkHeaders()` method to `PeerManager`
- [ ] Write integration test proving spam is rejected
- [ ] Run existing test suite to verify no regressions
- [ ] Update `ATTACK_CODE_POLICY_DECISION.md` with this finding
- [ ] Consider: Should this be high priority given headers-only architecture?
