# Mining Architecture Analysis Report
**Date**: 2025-10-22
**Analyzed By**: Claude Code
**Status**: Issues Identified - Fixes Pending

---

## Executive Summary

Analyzed the single-threaded CPU miner implementation in `src/chain/miner.cpp`. Found **4 critical bugs** including race conditions, infinite loop potential, and incorrect height checking logic. The miner is designed for regtest testing only but has threading safety issues that need addressing.

---

## Architecture Overview

### Components

```
┌─────────────┐         ┌──────────────┐         ┌──────────────────┐
│  RPC Thread │────────▶│   CPUMiner   │────────▶│ ChainstateManager│
│             │  Start()│              │Process  │                  │
│  generate   │  Stop() │ MiningWorker │Block    │   Validation     │
└─────────────┘         └──────────────┘         └──────────────────┘
```

### Threading Model

- **RPC Thread**: Receives `generate` command via Unix socket
  - Calls `miner_->Stop()` to ensure clean state
  - Calls `miner_->Start(target_height)`
  - Busy-waits polling `miner_->IsMining()` every 100ms
  - Returns list of mined block hashes

- **Mining Thread**: Single worker thread (`MiningWorker()`)
  - Increments nonces and checks PoW
  - Calls `chainstate_.ProcessNewBlockHeader()` when block found
  - Auto-stops when reaching target height
  - Uses atomics for coordination with RPC thread

### Key Files

- `src/chain/miner.cpp` - Main implementation (234 lines)
- `include/chain/miner.hpp` - Header with class definition
- `src/network/rpc_server.cpp:1020-1116` - RPC `generate` handler

---

## Critical Bugs Identified

### **BUG #1: Race Condition - Template Access**

**Severity**: HIGH ⚠️
**Type**: Data Race (Undefined Behavior)

**Location**:
- `miner.hpp:84-86` (member variables)
- `miner.cpp:51, 115, 121, 171` (access points)

**Problem**:
```cpp
// NON-ATOMIC shared state accessed by multiple threads!
BlockTemplate current_template_;      // Line 85
uint256 template_prev_hash_;         // Line 86
```

**Race Scenario**:
```
RPC Thread (Start)              Mining Thread (MiningWorker)
─────────────────              ────────────────────────────
Line 51:
current_template_ = Create()
                               Line 121:
                               current_template_.header.nNonce = nonce

                               Line 171:
                               current_template_.header.nTime++
```

**Impact**:
- Mining thread reads torn/inconsistent template data
- Undefined behavior per C++ memory model
- Could cause invalid blocks, crashes, or corruption

**Why It Happens**:
- `Start()` called from RPC thread writes to `current_template_`
- `MiningWorker()` running in mining thread reads/writes same memory
- No synchronization between threads

**Fix Required**: Add mutex around template access OR make mining thread create its own copy

---

### **BUG #2: Infinite Loop on Validation Failure**

**Severity**: CRITICAL 🔴
**Type**: Logic Error

**Location**: `miner.cpp:143-160`

**Problem**:
```cpp
if (!chainstate_.ProcessNewBlockHeader(found_header, state)) {
  LOG_ERROR("Miner: Failed to process mined block: {} - {}",
            state.GetRejectReason(), state.GetDebugMessage());
  // BUG: No break/return/continue - execution falls through!
}

// Check if we've reached target height
int target = target_height_.load();
if (target != -1 && current_template_.nHeight >= target) {
  LOG_INFO("Miner: Reached target height {}, stopping", target);
  mining_.store(false);
  break;
}

// Continue mining next block
current_template_ = CreateBlockTemplate();  // CREATES HEIGHT N+1 AGAIN!
template_prev_hash_ = current_template_.hashPrevBlock;
nonce = 0;
continue;
```

