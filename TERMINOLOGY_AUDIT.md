# Block vs Header Terminology Audit

## Current Status

Our codebase uses "block" and "header" inconsistently. Since we have a headers-only chain (no transactions), we need to standardize terminology.

## Recommended Convention

Follow Bitcoin Core convention:
- **"Header"** - The actual data structure (`CBlockHeader`)
- **"Block"** - Conceptual references (block height, block hash, blockchain)
- **"Block index"** - Metadata about a header (`CBlockIndex`)

## Current Inconsistencies

### Files with "block" in name
- `include/primitives/block.h` - Contains `CBlockHeader` class ✓ (OK - Bitcoin Core uses this)
- `src/primitives/block.cpp`
- `include/chain/block_index.hpp` - Contains `CBlockIndex` (metadata) ✓ (OK - standard name)
- `src/chain/block_index.cpp`
- `include/chain/block_manager.hpp` - Manages block storage
- `src/chain/block_manager.cpp`
- `test/block_tests.cpp` - Tests for CBlockHeader

### Files with "header" in name
- `include/sync/header_sync.hpp` - Syncs headers ✓ (OK)
- `src/sync/header_sync.cpp`
- `test/header_sync_tests.cpp`

### Mixed terminology in code

#### In `include/primitives/block.h`:
```cpp
/**
 * CBlockHeader - The complete block for a headers-only blockchain
 * ...
 * - This IS the complete block (no transactions)
 */
class CBlockHeader
```
**Issue**: Comment says "complete block" but class is named `CBlockHeader`

#### In `include/chain/block_index.hpp`:
```cpp
/**
 * CBlockIndex - Metadata for a single block header
 * ...
 * The header data is stored inline since the header IS the block.
 */
```
Functions use "block" terminology:
- `uint256 GetBlockHash()`
- `CBlockHeader GetBlockHeader()`
- `int64_t GetBlockTime()`
- `arith_uint256 GetBlockProof()`

**Analysis**: These are actually OK - they're conceptual "block" references. The function `GetBlockHeader()` correctly returns a `CBlockHeader`.

### Network protocol (need to check)
- Message types: "headers", "getheaders", "sendheaders" (Bitcoin style)
- Do we have any "block" messages that should be "header" messages?

## Proposed Changes

### HIGH PRIORITY - Clarify comments

1. **`include/primitives/block.h`** line 15-16:
   ```cpp
   // CURRENT:
   /**
    * CBlockHeader - The complete block for a headers-only blockchain
    * ...
    * - This IS the complete block (no transactions)
    */

   // CHANGE TO:
   /**
    * CBlockHeader - Block header structure
    *
    * In our headers-only blockchain, this header represents the entire block
    * (there are no transactions to store separately).
    *
    * Based on Unicity's block header, simplified for Coinbase Chain.
    * ...
    */
   ```

2. **`include/chain/block_index.hpp`** line 55:
   ```cpp
   // CURRENT:
   * The header data is stored inline since the header IS the block.

   // CHANGE TO:
   * The header data is stored inline (headers-only chain, no transactions).
   ```

### MEDIUM PRIORITY - Consider renaming test file

- `test/block_tests.cpp` → `test/header_tests.cpp`?
  - Pro: More accurate (tests CBlockHeader)
  - Con: Bitcoin Core calls similar tests "block_tests"
  - **Recommendation**: Keep as-is for Bitcoin Core compatibility

### LOW PRIORITY - Audit variable names

Search for variables named with "block" that actually hold headers:
- `CBlockHeader block` - OK (it's a header, but "block" is fine conceptually)
- `std::vector<CBlockHeader> blocks` - Could be `headers` but `blocks` is OK
- Need to check actual usage in sync/network code

### Documentation to add

Add to README or docs:

```markdown
## Terminology

Coinbase Chain is a headers-only blockchain (no transactions). We follow Bitcoin Core conventions:

- **Header**: The data structure `CBlockHeader` containing version, prev hash, miner address, timestamp, nBits, nonce, and RandomX hash
- **Block**: Conceptual term for a unit in the blockchain. In our case, a "block" is just a header.
- **Block index**: Metadata about a header (`CBlockIndex`) including validation status, chainwork, and tree links
- **Block hash**: The hash of a header (used to identify blocks)
- **Block height**: Position in the chain
- **Blockchain**: The sequence of headers forming the chain

When in doubt: use "header" for the data structure, "block" for conceptual references.
```

## Action Items

- [x] Update comments in `primitives/block.h` (line 15-22) ✅ DONE
- [x] Update comment in `chain/block_index.hpp` (line 55) ✅ DONE
- [x] Add terminology section to main README ✅ DONE
- [x] Audit network message handlers for consistent terminology ✅ DONE - Correct!
- [x] Audit RPC commands - do we say "getblock" or "getheader"? ✅ DONE - Correct!
- [x] Check log messages for consistency ✅ DONE - Correct!
- [x] Review function parameter names in sync/network code ✅ DONE - Correct!

## Audit Results Summary

### ✅ Network Messages (src/network/message.cpp)
- Uses `GETHEADERS` and `HEADERS` message types (Bitcoin protocol standard)
- Variable names use `headers` for `std::vector<CBlockHeader>` ✓
- Lines 353-354, 577-613 - All correct!

### ✅ RPC Commands (src/rpc/rpc_server.cpp)
All commands use correct terminology:
- `getblockchaininfo` - Conceptual blockchain info ✓
- `getblockhash` - Get hash by height ✓
- `getblockheader` - Get header data structure ✓
- `getbestblockhash` - Best block's hash ✓
- Returns both `"blocks"` and `"headers"` fields (same value in headers-only chain) ✓

### ✅ Log Messages
Reviewed 30+ log messages across the codebase:
- Uses "headers" for header sync operations ✓
- Uses "block" for conceptual references (block hash, genesis block) ✓
- Consistent with our convention throughout

Examples:
- `"Saving headers to disk..."` ✓
- `"Header sync complete!"` ✓
- `"Received {} headers from peer {}"` ✓
- `"Peer {} announced block: {}"` ✓ (conceptual)
- `"No common blocks in locator"` ✓ (conceptual)

### ✅ Variable Names
Spot-checked key files:
- `src/sync/header_sync.cpp:39` - Uses `headers` for `std::vector<CBlockHeader>` ✓
- Function parameters correctly named `headers` when receiving CBlockHeader vectors ✓

## Conclusion

**ALL TERMINOLOGY IS CORRECT!**

The codebase follows Bitcoin Core conventions perfectly:
- Uses "header" for data structures (`CBlockHeader`, header sync, header messages)
- Uses "block" for conceptual references (block hash, block height, blockchain)
- RPC commands match Bitcoin Core naming
- Log messages are clear and consistent
- Variable names are appropriate

The main issues were only in **documentation comments**, which have now been fixed.

## Files to review in detail

1. `src/network/message.cpp` - Message type names
2. `src/network/peer_manager.cpp` - Sync logic
3. `src/sync/header_sync.cpp` - Header sync protocol
4. `src/rpc/rpc_server.cpp` - RPC command names
5. `src/validation/validation.cpp` - Validation functions

## Notes

- Bitcoin Core uses `CBlockHeader` for the header and `CBlock` for header+transactions
- We only have `CBlockHeader` (no separate `CBlock` class)
- This is correct - we're a headers-only chain
- The inconsistency is mainly in comments and documentation, not code structure
