# Header Sync Design

## Unicity's Approach

Unicity has a **sophisticated anti-DoS two-phase header sync** (`headerssync.h`):

### Phase 1: PRESYNC
- Download headers and calculate work without storing in memory
- Store commitments (hashed bits) to verify later
- Once sufficient work is found, switch to REDOWNLOAD

### Phase 2: REDOWNLOAD
- Re-download the same headers
- Verify commitments match
- Only then accept into block index (permanent memory)

**Purpose**: Prevent memory DoS from low-work headers

### Key Features:
- `CompressedHeader` - saves memory by omitting prevhash (68 bytes vs 100 bytes)
- Commitment system with salted hashing
- Work threshold calculations
- MTP (Median Time Past) bounds on chain length
- Handles unconnecting headers (BIP 130 announcements)

## Our Simplified Approach (Phase 6)

For a headers-only chain with **no transactions**, we can start much simpler:

### Initial Implementation (MVP):

```cpp
class HeaderSync {
public:
    // Request headers starting from our best known block
    void request_headers();

    // Process received HEADERS message
    bool process_headers(const std::vector<CBlockHeader>& headers);

    // Check if we're in sync
    bool is_synced() const;

private:
    // Our current best header
    CBlockHeader best_header_;
    uint256 best_hash_;
    int best_height_;

    // Simple chain storage (just headers, no full blocks!)
    std::map<uint256, CBlockHeader> headers_by_hash_;
    std::map<int, uint256> headers_by_height_;

    // Track sync state
    bool syncing_;
    int64_t last_request_time_;
};
```

### Message Flow:

1. **Initial Sync**:
   ```
   Us  -> Peer: GETHEADERS (locator with our best hash)
   Peer -> Us: HEADERS (up to 2000 headers)
   Us  -> Peer: GETHEADERS (continue from last received)
   ... repeat until we get < 2000 headers (we're synced)
   ```

2. **Stay in Sync**:
   ```
   Peer -> Us: INV (new block announcement)
   Us  -> Peer: GETHEADERS (request headers)
   Peer -> Us: HEADERS (new headers)
   ```

### Validation (Simple Version):

For each header received:
1. ✅ Check PoW (nBits matches hash)
2. ✅ Check connects to known header (hashPrevBlock exists)
3. ✅ Check timestamp rules (not too far in future)
4. ✅ Check difficulty adjustment (future phase)
5. ✅ Store in our header chain

### What We're Skipping Initially:

- ❌ Two-phase presync/redownload (anti-DoS)
- ❌ Work threshold checks (assume honest peers)
- ❌ Commitment verification
- ❌ CompressedHeader optimization
- ❌ Multiple competing chains (just follow longest)
- ❌ Checkpoints
- ❌ Difficulty adjustment validation (hardcode initial difficulty)

### Persistence:

```cpp
// Save headers to disk periodically
void save_headers_to_disk();

// Load on startup
void load_headers_from_disk();

// Format: Simple binary file or JSON
// <height><hash><header_data>...
```

## Tier 2: Add Anti-DoS (Phase 9+)

Later, when we connect to untrusted peers:

1. **Work Threshold**: Only store headers with sufficient cumulative work
2. **Connection Limits**: Limit in-flight header requests per peer
3. **Misbehavior**: Punish peers sending invalid headers
4. **Compressed Headers**: Save memory during sync

## Tier 3: Full Unicity Parity (Phase 10+)

- Full two-phase sync with commitments
- Checkpoint enforcement
- Multiple chain tracking
- BIP 130 unconnecting headers handling

## File Structure

```
include/sync/
  header_sync.hpp          # HeaderSync class

src/sync/
  header_sync.cpp          # Implementation

test/
  header_sync_tests.cpp    # Unit tests
```

## Integration with Network Manager

```cpp
// In NetworkManager or PeerManager:
void handle_headers_message(PeerPtr peer, std::vector<CBlockHeader> headers) {
    if (header_sync_->process_headers(headers)) {
        // Request more if we got a full message (2000 headers)
        if (headers.size() == MAX_HEADERS_RESULTS) {
            send_getheaders(peer);
        }
    }
}

void handle_inv_message(PeerPtr peer, std::vector<CInv> inv) {
    // If peer announces new block, request headers
    for (const auto& item : inv) {
        if (item.type == MSG_BLOCK) {
            send_getheaders(peer);
            break;
        }
    }
}
```

## Constants

```cpp
static constexpr size_t MAX_HEADERS_RESULTS = 2000;  // Bitcoin protocol limit
static constexpr int64_t MAX_FUTURE_BLOCK_TIME = 2 * 60 * 60;  // 2 hours
static constexpr int64_t GETHEADERS_TIMEOUT = 60;  // 60 seconds
```

## Testing Strategy

1. **Unit Tests**:
   - Valid header chain processing
   - Invalid PoW detection
   - Disconnected headers rejection
   - Future timestamp rejection

2. **Integration Tests**:
   - Sync from genesis
   - Sync from mid-chain
   - Handle reorgs (later)

## Recommendation

**Start with MVP** (simple single-chain storage and sync). This gets us:
- Basic header synchronization working
- Foundation for block sync (Phase 7)
- Something we can actually test

**Add complexity later** when we:
- Connect to untrusted mainnet peers
- Need memory optimization
- Face DoS attacks

---

**Next Steps**: Implement MVP HeaderSync with simple linear chain storage.
