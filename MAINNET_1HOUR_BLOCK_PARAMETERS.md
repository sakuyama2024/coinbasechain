# Mainnet 1-Hour Block Time: Parameter Review & Recommendations

**Current:** 2-minute block time
**Proposed:** 1-hour (60-minute) block time
**Change Factor:** 30x increase

---

## Executive Summary

When increasing block time from 2 minutes to 1 hour (30x), we need to adjust:
- ✅ **Block-count-based parameters** (adjust proportionally to maintain similar real-world timing)
- ✅ **Some hybrid parameters** (careful analysis needed)
- ❌ **Time-based parameters** (keep unchanged - they measure real-world time)

**Critical Changes Required:** 4 parameters
**Important Security Policy:** 1 parameter (requires decision)
**Recommended Changes:** 6 parameters
**Keep Unchanged:** 35+ parameters

---

## CRITICAL CHANGES (Must Change)

### 1. Block Target Spacing ⚠️ CRITICAL
```cpp
// File: include/chain/chainparams.hpp
consensus.nPowTargetSpacing = 2 * 60;  // Currently: 2 minutes
```

**Recommendation:** **CHANGE to 3600 (1 hour)**
```cpp
consensus.nPowTargetSpacing = 60 * 60;  // 1 hour = 3600 seconds
```
**Reason:** This IS the block time. Must change to 1 hour.

---

### 2. Anti-DoS Work Buffer ⚠️ CRITICAL
```cpp
// File: include/chain/validation.hpp
static constexpr int ANTI_DOS_WORK_BUFFER_BLOCKS = 144;
```

**Current Impact:**
- At 2min: 144 blocks = 4.8 hours ✅ Reasonable
- At 1hr: 144 blocks = 144 hours (6 days) ❌ Too long!

**Recommendation:** **CHANGE to 6 blocks**
```cpp
static constexpr int ANTI_DOS_WORK_BUFFER_BLOCKS = 6;  // 6 hours at 1-hour blocks
```
**Reason:** Prevents accepting headers too far ahead of our best chain. 6 days is excessive - peers shouldn't have 144 more headers than us except during initial sync.

**Alternative:** 12 blocks (12 hours) if you want more leniency

---

### 3. Hashrate Calculation Window ⚠️ CRITICAL
```cpp
// File: include/network/protocol.hpp
constexpr int DEFAULT_HASHRATE_CALCULATION_BLOCKS = 120;
```

**Current Impact:**
- At 2min: 120 blocks = 4 hours ✅ Good for statistics
- At 1hr: 120 blocks = 5 days ❌ Way too long for meaningful hashrate

**Recommendation:** **CHANGE to 6 blocks**
```cpp
constexpr int DEFAULT_HASHRATE_CALCULATION_BLOCKS = 6;  // 6 hours at 1-hour blocks
```
**Reason:** Hashrate should be calculated over a reasonable recent window (hours, not days).

**Alternative:** 12 blocks (12 hours) or 24 blocks (1 day)

---

### 4. Orphan Header Expiry Time ⚠️ IMPORTANT
```cpp
// File: include/network/protocol.hpp
constexpr int64_t ORPHAN_HEADER_EXPIRE_TIME = 600;  // 10 minutes
```

**Current Impact:**
- At 2min blocks: 10 minutes = 5 blocks worth ✅
- At 1hr blocks: 10 minutes = 1/6 of a block ❌ Too short

**Recommendation:** **CHANGE to 7200 (2 hours)**
```cpp
constexpr int64_t ORPHAN_HEADER_EXPIRE_TIME = 7200;  // 2 hours (2 blocks worth)
```
**Reason:** Orphan headers might legitimately wait longer with 1-hour blocks. 2-3 hours = 2-3 blocks worth is reasonable.

---

### 5. Suspicious Reorg Depth ⚠️ CRITICAL (Security Policy Decision)
```cpp
// File: include/application.hpp
int suspicious_reorg_depth = 100;  // Default: 100 blocks
```

**Current Impact:**
- At 2min: 100 blocks = 200 minutes (3.3 hours) ✅ Reasonable finality
- At 1hr: 100 blocks = 100 hours (4.2 days) ❌ Very long finality window

