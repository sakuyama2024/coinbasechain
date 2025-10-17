# Chain Simplification for Headers-Only

## What Unicity Has (That We Don't Need)

### CBlockIndex - What to Remove:

```cpp
// ❌ REMOVE - No transaction data
unsigned int nTx;           // Number of transactions
unsigned int nChainTx;      // Cumulative transactions

// ❌ REMOVE - No file storage (small enough for memory)
int nFile;                  // Which blk*.dat file
unsigned int nDataPos;      // File offset
unsigned int nUndoPos;      // Undo data offset

// ❌ REMOVE - No complex validation (just PoW check)
uint32_t nStatus;           // BLOCK_VALID_TREE, BLOCK_VALID_SCRIPTS, etc.

// ❌ REMOVE - No skip list (chain is small, can walk pprev)
CBlockIndex* pskip;         // Skip list pointer

// ❌ REMOVE - Sequence ID (no mempool/relay tracking)
int32_t nSequenceId;

// ❌ REMOVE - Max time tracking (for MTP, but we can calculate on demand)
unsigned int nTimeMax;
```

### What to KEEP:

```cpp
// ✅ KEEP - Core tree structure
const uint256* phashBlock;      // Our hash
CBlockIndex* pprev;             // Parent (tree structure)
int nHeight;                    // Height

// ✅ KEEP - Chain selection
arith_uint256 nChainWork;       // For "most work" selection

// ✅ KEEP - Header data (this IS our block!)
int32_t nVersion;
uint160 minerAddress;
uint32_t nTime;
uint32_t nBits;
uint32_t nNonce;
uint256 hashRandomX;
```

## Simplified CBlockIndex

```cpp
class CBlockIndex {
public:
    // Pointer to hash (owned by BlockManager's map key)
    const uint256* phashBlock{nullptr};

    // Tree structure
    CBlockIndex* pprev{nullptr};
    int nHeight{0};

    // Chain selection
    arith_uint256 nChainWork{};

    // Header data (this IS the block!)
    int32_t nVersion{0};
    uint160 minerAddress;
    uint32_t nTime{0};
    uint32_t nBits{0};
    uint32_t nNonce{0};
    uint256 hashRandomX;

    // Constructor from header
    explicit CBlockIndex(const CBlockHeader& block)
        : nVersion{block.nVersion},
          minerAddress{block.minerAddress},
          nTime{block.nTime},
          nBits{block.nBits},
          nNonce{block.nNonce},
          hashRandomX{block.hashRandomX}
    {
    }

    // Get header back
    CBlockHeader GetBlockHeader() const {
        CBlockHeader block;
        block.nVersion = nVersion;
        if (pprev)
            block.hashPrevBlock = pprev->GetBlockHash();
        block.minerAddress = minerAddress;
        block.nTime = nTime;
        block.nBits = nBits;
        block.nNonce = nNonce;
        block.hashRandomX = hashRandomX;
        return block;
    }

    uint256 GetBlockHash() const {
        assert(phashBlock != nullptr);
        return *phashBlock;
    }

    // Calculate median time past (on demand, not cached)
    int64_t GetMedianTimePast() const {
        int64_t pmedian[11];
        int64_t* pbegin = &pmedian[11];
        int64_t* pend = &pmedian[11];

        const CBlockIndex* pindex = this;
        for (int i = 0; i < 11 && pindex; i++, pindex = pindex->pprev)
            *(--pbegin) = pindex->nTime;

        std::sort(pbegin, pend);
        return pbegin[(pend - pbegin) / 2];
    }

    // Simple ancestry (no skip list, just walk pprev)
    const CBlockIndex* GetAncestor(int height) const {
        if (height > nHeight || height < 0)
            return nullptr;

        const CBlockIndex* pindex = this;
        while (pindex && pindex->nHeight > height)
            pindex = pindex->pprev;

        return pindex;
    }
};
```

**Size comparison**:
- Unicity: ~150+ bytes (with all the extra fields)
- Ours: ~120 bytes (just essentials)

## CChain - Unchanged

CChain is already simple and perfect for us:

```cpp
class CChain {
private:
    std::vector<CBlockIndex*> vChain;

public:
    CBlockIndex* Genesis() const;
    CBlockIndex* Tip() const;
    CBlockIndex* operator[](int nHeight) const;
    bool Contains(const CBlockIndex* pindex) const;
    int Height() const;
    void SetTip(CBlockIndex& block);
    CBlockLocator GetLocator() const;
    const CBlockIndex* FindFork(const CBlockIndex* pindex) const;
};
```

**Keep as-is** - it's already minimal.

## BlockManager - Simplified

