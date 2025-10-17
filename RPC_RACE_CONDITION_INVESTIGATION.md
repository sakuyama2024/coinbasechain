# RPC Server and Suspicious Reorg Investigation

## Issue 1: RPC Socket Connection Failure (SOLVED)

The `feature_suspicious_reorg.py` test failed to connect to RPC socket even though:
- Logs show "RPC server started successfully"
- Process is running
- Manual RPC calls work fine
- Identical `TestNode` framework works for `p2p_reorg.py`

## Root Cause (Issue 1)
**Unix domain socket path length limit exceeded!**

- macOS/BSD systems limit Unix socket paths to 104 bytes (including null terminator)
- Test used prefix `coinbasechain_suspicious_reorg_` which resulted in paths like:
  `/var/folders/.../coinbasechain_suspicious_reorg_XXXXXXXX/node0/node.sock` = 104 bytes
- The socket filename was silently truncated to `node.soc` (missing 'k')
- TestNode framework was looking for `node.sock`, couldn't find it, timed out

## Solution (Issue 1)
Changed test directory prefix from `coinbasechain_suspicious_reorg_` to `cbc_susp_`:
```python
test_dir = Path(tempfile.mkdtemp(prefix="cbc_susp_"))
```

This reduced the path length enough to avoid truncation.

## Issue 2: Suspicious Reorg Detection Not Working (DISCOVERED)

After fixing the socket path issue, the test now runs but **fails because the suspicious reorg protection doesn't work**:
- Node1 configured with `--suspiciousreorgdepth=5`
- Node1 at height 5, Node0 mines to height 17 (would require 12-block reorg)
- When nodes connect, Node1 **accepts the 12-block reorg** when it should refuse

## Original Hypothesis (Issue 1 - INCORRECT)
Suspected race condition where `RPCServer::Start()` returns before the server thread actually reaches `accept()`, causing early connection attempts to fail.

## Attempted Fix
Added synchronization to `/Users/mike/Code/coinbasechain/src/rpc/rpc_server.cpp`:

1. **Added members** (`include/rpc/rpc_server.hpp`):
   - `std::atomic<bool> server_ready_`
   - `std::mutex ready_mutex_`
   - `std::condition_variable ready_cv_`

2. **Modified `Start()`** to wait for server thread to be ready:
   ```cpp
   running_ = true;
   server_ready_ = false;
   server_thread_ = std::thread(&RPCServer::ServerThread, this);

   // Wait for server thread to reach accept() loop (with timeout)
   {
       std::unique_lock<std::mutex> lock(ready_mutex_);
       if (!ready_cv_.wait_for(lock, std::chrono::seconds(5),
                              [this]() { return server_ready_.load(); })) {
           LOG_ERROR("RPC server thread failed to start within timeout");
           // cleanup and return false
       }
   }
   ```

3. **Modified `ServerThread()`** to signal when ready:
   ```cpp
   void RPCServer::ServerThread() {
       // Signal that we're ready to accept connections
       {
           std::lock_guard<std::mutex> lock(ready_mutex_);
           server_ready_ = true;
       }
       ready_cv_.notify_one();

       while (running_) {
           // ... accept loop
       }
   }
   ```

## Attempted Fix (Issue 1 - REVERTED)
- Synchronization fix didn't resolve the test failure
- `p2p_reorg.py` continues to work fine
- Manual testing works fine
- The issue was actually the socket path length, not a race condition

## Test Framework Changes (KEPT)
Modified `/Users/mike/Code/coinbasechain/test/functional/test_framework/test_node.py`:

1. Changed subprocess `stdout/stderr` from `PIPE` to `DEVNULL` (prevents buffer blocking)
2. Modified `wait_for_rpc_connection()` to actually try RPC calls instead of just checking socket file existence

These changes are beneficial and were kept.

## Root Cause Analysis (Issue 2): Incremental Header Processing Bypasses Reorg Check

The suspicious reorg protection in `ChainstateManager::ActivateBestChain()` (lines 77-94) calculates reorg depth as:
```cpp
int reorg_depth = 0;
const chain::CBlockIndex* pindexCount = pindexOldTip;
while (pindexCount && pindexCount != pindexFork) {
    reorg_depth++;
    pindexCount = pindexCount->pprev;
}
```

**The Problem:**
When Node1 receives headers from Node0 via P2P, they arrive incrementally (one at a time). Each header triggers `ProcessNewBlockHeader()` → `ActivateBestChain()`.

**Example:**
- Node1 starts at height 5 (tip = block 5)
- Node0 sends header for height 6 from competing chain
  - Fork point: block 5 (common ancestor)
  - Current tip: block 5
  - Reorg depth: 0 blocks to disconnect
  - ✓ Accepted (0 ≤ 5 limit)
- Node0 sends header for height 7
  - Fork point: still block 5
  - Current tip: NOW block 6 (just updated!)
  - Reorg depth: 1 block to disconnect
  - ✓ Accepted (1 ≤ 5 limit)
- This continues for all 12 blocks...

Each individual header only sees a small reorg depth relative to the **constantly-updating tip**, never the original tip before the sync started.

**The Fix Needed:**
Track the tip at the start of header sync and calculate reorg depth against that original tip, not the incrementally-updated tip. This requires coordination between `HeaderSync` and `ChainstateManager`.

## How Unicity Handles This

Unicity solves this problem by using `ActivateBestChainStep()` which processes entire reorgs in a single call:

```cpp
// validation.cpp:821 - ActivateBestChainStep()
const CBlockIndex* pindexOldTip = m_chain.Tip();  // Capture ORIGINAL tip
const CBlockIndex* pindexFork = m_chain.FindFork(pindexMostWork);

// Check reorg depth BEFORE disconnecting any blocks
auto reorgLength = pindexOldTip->nHeight - pindexFork->nHeight;
if (reorgLength >= suspiciousDepth) {
    // REJECT THE ENTIRE REORG
    return false;
}

// Only now disconnect/connect blocks
while (m_chain.Tip() && m_chain.Tip() != pindexFork) {
    DisconnectTip(state);
}
```

**Key insight:** Unicity's `ActivateBestChain()` calls `ActivateBestChainStep()` which processes ALL blocks in a reorg as a single atomic operation. The reorg check happens ONCE at the start with the original tip captured.

CoinbaseChain processes headers incrementally (one at a time), so each call to `ActivateBestChain()` only sees a 1-block change, never realizing it's part of a larger reorg.