**Critical Insight: Reorg Probability with 1-Hour Blocks**

With 1-hour blocks, natural reorgs become **extremely rare**:

| Metric | 2-Minute Blocks | 1-Hour Blocks | Ratio |
|--------|----------------|---------------|-------|
| Block interval | 120 seconds | 3600 seconds | 30x |
| Network propagation | ~5-10 seconds | ~5-10 seconds | Same |
| Propagation ratio | 5/120 = 4% | 5/3600 = 0.14% | **30x safer** |

**What This Means:**
- **2-min blocks:** Next block can arrive before previous block fully propagates → natural 1-2 block reorgs common
- **1-hour blocks:** By the time next block is mined, previous block has propagated globally → natural reorgs **extremely rare**

**Any significant reorg with 1-hour blocks indicates:**
1. 🔴 **51% attack** (deliberate chain rewrite)
2. 🔴 **Network partition** (hours-long split)
3. 🔴 **Catastrophic failure** (major infrastructure issue)

**Conclusion:** Even a **3-6 block reorg** (3-6 hours) is highly suspicious with 1-hour blocks.

---

**Revised Recommendations:**

**Option A: Aggressive (RECOMMENDED)**
```cpp
int suspicious_reorg_depth = 6;  // 6 hours at 1-hour blocks
```
- ✅ Any deeper reorg is almost certainly an attack
- ✅ Natural reorgs should be 1-2 blocks maximum
- ✅ Fast finality: 6 hours (vs current 3.3 hours - acceptable)
- ⚠️ Requires high network reliability

**Option B: Balanced**
```cpp
int suspicious_reorg_depth = 12;  // 12 hours at 1-hour blocks
```
- ✅ Generous tolerance for extended network issues
- ✅ Same-day finality (practical for exchanges)
- ✅ Still much stricter than Bitcoin's approach (6 blocks = 1 hour for Bitcoin)

**Option C: Conservative**
```cpp
int suspicious_reorg_depth = 24;  // 24 hours (1 day)
```
- ✅ Very generous - handles day-long network partitions
- ✅ Good for initial mainnet deployment (extra safety margin)
- ⚠️ Longer finality than most users expect

**Option D: Keep 100 blocks (NOT RECOMMENDED)**
```cpp
int suspicious_reorg_depth = 100;  // 4.2 days - IMPRACTICAL
```
- ❌ 4+ days before finality is unusable for most applications
- ❌ Exchanges won't list coins with 4-day finality
- ❌ Doesn't match reorg probability (any 100-block reorg is 100% attack)

---

**Recommendation:** **Option A (6 blocks)** or **Option B (12 blocks)**

```cpp
// Recommended (6 hours - natural reorgs extremely unlikely)
int suspicious_reorg_depth = 6;

// OR Conservative (12 hours - extra tolerance)
int suspicious_reorg_depth = 12;
```

**Reasoning:**
1. With 1-hour blocks, reorgs beyond 2-3 blocks are vanishingly rare naturally
2. Any 6+ block reorg is almost certainly malicious or catastrophic
3. 6-12 hour finality is practical for real-world use (exchanges, services)
4. Bitcoin uses 6 blocks (1 hour) - we'd use 6 blocks (6 hours) = 6x more conservative
5. The old values (48-100 blocks) don't match the reorg probability model

**⚠️ CRITICAL:** This is a **consensus-critical security policy**. Requires community discussion and decision before mainnet deployment.

**Testing Strategy:**
1. Start with **12 blocks on testnet** (conservative)
2. Monitor actual reorg behavior for 2+ weeks
3. If no natural reorgs beyond 2 blocks, consider lowering to **6 blocks**
4. Deploy chosen value to mainnet

---

## RECOMMENDED CHANGES (Should Change)

### 6. ASERT Half-Life 📊 RECOMMENDED
```cpp
// File: include/chain/chainparams.hpp
consensus.nASERTHalfLife = 2 * 24 * 60 * 60;  // 2 days (172800 seconds)
```

**Current Impact:**
- At 2min: 2 days = 1440 blocks
- At 1hr: 2 days = 48 blocks