**What Happens When Validation Fails**:
1. Block at height N+1 fails validation (e.g., network expiration at height 100)
2. Chain tip remains at height N (doesn't advance)
3. Code falls through and creates new template
4. `CreateBlockTemplate()` reads tip (height N), creates template for N+1
5. In regtest, nonce=0 wins instantly
6. **Loop repeats forever mining same invalid height**

**This was the timebomb bug!** We worked around it by disabling the timebomb in regtest, but the underlying logic is still broken.

**Real-World Logs** (from previous debugging):
```
[2025-10-22 19:55:02.845] [default] [info] Miner: *** BLOCK FOUND *** Height: 101, Nonce: 0
[2025-10-22 19:55:02.845] [default] [error] Contextual check failed: network-expired
[2025-10-22 19:55:02.845] [default] [error] Miner: Failed to process mined block
[2025-10-22 19:55:02.970] [default] [info] Miner: *** BLOCK FOUND *** Height: 101, Nonce: 0
[2025-10-22 19:55:02.970] [default] [error] Miner: Failed to process mined block
... repeats forever ...
```

**Fix Required**: Add `continue` after validation failure to skip template creation

---

### **BUG #3: Incorrect Height Check Logic**

**Severity**: MEDIUM
**Type**: Logic Error

**Location**: `miner.cpp:149-154`

**Problem**:
```cpp
// Check if we've reached target height
int target = target_height_.load();
if (target != -1 && current_template_.nHeight >= target) {
  LOG_INFO("Miner: Reached target height {}, stopping", target);
  mining_.store(false);
  break;
}
```

**Issue**: Checks **template height** instead of **actual chain height**

**Why This Is Wrong**:
- `current_template_.nHeight` is what we're *trying* to mine
- Actual chain height might be different if validation fails
- Example: Template says height 101, but validation failed so tip is still 100

**Scenario**:
```
generate 100  (from height 0)
1. Mine height 1-100 successfully
2. Try to mine height 101
3. Validation fails (e.g., some future bug)
4. Check: current_template_.nHeight (101) >= target (100) ✓ PASSES
5. Miner stops, thinking it reached target
6. BUT actual chain is at height 100! Only mined 100 blocks, not 101
```

**Fix Required**: Check actual chain height from chainstate, not template

---

### **BUG #4: Race Condition - mining_address_ Access**

**Severity**: MEDIUM ⚠️
**Type**: Data Race

**Location**:
- `miner.hpp:75` (member variable)
- `miner.cpp:198` (read in CreateBlockTemplate)
- `rpc_server.cpp:1062` (write via SetMiningAddress)

**Problem**:
```cpp
// NON-ATOMIC shared state
uint160 mining_address_;  // 160 bits = 20 bytes
```

**Race Scenario**:
```
RPC Thread                      Mining Thread
──────────                      ─────────────
Line 1062:
miner_->SetMiningAddress(addr)
                               Line 198:
                               tmpl.header.minerAddress = mining_address_
```

**Impact**:
- Mining thread might read partial/torn address during write
- Could mine block with corrupted or wrong address
- Less severe than Bug #1 because:
  - Writes are infrequent (only when address changes)
  - Wrong address doesn't crash node
  - Block is still valid, just reward goes to wrong address

**Fix Required**: Add mutex OR make atomic (requires atomic<uint160> which doesn't exist, so mutex needed)

---

### **MINOR: Inefficient RPC Polling**

**Severity**: LOW
**Type**: Performance Issue

**Location**: `rpc_server.cpp:1082-1085`

**Problem**:
```cpp
while (miner_->IsMining() && wait_count < 6000) {
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  wait_count++;
}
```

**Issue**: RPC thread busy-waits polling every 100ms for up to 10 minutes

**Impact**:
- Wastes CPU cycles (though 100ms sleep is reasonable)
- Delays RPC response by up to 100ms after mining completes
- Not critical for regtest, but inefficient design

**Better Approach**: Use `std::condition_variable` for mining thread to notify RPC thread when done

---

## Detailed Code Analysis

### Start() Flow

```cpp
bool CPUMiner::Start(int target_height) {
  if (mining_.load()) {
    LOG_WARN("Miner: Already mining");
    return false;
  }

  // Join any previous thread if it finished
  if (worker_.joinable()) {
    worker_.join();
  }

  // ... setup ...

  // RACE: RPC thread writes to current_template_
  current_template_ = CreateBlockTemplate();  // Line 51
  template_prev_hash_ = current_template_.hashPrevBlock;

  // Start mining thread which will READ current_template_
  worker_ = std::thread([this]() { MiningWorker(); });  // Line 60

  return true;
}
```

### MiningWorker() Flow

```cpp
void CPUMiner::MiningWorker() {
  uint32_t nonce = 0;

  while (mining_.load()) {
    // Check if template needs regeneration
    if (ShouldRegenerateTemplate()) {
      LOG_INFO("Miner: Chain tip changed, regenerating template");
      current_template_ = CreateBlockTemplate();  // Line 115
      template_prev_hash_ = current_template_.hashPrevBlock;
      nonce = 0;
    }

    // RACE: Mining thread reads current_template_
    current_template_.header.nNonce = nonce;  // Line 121

    // Try mining
    uint256 rx_hash;
    bool found_block = consensus::CheckProofOfWork(
        current_template_.header, current_template_.nBits, params_,
        crypto::POWVerifyMode::MINING, &rx_hash);

    if (found_block) {
      blocks_found_.fetch_add(1);

      CBlockHeader found_header = current_template_.header;
      found_header.hashRandomX = rx_hash;

      // Process block through validation
      validation::ValidationState state;
      if (!chainstate_.ProcessNewBlockHeader(found_header, state)) {
        LOG_ERROR("Miner: Failed to process mined block: {} - {}",
                  state.GetRejectReason(), state.GetDebugMessage());
        // BUG #2: No continue here! Falls through to template creation
      }

      // BUG #3: Checks template height, not actual chain height
      int target = target_height_.load();
      if (target != -1 && current_template_.nHeight >= target) {
        LOG_INFO("Miner: Reached target height {}, stopping", target);
        mining_.store(false);
        break;
      }

      // Continue mining next block
      current_template_ = CreateBlockTemplate();  // Line 157
      template_prev_hash_ = current_template_.hashPrevBlock;
      nonce = 0;
      continue;
    }

    // Update stats
    total_hashes_.fetch_add(1);

    // Next nonce
    nonce++;
    if (nonce == 0) {
      // RACE: Mining thread writes current_template_
      current_template_.header.nTime++;  // Line 171
      LOG_DEBUG("Miner: Nonce space exhausted, incremented nTime to {}",
                current_template_.header.nTime);
    }
  }
}
```

---

## Recommended Fixes

### Priority 1: Critical Bugs (Must Fix)

**1. Fix Infinite Loop (Bug #2)**

`miner.cpp:143-146`:
```cpp
if (!chainstate_.ProcessNewBlockHeader(found_header, state)) {
  LOG_ERROR("Miner: Failed to process mined block: {} - {}",
            state.GetRejectReason(), state.GetDebugMessage());
  continue;  // ADD THIS - skip to next iteration, don't create template
}
```

**2. Fix Height Check (Bug #3)**

`miner.cpp:149-154`:
```cpp
// Check if we've reached target height
int target = target_height_.load();
const chain::CBlockIndex *tip = chainstate_.GetTip();
int actual_height = tip ? tip->nHeight : -1;  // Use ACTUAL chain height
if (target != -1 && actual_height >= target) {
  LOG_INFO("Miner: Reached target height {}, stopping", target);
  mining_.store(false);
  break;
}
```

**3. Fix Template Race Condition (Bug #1)**

**Option A**: Add mutex (simple, safe)
```cpp
// In miner.hpp:
mutable std::mutex template_mutex_;

// In all access points:
std::lock_guard<std::mutex> lock(template_mutex_);
current_template_.header.nNonce = nonce;
```

**Option B**: Local copy in mining thread (cleaner, avoids locking)
```cpp
// In MiningWorker():
BlockTemplate local_template = CreateBlockTemplate();

while (mining_.load()) {
  if (ShouldRegenerateTemplate()) {
    local_template = CreateBlockTemplate();
    nonce = 0;
  }

  local_template.header.nNonce = nonce;
  // ... rest of logic uses local_template ...
}
```

### Priority 2: Medium Severity (Should Fix)

**4. Fix mining_address_ Race (Bug #4)**

```cpp
// In miner.hpp:
mutable std::mutex address_mutex_;

// In SetMiningAddress:
void SetMiningAddress(const uint160& address) {
  std::lock_guard<std::mutex> lock(address_mutex_);
  mining_address_ = address;
}

// In CreateBlockTemplate:
uint160 GetMiningAddress() const {
  std::lock_guard<std::mutex> lock(address_mutex_);
  return mining_address_;
}

// Then in CreateBlockTemplate line 198:
tmpl.header.minerAddress = GetMiningAddress();
```

### Priority 3: Nice to Have

**5. Replace RPC Polling with Condition Variable**

This is more complex and low priority for regtest. Can be done later.

---

## Testing Recommendations

After fixes, test:

1. **Validation Failure Test**: Create scenario where validation fails mid-mining
   - Should NOT infinite loop
   - Should stop cleanly
   - Should report failure to RPC

2. **Concurrent RPC Test**: Call `generate` while miner is already running
   - Should handle gracefully
   - No crashes or data races

3. **Stress Test**: Run chaos convergence test again
   - Should complete without timeouts
   - No infinite loops even if validation fails

4. **Height Check Test**: Ensure miner stops at correct height
   - Even if some blocks fail validation
   - Check actual chain height, not template height

---

## Current Status

**Fixed**:
- ✅ Network expiration timebomb (disabled for regtest)
- ✅ NetworkExpired notification system (for mainnet/testnet)

**Pending**:
- ❌ Bug #1: Template race condition
- ❌ Bug #2: Infinite loop on validation failure
- ❌ Bug #3: Incorrect height check
- ❌ Bug #4: mining_address_ race condition
- ❌ Minor: RPC polling inefficiency

---

## Notes

- This is a **regtest-only** miner, not for production
- Designed to be simple, single-threaded
- But has threading issues due to RPC control from separate thread
- All bugs are fixable with ~50 lines of code changes
- Recommended to fix all Priority 1 bugs before production use of testnet/mainnet

---

## Next Steps

1. Fix Bug #2 (infinite loop) - 1 line change
2. Fix Bug #3 (height check) - 3 lines
3. Fix Bug #1 (template race) - prefer Option B (local copy)
4. Fix Bug #4 (address race) - add mutex
5. Test thoroughly
6. Consider refactoring RPC polling (low priority)

---

**End of Report**
