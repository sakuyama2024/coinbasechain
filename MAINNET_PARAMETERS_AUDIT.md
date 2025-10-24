# Complete Parameter Audit for 1-Hour Block Time

This document lists **ALL** configurable parameters in the codebase that may need review for the transition from 2-minute to 1-hour blocks.

## 1. CONSENSUS PARAMETERS (CRITICAL)

### Block Timing
| Parameter | File | Line | Current | Proposed | Status |
|-----------|------|------|---------|----------|--------|
| `nPowTargetSpacing` (MainNet) | `src/chain/chainparams.cpp` | 75 | `2 * 60` (120s) | `60 * 60` (3600s) | **MUST CHANGE** |
| `nPowTargetSpacing` (default) | `include/chain/chainparams.hpp` | 37 | `120` | `3600` | **MUST CHANGE** |
| `nPowTargetSpacing` (TestNet) | `src/chain/chainparams.cpp` | 138 | `2 * 60` | `60 * 60` | **MUST CHANGE** |
| `nPowTargetSpacing` (RegTest) | `src/chain/chainparams.cpp` | 192 | `2 * 60` | `60 * 60` | **MUST CHANGE** |

### Difficulty Adjustment
| Parameter | File | Line | Current | Proposed | Status |
|-----------|------|------|---------|----------|--------|
| `nASERTHalfLife` (MainNet) | `src/chain/chainparams.cpp` | 77 | `2 * 24 * 60 * 60` (2 days) | **KEEP** | ✓ Correct (48 blocks now) |
| `nASERTHalfLife` (TestNet) | `src/chain/chainparams.cpp` | 140 | `60 * 60` (1 hour) | **KEEP or 2-4 hours?** | Review |
| `nASERTAnchorHeight` | `src/chain/chainparams.cpp` | 83, 143 | `1` | **KEEP** | ✓ Correct |

### RandomX
| Parameter | File | Line | Current | Proposed | Status |
|-----------|------|------|---------|----------|--------|
| `nRandomXEpochDuration` (MainNet) | `src/chain/chainparams.cpp` | 76 | `7 * 24 * 60 * 60` (1 week) | **KEEP** | ✓ Correct |
| `nRandomXEpochDuration` (TestNet) | `src/chain/chainparams.cpp` | 139 | `7 * 24 * 60 * 60` (1 week) | **KEEP** | ✓ Correct |
| `nRandomXEpochDuration` (RegTest) | `src/chain/chainparams.cpp` | 193-194 | `365 * 24 * 60 * 60 * 100` | **KEEP** | ✓ Correct |

### Network Expiration (Timebomb)
| Parameter | File | Line | Current | Proposed | Status |
|-----------|------|------|---------|----------|--------|
| `nNetworkExpirationInterval` (MainNet) | `src/chain/chainparams.cpp` | 113 | `0` (disabled) | **KEEP** | ✓ Correct |
| `nNetworkExpirationInterval` (TestNet) | `src/chain/chainparams.cpp` | 152 | `2160` blocks (~90 days) | **KEEP** | ✓ Already for 1hr blocks |
| `nNetworkExpirationGracePeriod` (TestNet) | `src/chain/chainparams.cpp` | 153 | `24` blocks (1 day) | **KEEP** | ✓ Already for 1hr blocks |

### Minimum Chain Work
| Parameter | File | Line | Current | Proposed | Status |
|-----------|------|------|---------|----------|--------|
| `nMinimumChainWork` (MainNet) | `src/chain/chainparams.cpp` | 90 | `0x0...0` | **UPDATE** after launch | Set to ~90% of chain work |

---

## 2. VALIDATION & IBD PARAMETERS (CRITICAL)

### Timestamp Validation
| Parameter | File | Line | Current | Proposed | Status |
|-----------|------|------|---------|----------|--------|
| `MAX_FUTURE_BLOCK_TIME` | `include/chain/validation.hpp` | 121 | `2 * 60 * 60` (2 hours) | **KEEP** | ✓ Absolute time |
| `MEDIAN_TIME_SPAN` | `include/chain/block_index.hpp` | 19 | `11` blocks | **KEEP** | ✓ Block count |

### IBD Detection
| Parameter | File | Line | Current | Proposed | Status |
|-----------|------|------|---------|----------|--------|
| IBD tip age check | `src/chain/chainstate_manager.cpp` | 569 | `3600` (1 hour) | `3 * 3600` (3 hours) | **MUST CHANGE** |
| Comment | `src/chain/chainstate_manager.cpp` | 567 | "1 hour for 2-minute blocks" | "3 hours for 1-hour blocks" | **UPDATE COMMENT** |

### DoS Protection
| Parameter | File | Line | Current | Proposed | Status |
|-----------|------|------|---------|----------|--------|
| `ANTI_DOS_WORK_BUFFER_BLOCKS` | `include/chain/validation.hpp` | 124 | `144` blocks | **KEEP** | Now 6 days vs 4.8 hours |
| Comment | `include/chain/validation.hpp` | 125 | "~4.8 hours at 2 min/block" | "~6 days at 1 hour/block" | **UPDATE COMMENT** |