**Analysis:**
- Bitcoin uses ~3.5 days half-life with 10-min blocks (~504 blocks)
- We currently use 2 days with 2-min blocks (1440 blocks)
- With 1hr blocks: 2 days = only 48 blocks

**Recommendation:** **Consider increasing to 3-7 days**
```cpp
consensus.nASERTHalfLife = 5 * 24 * 60 * 60;  // 5 days = 120 blocks at 1-hour
```

**Options:**
- **Conservative (3 days = 72 blocks):** Faster difficulty adjustment
- **Moderate (5 days = 120 blocks):** Balanced, similar to current block-count
- **Bitcoin-like (7 days = 168 blocks):** More stable, like Bitcoin's approach

**Reason:** ASERT half-life controls how quickly difficulty adjusts. With fewer blocks per day, we might want a longer half-life in real-time to maintain similar adjustment responsiveness.

**⚠️ IMPORTANT:** This needs simulation/analysis with your ASERT implementation. Test on testnet first!

---

### 7. Median Time Span 📊 RECOMMENDED
```cpp
// File: include/chain/block_index.hpp
static constexpr int MEDIAN_TIME_SPAN = 11;
```

**Current Impact:**
- At 2min: 11 blocks = 22 minutes ✅
- At 1hr: 11 blocks = 11 hours ❌ Very long

**Recommendation:** **CHANGE to 6 blocks**
```cpp
static constexpr int MEDIAN_TIME_SPAN = 6;  // 6 hours at 1-hour blocks
```

**Reason:** Median Time Past prevents timestamp manipulation. Bitcoin uses 11 blocks (110 minutes). With 1-hour blocks, 11 blocks = 11 hours which is excessive. 6 blocks (6 hours) provides good protection while being more responsive.

**Alternative:** 11 blocks if you want maximum timestamp manipulation protection (11 hours of history)

---

### 8. Inactivity Timeout 🔄 OPTIONAL
```cpp
// File: include/network/protocol.hpp
constexpr int INACTIVITY_TIMEOUT_SEC = 20 * 60;  // 20 minutes
```

**Recommendation:** **Consider increasing to 60 minutes**
```cpp
constexpr int INACTIVITY_TIMEOUT_SEC = 60 * 60;  // 60 minutes (1 block time)
```

**Reason:** With 1-hour blocks, blockchain activity is 30x slower. A peer might legitimately be idle for longer periods. Increasing to 1 hour (= 1 block time) matches the new rhythm.

**Alternative:** Keep at 20 minutes if you want tighter connection management

---

### 9. Ping Interval 🔄 OPTIONAL
```cpp
// File: include/network/protocol.hpp
constexpr int PING_INTERVAL_SEC = 120;  // 2 minutes
```

**Recommendation:** **Consider increasing to 5 minutes**
```cpp
constexpr int PING_INTERVAL_SEC = 5 * 60;  // 5 minutes
```

**Reason:** With much slower blockchain activity, less frequent pings are acceptable. Reduces network chatter.

**Alternative:** Keep at 2 minutes for tighter peer monitoring

---

### 10. Feeler Connection Interval 🔄 OPTIONAL
```cpp
// File: include/network/network_manager.hpp
static constexpr std::chrono::minutes FEELER_INTERVAL{2};
```

**Recommendation:** **Consider increasing to 5 minutes**
```cpp
static constexpr std::chrono::minutes FEELER_INTERVAL{5};
```

**Reason:** Network topology discovery doesn't need to be as frequent with slower blocks.

**Alternative:** Keep at 2 minutes for faster network discovery

---

## KEEP UNCHANGED (No Changes Needed)

### Time-Based Security Parameters ✅
```cpp
// MAX_FUTURE_BLOCK_TIME - Clock drift protection
MAX_FUTURE_BLOCK_TIME = 2 * 60 * 60;  // KEEP: 2 hours

// DEFAULT_MAX_TIME_ADJUSTMENT - Network time sync
DEFAULT_MAX_TIME_ADJUSTMENT = 70 * 60;  // KEEP: ±70 minutes

// DISCOURAGEMENT_DURATION - Ban duration
DISCOURAGEMENT_DURATION = 24 * 60 * 60;  // KEEP: 24 hours
```
**Reason:** These are based on real-world time (clock drift, ban duration), not blockchain state.