```cpp
class BlockManager {
private:
    // All known blocks (hash -> CBlockIndex)
    // The map owns the CBlockIndex objects
    std::map<uint256, CBlockIndex> m_block_index;

    // The active chain
    CChain m_active_chain;

public:
    // Lookup block by hash
    CBlockIndex* LookupBlockIndex(const uint256& hash) {
        auto it = m_block_index.find(hash);
        if (it == m_block_index.end())
            return nullptr;
        return &it->second;
    }

    // Add new block header
    CBlockIndex* AddToBlockIndex(const CBlockHeader& header) {
        uint256 hash = header.GetHash();

        // Already have it?
        auto it = m_block_index.find(hash);
        if (it != m_block_index.end())
            return &it->second;

        // Create new entry
        auto [iter, inserted] = m_block_index.emplace(hash, CBlockIndex(header));
        CBlockIndex* pindex = &iter->second;

        // Set hash pointer
        pindex->phashBlock = &iter->first;

        // Connect to parent
        pindex->pprev = LookupBlockIndex(header.hashPrevBlock);
        if (pindex->pprev) {
            pindex->nHeight = pindex->pprev->nHeight + 1;
            pindex->nChainWork = pindex->pprev->nChainWork + GetBlockProof(*pindex);
        } else {
            // Genesis
            pindex->nHeight = 0;
            pindex->nChainWork = GetBlockProof(*pindex);
        }

        return pindex;
    }

    // Get active chain
    CChain& ActiveChain() { return m_active_chain; }

    // Simple persistence (JSON or binary)
    bool Save(const std::string& filepath);
    bool Load(const std::string& filepath);
};
```

**What we removed**:
- ❌ LevelDB persistence (use simple JSON/binary file)
- ❌ Block file management (blk*.dat, rev*.dat)
- ❌ UTXO tracking
- ❌ Tx index
- ❌ Block pruning
- ❌ Assume-valid snapshots

## Memory Usage Estimate

For 1 million headers:
- Unicity: ~150 bytes × 1M = ~150 MB
- Ours: ~120 bytes × 1M = ~120 MB

**With 10 million headers**:
- Ours: ~1.2 GB (easily fits in memory)

**Conclusion**: We can keep **all headers in memory**, no need for complex file management.

## Persistence Strategy

### Option 1: JSON (Simple, Debug-Friendly)
```json
{
  "blocks": [
    {
      "hash": "abc123...",
      "height": 0,
      "prev": "000000...",
      "version": 1,
      "minerAddress": "...",
      "time": 1234567890,
      "bits": 486604799,
      "nonce": 12345,
      "hashRandomX": "...",
      "chainwork": "0000000000000000000000000000000000000000000000000000000100010001"
    }
  ],
  "tip": "abc123..."
}
```

**Pros**: Easy to debug, human-readable
**Cons**: Larger files, slower parsing

### Option 2: Simple Binary (Fast, Compact)
```
<magic: 4 bytes = 0xD9B4BEF9>
<version: 4 bytes = 1>
<num_blocks: 4 bytes>
for each block:
  <hash: 32 bytes>
  <header: 100 bytes>
  <height: 4 bytes>
  <chainwork: 32 bytes>
<tip_hash: 32 bytes>
```

**Pros**: Fast, compact
**Cons**: Not human-readable

### Option 3: Hybrid
- Use JSON for initial development (easy debugging)
- Switch to binary later (performance)

## Recommended Architecture

```
include/chain/
  block_index.hpp       // Simplified CBlockIndex
  chain.hpp             // CChain (unchanged from Bitcoin)
  block_manager.hpp     // Simplified BlockManager

src/chain/
  block_index.cpp       // GetMedianTimePast(), GetAncestor()
  chain.cpp             // SetTip(), GetLocator(), FindFork()
  block_manager.cpp     // AddToBlockIndex(), Save(), Load()
```

## What This Enables

With these three simple classes:

1. **Store all headers** in memory (m_block_index)
2. **Track active chain** (m_active_chain)
3. **Handle forks** (multiple CBlockIndex branches)
4. **Generate locators** for GETHEADERS
5. **Persist to disk** (simple JSON/binary)
6. **Fast lookups** (hash → block, height → block)

## Implementation Order

1. ✅ Create design docs (this file)
2. ⏳ Implement CBlockIndex (simple version)
3. ⏳ Implement CChain (copy from Unicity, minimal changes)
4. ⏳ Implement BlockManager (simplified)
5. ⏳ Add GetBlockProof() for chain work calculation
6. ⏳ Add persistence (JSON initially)
7. ⏳ Add unit tests
8. ⏳ Integrate with HeaderSync

---

**Key Insight**: Headers-only means **the header IS the block**. We don't need:
- Separate storage for headers vs full blocks
- Tx count tracking
- UTXO sets
- Undo data
- Complex validation stages

We just need to track which header chain has the most work and persist it!