### Reorg Detection
| Parameter | File | Line | Current | Proposed | Status |
|-----------|------|------|---------|----------|--------|
| `SUSPICIOUS_REORG_DEPTH` | `src/chain/chainstate_manager.cpp` | 21 | `100` blocks | **REVIEW** | Now ~4 days vs 3.3 hours |

---

## 3. NETWORK PROTOCOL PARAMETERS

### Timeouts (Absolute Time - Already Correct)
| Parameter | File | Line | Current | Status |
|-----------|------|------|---------|--------|
| `VERSION_HANDSHAKE_TIMEOUT_SEC` | `include/network/protocol.hpp` | 117 | `60` sec | ✓ Absolute time |
| `PING_INTERVAL_SEC` | `include/network/protocol.hpp` | 118 | `120` sec | ✓ Absolute time |
| `PING_TIMEOUT_SEC` | `include/network/protocol.hpp` | 119-120 | `20 * 60` (20 min) | ✓ Absolute time |
| `INACTIVITY_TIMEOUT_SEC` | `include/network/protocol.hpp` | 121-122 | `20 * 60` (20 min) | ✓ Absolute time |

### Orphan Management
| Parameter | File | Line | Current | Proposed | Status |
|-----------|------|------|---------|----------|--------|
| `ORPHAN_HEADER_EXPIRE_TIME` | `include/network/protocol.hpp` | 110 | `600` sec (10 min) | `3600` (1 hour) | **SHOULD CHANGE** |
| `MAX_ORPHAN_HEADERS` | `include/network/protocol.hpp` | 108 | `1000` | **REVIEW** | May need adjustment |
| `MAX_ORPHAN_HEADERS_PER_PEER` | `include/network/protocol.hpp` | 109 | `50` | **REVIEW** | May need adjustment |

### Connection Limits
| Parameter | File | Line | Current | Status |
|-----------|------|------|---------|--------|
| `DEFAULT_MAX_OUTBOUND_CONNECTIONS` | `include/network/protocol.hpp` | 113 | `8` | ✓ Not time-dependent |
| `DEFAULT_MAX_INBOUND_CONNECTIONS` | `include/network/protocol.hpp` | 114 | `125` | ✓ Not time-dependent |

### Protocol Message Limits
| Parameter | File | Line | Current | Status |
|-----------|------|------|---------|--------|
| `MAX_LOCATOR_SZ` | `include/network/protocol.hpp` | 101-102 | `101` | ✓ Not time-dependent |
| `MAX_INV_SIZE` | `include/network/protocol.hpp` | 103 | `50000` | ✓ Not time-dependent |
| `MAX_HEADERS_SIZE` | `include/network/protocol.hpp` | 104 | `2000` | ✓ Not time-dependent |
| `MAX_ADDR_SIZE` | `include/network/protocol.hpp` | 105 | `1000` | ✓ Not time-dependent |

### Feeler Connections
| Parameter | File | Line | Current | Status |
|-----------|------|------|---------|--------|
| `FEELER_INTERVAL` | `include/network/network_manager.hpp` | 157 | `2` minutes | ✓ Absolute time |

### SendMessages Loop
| Parameter | File | Line | Current | Status |
|-----------|------|------|---------|--------|
| `SENDMESSAGES_INTERVAL` | `include/network/network_manager.hpp` | 158 | `1` second | ✓ Absolute time |

---

## 4. ADDRESS MANAGER PARAMETERS

| Parameter | File | Line | Current | Status |
|-----------|------|------|---------|--------|
| `STALE_AFTER_DAYS` | `src/network/addr_manager.cpp` | 13 | `30` days | ✓ Absolute time |
| `MAX_FAILURES` | `src/network/addr_manager.cpp` | 14 | `10` | ✓ Not time-dependent |
| `SECONDS_PER_DAY` | `src/network/addr_manager.cpp` | 15 | `86400` | ✓ Absolute time |

---

## 5. BAN MANAGER PARAMETERS

| Parameter | File | Line | Current | Status |
|-----------|------|------|---------|--------|
| `DISCOURAGEMENT_DURATION` | `include/network/banman.hpp` | 77 | `24 * 60 * 60` (24 hours) | ✓ Absolute time |

---

## 6. PEER MANAGER PARAMETERS