---

### Network Timeouts ✅
```cpp
VERSION_HANDSHAKE_TIMEOUT_SEC = 60;     // KEEP: 1 minute
PING_TIMEOUT_SEC = 20 * 60;             // KEEP: 20 minutes (or increase if you increased PING_INTERVAL)
```
**Reason:** Network latency and connection establishment are independent of block time.

---

### Message Size Limits ✅
```cpp
MAX_PROTOCOL_MESSAGE_LENGTH = 4 * 1000 * 1000;  // KEEP: 4 MB
MAX_HEADERS_SIZE = 2000;                        // KEEP: 2000 headers per message
MAX_INV_SIZE = 50000;                           // KEEP: 50000 inventory items
MAX_ADDR_SIZE = 1000;                           // KEEP: 1000 addresses
```
**Reason:** Bandwidth and memory constraints, not time-based.

---

### Connection Limits ✅
```cpp
DEFAULT_MAX_OUTBOUND_CONNECTIONS = 8;   // KEEP
DEFAULT_MAX_INBOUND_CONNECTIONS = 125;  // KEEP
```
**Reason:** Network topology parameters, not time-based.

---

### DoS Protection (Count-Based) ✅
```cpp
DISCOURAGEMENT_THRESHOLD = 100;         // KEEP
MAX_UNCONNECTING_HEADERS = 10;          // KEEP
MAX_ORPHAN_HEADERS = 1000;              // KEEP (absolute limit)
MAX_ORPHAN_HEADERS_PER_PEER = 50;       // KEEP
```
**Reason:** These are count-based thresholds, not time-based.

---

### Address Manager ✅
```cpp
STALE_AFTER_DAYS = 30;                  // KEEP: 30 days
MAX_FAILURES = 10;                      // KEEP: 10 connection failures
```
**Reason:** Real-world time and count-based.

---

### RandomX Parameters ✅
```cpp
consensus.nRandomXEpochDuration = 7 * 24 * 60 * 60;  // KEEP: 1 week
```
**Reason:** RandomX cache regeneration is based on real-world security considerations (time to generate rainbow tables, hardware availability). Keep at 1 week in real time.

**At 1hr blocks:** 1 week = 168 blocks (vs 5040 blocks at 2min)

---

### NAT/UPnP ✅
```cpp
PORT_MAPPING_DURATION_SECONDS = 3600;   // KEEP or INCREASE to 2 hours
REFRESH_INTERVAL_SECONDS = 1800;        // KEEP: 30 minutes
```
**Reason:** Network configuration, not blockchain-related. Could increase mapping duration slightly if desired.

---

## Implementation Checklist

### Phase 1: Testnet Testing
- [ ] Update testnet with all CRITICAL changes
- [ ] Monitor for 1 week (168 blocks at 1hr):
  - [ ] Difficulty adjustment behavior (ASERT)
  - [ ] Orphan header handling
  - [ ] DoS protection effectiveness
  - [ ] Hashrate statistics accuracy
  - [ ] Network peer behavior

### Phase 2: Parameter Tuning
- [ ] Analyze ASERT half-life performance
- [ ] Test Median Time Span with 6 blocks
- [ ] Verify orphan expiry is adequate
- [ ] Check if increased ping intervals cause issues

### Phase 3: Mainnet Deployment
- [ ] Update chainparams.cpp for mainnet
- [ ] Update all affected constants
- [ ] Document changes in release notes
- [ ] Hard fork activation at specific block height

---

## Code Changes Required

### File: `src/chain/chainparams.cpp`
```cpp
// MainNetParams constructor
consensus.nPowTargetSpacing = 60 * 60;  // 1 hour (was: 2 * 60)
consensus.nASERTHalfLife = 5 * 24 * 60 * 60;  // 5 days (was: 2 * 24 * 60 * 60)
// consensus.nRandomXEpochDuration - KEEP at 1 week
```

### File: `include/chain/validation.hpp`
```cpp
static constexpr int ANTI_DOS_WORK_BUFFER_BLOCKS = 6;  // Was: 144
```

