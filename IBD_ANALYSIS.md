# Initial Block Download (IBD) - Unicity vs CoinbaseChain

## How Unicity Determines IBD Status

From `validation.cpp:192`, Unicity considers the node to be in IBD if **ANY** of these conditions are true:

```cpp
bool ChainstateManager::IsInitialBlockDownload() const
{
    // 1. Still loading blocks from disk
    if (m_blockman.LoadingBlocks()) {
        return true;
    }

    // 2. No tip yet (just started)
    if (chain.Tip() == nullptr) {
        return true;
    }

    // 3. Chain doesn't have minimum required work
    if (chain.Tip()->nChainWork < MinimumChainWork()) {
        return true;
    }

    // 4. Tip is too old (more than max_tip_age behind current time)
    //    Default max_tip_age = 24 hours
    if (chain.Tip()->Time() < Now<NodeSeconds>() - m_options.max_tip_age) {
        return true;
    }

    // Once all conditions pass, latch to false (never go back to IBD)
    m_cached_finished_ibd.store(true, std::memory_order_relaxed);
    return false;
}
```

## What Happens During IBD

### 1. **Headers-First Sync**
- Download all headers from peers (up to 2000 at a time via GETHEADERS/HEADERS)
- Validate headers (PoW, timestamps, connects to known chain)
- Build header chain in memory
- **For Unicity**: This downloads ~10+ years of headers (millions of headers)

### 2. **Block Download** (Full nodes only)
- Download actual block data (transactions) for validated headers
- Process transactions, update UTXO set
- This is the SLOW part (GB of data)

### 3. **Exit IBD** When:
- ✅ Chain has sufficient work
- ✅ Tip is recent (< 24 hours old)
- ✅ All blocks up to tip are downloaded and validated

## For CoinbaseChain (Headers-Only)

### Our Simplifications

**We have NO transactions**, so:

1. **Headers = Blocks**: 100 bytes each
   - 1 million headers = ~100 MB
   - 10 million headers = ~1 GB
   - Download in minutes, not days!

2. **No UTXO set**: No transaction validation
   - Skip the slowest part of IBD
   - Just validate PoW + chain structure

3. **No block files**: Everything in memory
   - No disk I/O bottleneck
   - Simple persistence (one JSON/binary file)

### Simple IBD for CoinbaseChain

```cpp
bool IsInitialBlockDownload() const {
    // 1. No tip yet?
    if (chain.Tip() == nullptr) {
        return true;
    }

    // 2. Tip too old? (> 1 hour, since we have 2-min blocks)
    if (chain.Tip()->nTime < Now() - 3600) {
        return true;
    }

    // That's it! No need for:
    // - MinimumChainWork check (headers are tiny, just download all)
    // - LoadingBlocks check (no blocks to load)

    return false;
}
```

### What This Means

**Unicity IBD:**
- Download headers: ~30 minutes
- Download blocks: **HOURS to DAYS** (depending on hardware)
- Validate transactions: **HOURS**
- Total: Can take days on slow hardware

**CoinbaseChain IBD:**
- Download headers: **< 1 minute** (even for 10M headers at 100KB/sec = ~100 seconds)
- Validate headers: **< 1 minute** (just PoW checks, no tx validation)
- Total: **Minutes at most**

## Implications for Design

### We Can Simplify:

1. **No presync/redownload**: Just download headers once
   - Unicity's `HeadersSyncState` two-phase sync is anti-DoS for slow IBD
   - We're so fast, DoS risk is minimal

2. **No minimum chain work**: Just sync to longest valid chain
   - Headers are cheap, can afford to download competing forks

3. **No block file management**: Store all headers in memory
   - Even 10M headers × 120 bytes = ~1.2 GB (fits in RAM)

4. **No complex state machine**: Just "syncing" or "synced"
   - Syncing: Tip is old, actively requesting headers
   - Synced: Tip is recent, just listening for new headers

5. **No bandwidth management**: Headers are tiny
   - Bitcoin worries about bandwidth because blocks are huge
   - Our headers: 2000 headers = 200 KB (negligible)

### What We Still Need:

1. **Header validation**: PoW, timestamps, difficulty
2. **Chain selection**: Choose longest valid chain
3. **Peer management**: Don't get stuck on bad peers
4. **Reorganization**: Handle competing forks

But all of this is **simpler** because headers are small and fast.

## Recommended IBD Implementation

### Phase 1: Simple Sync
```cpp
class HeaderSync {
    enum class State {
        SYNCING,   // Downloading headers
        SYNCED     // Up to date
    };

    bool is_synced() const {
        return state_ == State::SYNCED &&
               chain_.Tip() != nullptr &&
               chain_.Tip()->nTime > Now() - 3600;
    }

    void sync_from_peer(Peer& peer) {
        // Just send GETHEADERS repeatedly until caught up
        while (!is_synced()) {
            send_getheaders(peer);
            auto headers = receive_headers(peer);
            process_headers(headers);
        }
    }
};
```

### Phase 2: Add Anti-DoS (Later)
- Peer timeouts
- Misbehavior scoring
- Ban peers sending invalid headers

### Phase 3: Optimize (Later)
- Parallel downloads from multiple peers
- Checkpoint enforcement
- Compressed headers (68 bytes vs 100 bytes)

## Key Insight

**Bitcoin/Unicity IBD is complex because blocks are huge.**

Headers-only blockchain has:
- 1000× less data to download
- 100× less computation (no tx validation)
- **Much simpler IBD logic**

We should embrace this simplicity!

---

## Comparison Table

| Feature | Unicity (Full Node) | CoinbaseChain (Headers-Only) |
|---------|---------------------|------------------------------|
| Data to download | ~500 GB blocks | ~100 MB headers |
| Download time | Hours to days | < 1 minute |
| Validation | Transactions + PoW | PoW only |
| Storage | Disk (LevelDB) | Memory (simple file) |
| IBD complexity | 2-phase presync | Single-pass |
| Anti-DoS | Essential | Nice-to-have |
| MinimumChainWork | Required | Optional |
| Bandwidth concern | Critical | Negligible |

**Conclusion**: We can have a **dramatically simpler** IBD implementation!