### Misbehavior Penalties
| Parameter | File | Line | Current | Status |
|-----------|------|------|---------|--------|
| `DISCOURAGEMENT_THRESHOLD` | `include/network/peer_manager.hpp` | 15 | `100` points | ✓ Not time-dependent |
| `INVALID_POW` | `include/network/peer_manager.hpp` | 19 | `100` | ✓ Not time-dependent |
| `OVERSIZED_MESSAGE` | `include/network/peer_manager.hpp` | 20 | `20` | ✓ Not time-dependent |
| `NON_CONTINUOUS_HEADERS` | `include/network/peer_manager.hpp` | 21 | `20` | ✓ Not time-dependent |
| `LOW_WORK_HEADERS` | `include/network/peer_manager.hpp` | 22 | `10` | ✓ Not time-dependent |
| `INVALID_HEADER` | `include/network/peer_manager.hpp` | 23 | `100` | ✓ Not time-dependent |
| `TOO_MANY_UNCONNECTING` | `include/network/peer_manager.hpp` | 24 | `100` | ✓ Not time-dependent |
| `TOO_MANY_ORPHANS` | `include/network/peer_manager.hpp` | 25 | `100` | ✓ Not time-dependent |
| `MAX_UNCONNECTING_HEADERS` | `include/network/peer_manager.hpp` | 29 | `10` messages | ✓ Not time-dependent |

---

## 7. TIME DATA / NTP-LIKE PARAMETERS

| Parameter | File | Line | Current | Status |
|-----------|------|------|---------|--------|
| `DEFAULT_MAX_TIME_ADJUSTMENT` | `include/chain/timedata.hpp` | 19 | `70 * 60` (±70 min) | ✓ Absolute time |
| `MAX_TIME_SAMPLES` | `src/chain/timedata.cpp` | 15 | `200` peers | ✓ Not time-dependent |

---

## 8. MINING / RPC PARAMETERS

| Parameter | File | Line | Current | Proposed | Status |
|-----------|------|------|---------|----------|--------|
| `DEFAULT_HASHRATE_CALCULATION_BLOCKS` | `include/network/protocol.hpp` | 125 | `120` blocks | `120` or `72`? | **REVIEW** |
| Comment | docs/PROTOCOL_SPECIFICATION.md | 703 | "~4 hours" | "~5 days" or reduce blocks | **UPDATE COMMENT** |

---

## 9. NAT/UPnP PARAMETERS

| Parameter | File | Line | Current | Status |
|-----------|------|------|---------|--------|
| `UPNP_DISCOVER_TIMEOUT_MS` | `src/network/nat_manager.cpp` | 18 | `2000` ms | ✓ Absolute time |
| `PORT_MAPPING_DURATION_SECONDS` | `src/network/nat_manager.cpp` | 19 | `3600` (1 hour) | ✓ Absolute time |
| `REFRESH_INTERVAL_SECONDS` | `src/network/nat_manager.cpp` | 20 | `1800` (30 min) | ✓ Absolute time |

---

## SUMMARY BY PRIORITY

### 🔴 CRITICAL - MUST CHANGE
1. ✅ `nPowTargetSpacing` (4 locations) - Core consensus
2. ✅ IBD tip age check (chainstate_manager.cpp:569) - Sync detection

### 🟡 IMPORTANT - SHOULD CHANGE
3. ✅ `ORPHAN_HEADER_EXPIRE_TIME` - From 10 min to 1 hour
4. ✅ TestNet `nASERTHalfLife` - Consider 2-4 hours for faster testing

### 🟢 REVIEW & DECIDE
5. ✅ `SUSPICIOUS_REORG_DEPTH` (100 blocks) - Now ~4 days
6. ✅ `DEFAULT_HASHRATE_CALCULATION_BLOCKS` (120) - Now ~5 days, consider reducing
7. ✅ `MAX_ORPHAN_HEADERS` / `MAX_ORPHAN_HEADERS_PER_PEER` - May need tuning

### 📝 COMMENTS ONLY
8. ✅ Update all comments referencing "2-minute blocks" or time calculations
9. ✅ Update documentation (WARP.md, PROTOCOL_SPECIFICATION.md, etc.)

### ✓ ALREADY CORRECT
- All absolute time constants (timeouts, durations, etc.)
- Block count constants (MEDIAN_TIME_SPAN, etc.)
- Network limits and DoS thresholds
- Connection and message size limits
- Network expiration already configured for 1-hour blocks

---

## FILES REQUIRING CHANGES

### Code Changes (6 files):
1. `src/chain/chainparams.cpp` - nPowTargetSpacing (4 places)
2. `include/chain/chainparams.hpp` - default nPowTargetSpacing
3. `src/chain/chainstate_manager.cpp` - IBD tip age check
4. `include/network/protocol.hpp` - ORPHAN_HEADER_EXPIRE_TIME
5. `include/chain/validation.hpp` - ANTI_DOS_WORK_BUFFER comment
6. `include/network/protocol.hpp` - DEFAULT_HASHRATE_CALCULATION comment

### Documentation Updates:
- `WARP.md`
- `docs/PROTOCOL_SPECIFICATION.md`
- `docs/ARCHITECTURE.md`
- `README.md`
- Any test files with hardcoded time expectations