### File: `include/application.hpp`
```cpp
int suspicious_reorg_depth = 6;  // Was: 100 (or use 12 for extra tolerance)
// Note: With 1-hour blocks, natural reorgs beyond 2-3 blocks are extremely rare
```

### File: `include/chain/block_index.hpp`
```cpp
static constexpr int MEDIAN_TIME_SPAN = 6;  // Was: 11
```

### File: `include/network/protocol.hpp`
```cpp
constexpr int DEFAULT_HASHRATE_CALCULATION_BLOCKS = 6;  // Was: 120
constexpr int64_t ORPHAN_HEADER_EXPIRE_TIME = 7200;     // Was: 600 (10 min)

// Optional:
constexpr int PING_INTERVAL_SEC = 5 * 60;               // Was: 120
constexpr int INACTIVITY_TIMEOUT_SEC = 60 * 60;         // Was: 20 * 60
```

### File: `include/network/network_manager.hpp`
```cpp
// Optional:
static constexpr std::chrono::minutes FEELER_INTERVAL{5};  // Was: 2
```

---

## Risk Assessment

### High Risk (Must Get Right)
- **ANTI_DOS_WORK_BUFFER_BLOCKS:** Too high = slow sync, too low = DoS vulnerability
- **ASERT Half-Life:** Wrong value = difficulty oscillation or stuck difficulty
- **suspicious_reorg_depth:** Too low = reject legitimate forks, too high = weak finality guarantees

### Medium Risk (Test Thoroughly)
- **MEDIAN_TIME_SPAN:** Affects timestamp manipulation resistance
- **ORPHAN_HEADER_EXPIRE_TIME:** Too short = legitimate orphans dropped, too long = memory bloat

### Low Risk (Easy to Adjust)
- **PING_INTERVAL_SEC:** Just network efficiency
- **INACTIVITY_TIMEOUT_SEC:** Connection management preference
- **Hashrate calculation blocks:** Only affects statistics display

---

## Testing Strategy

1. **Simulation:** Test ASERT difficulty adjustment with 1-hour blocks
2. **Testnet:** Deploy for at least 2 weeks (336 blocks)
3. **Monitor:**
   - Block time variance
   - Difficulty adjustment responsiveness
   - Network topology (peer count stability)
   - Orphan rate and expiry
4. **Tune:** Adjust ASERT half-life if needed based on testnet data

---

## Summary Table

| Parameter | Current | Recommended | Priority | Reason |
|-----------|---------|-------------|----------|--------|
| `nPowTargetSpacing` | 2 min | 60 min | CRITICAL | Define block time |
| `ANTI_DOS_WORK_BUFFER_BLOCKS` | 144 | 6 | CRITICAL | 144 hrs too long |
| `DEFAULT_HASHRATE_CALCULATION_BLOCKS` | 120 | 6 | CRITICAL | 5 days too long |
| `ORPHAN_HEADER_EXPIRE_TIME` | 10 min | 2 hrs | IMPORTANT | 10 min too short |
| `suspicious_reorg_depth` | 100 | **6-12** | **CRITICAL** | Reorgs unlikely with 1hr blocks |
| `nASERTHalfLife` | 2 days | 5 days | RECOMMENDED | Test on testnet |
| `MEDIAN_TIME_SPAN` | 11 | 6 | RECOMMENDED | 11 hrs too long |
| `INACTIVITY_TIMEOUT_SEC` | 20 min | 60 min | OPTIONAL | Match block rhythm |
| `PING_INTERVAL_SEC` | 2 min | 5 min | OPTIONAL | Less network chatter |
| `FEELER_INTERVAL` | 2 min | 5 min | OPTIONAL | Slower discovery OK |
| Everything else | - | KEEP | - | Time or count based |

---

**Next Steps:**
1. **Review and approve these recommendations**
2. **CRITICAL: Decide on suspicious_reorg_depth policy** (6 or 12 blocks recommended)
3. Test on testnet with modified parameters (start with 12, consider lowering to 6)
4. Monitor for at least 336 blocks (2 weeks at 1-hour blocks)
   - Track actual reorg frequency and depth
   - Verify natural reorgs stay within 1-2 blocks
5. Adjust ASERT half-life based on testnet data
6. Deploy to mainnet via hard fork
