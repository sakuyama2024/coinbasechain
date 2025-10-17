# Chain Storage Design

## Unicity's Architecture

Unicity (Bitcoin Core) uses a **three-layer architecture** for blockchain storage:

### 1. **CBlockIndex** - Block Metadata Node
```cpp
class CBlockIndex {
    const uint256* phashBlock;      // Hash of this block
    CBlockIndex* pprev;             // Pointer to previous block (parent)
    CBlockIndex* pskip;             // Skip list for efficient ancestry lookup
    int nHeight;                    // Height in chain
    arith_uint256 nChainWork;       // Cumulative work to this block
    uint32_t nStatus;               // Validation status flags

    // Block header fields inline:
    int32_t nVersion;
    uint160 minerAddress;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;
    uint256 hashRandomX;

    // File storage locations:
    int nFile;                      // Which blk*.dat file
    unsigned int nDataPos;          // Offset in file
    unsigned int nUndoPos;          // Undo data offset
};
```

**Purpose**:
- Represents a single block's **metadata**
- Forms a **tree structure** via `pprev` pointers (allows multiple forks)
- Contains validation status, cumulative work
- Points to on-disk storage location
- Has header data **inline** (no separate CBlockHeader storage)

### 2. **CChain** - Active Chain Vector
```cpp
class CChain {
private:
    std::vector<CBlockIndex*> vChain;  // Linear array of pointers

public:
    CBlockIndex* Genesis() const;       // First block
    CBlockIndex* Tip() const;           // Last block (current tip)
    CBlockIndex* operator[](int height); // Access by height
    bool Contains(const CBlockIndex*);  // Is block in THIS chain?
    CBlockIndex* Next(const CBlockIndex*); // Successor in chain
    int Height() const;                 // Chain height
    void SetTip(CBlockIndex&);          // Set new tip
    CBlockLocator GetLocator() const;   // For GETHEADERS
    const CBlockIndex* FindFork(const CBlockIndex*); // Find common ancestor
};
```

**Purpose**:
- Represents **one specific chain** (the "active" or "best" chain)
- Fast `O(1)` access by height: `chain[100]`
- Used for: main chain, competing forks during reorg
- **Does not own** the blocks - just points to CBlockIndex objects

### 3. **BlockManager** - Global Block Index
```cpp
class BlockManager {
private:
    // ALL known blocks (including orphans, all forks)
    std::unordered_map<uint256, CBlockIndex> m_block_index;

    // The currently active chain
    CChain m_active_chain;

public:
    CBlockIndex* LookupBlockIndex(const uint256& hash);
    CBlockIndex* AddToBlockIndex(const CBlockHeader& block);
    bool LoadBlockIndex();
    bool WriteBlockIndexDB();
};
```

**Purpose**:
- Owns **ALL** CBlockIndex objects (all forks, orphans)
- Provides hash → CBlockIndex lookup
- Manages persistence (load/save from LevelDB)
- Manages active chain pointer

## How They Work Together

```
                    BlockManager
                    ┌──────────────────────────────┐
                    │ m_block_index (hash map)     │
                    │ ┌──────┐ ┌──────┐ ┌──────┐  │
                    │ │Block0│ │Block1│ │Block2│  │
                    │ └──┬───┘ └──┬───┘ └──┬───┘  │
                    │    │        │        │       │
                    └────┼────────┼────────┼───────┘
                         │        │        │
                    ┌────▼────────▼────────▼───────┐
                    │ m_active_chain (CChain)      │
                    │ vChain = [ptr0, ptr1, ptr2]  │
                    │           ▲              ▲    │
                    │           │              │    │
                    │       Genesis()       Tip()   │
                    └──────────────────────────────┘
```

**Tree Structure** (via pprev):
```
                    ┌──────┐
                    │Block0│ (genesis)
                    └──┬───┘
                       │
                    ┌──▼───┐
                    │Block1│
                    └──┬───┘
                       │
              ┌────────┴────────┐
              │                 │
           ┌──▼───┐          ┌──▼───┐
           │Block2│ (fork A) │Block2'│ (fork B)
           └──┬───┘          └──────┘
              │
           ┌──▼───┐
           │Block3│
           └──────┘
```

- BlockManager contains ALL blocks (Block2 and Block2')
- CChain (active) points to one path: [Block0, Block1, Block2, Block3]

## Key Algorithms

### GetLocator() - Exponential Backoff
```cpp
// Returns: [tip, tip-1, tip-2, tip-4, tip-8, tip-16, ..., genesis]
std::vector<uint256> LocatorEntries(const CBlockIndex* index) {
    int step = 1;
    std::vector<uint256> have;
    while (index) {
        have.push_back(index->GetBlockHash());
        if (index->nHeight == 0) break;
        int height = max(index->nHeight - step, 0);
        index = index->GetAncestor(height);  // Use skip list
        if (have.size() > 10) step *= 2;     // Exponential backoff
    }
    return have;
}
```

**Purpose**: Find common ancestor with peer efficiently
- Start dense (recent blocks)
- Get sparse (older blocks)
- Always include genesis

### SetTip() - Activate New Chain
```cpp
void CChain::SetTip(CBlockIndex& block) {
    CBlockIndex* pindex = &block;
    vChain.resize(pindex->nHeight + 1);
    while (pindex && vChain[pindex->nHeight] != pindex) {
        vChain[pindex->nHeight] = pindex;
        pindex = pindex->pprev;  // Walk backwards
    }
}
```

**Purpose**: Set a new tip and populate entire chain vector
- Walk backwards from tip using pprev
- Fill vChain[height] = block for each height

### FindFork() - Common Ancestor
```cpp
const CBlockIndex* CChain::FindFork(const CBlockIndex* pindex) const {
    if (pindex->nHeight > Height())
        pindex = pindex->GetAncestor(Height());  // Bring to our level
    while (pindex && !Contains(pindex))
        pindex = pindex->pprev;  // Walk back until found
    return pindex;
}
```

**Purpose**: Find where another chain branches off from this chain

## Our Simplified Implementation

For **headers-only chain**, we can simplify:

### What We NEED:
1. **CBlockIndex** - Block metadata with tree structure
   - ✅ pprev pointer (tree structure)
   - ✅ nHeight
   - ✅ nChainWork (for chain selection)
   - ✅ Header data inline
   - ❌ Skip list (can add later for performance)
   - ❌ File positions (we store headers in memory/simple file)
   - ❌ Validation status flags (simplified)

2. **CChain** - Active chain vector
   - ✅ vChain vector
   - ✅ Tip(), Genesis(), operator[]
   - ✅ GetLocator()
   - ✅ SetTip()
   - ✅ FindFork()

3. **BlockManager** - Block index map
   - ✅ m_block_index (hash map)
   - ✅ m_active_chain
   - ✅ LookupBlockIndex()
   - ✅ AddToBlockIndex()
   - ✅ Simple file persistence (not LevelDB initially)

### Simplifications:

- **No file positions**: Store all headers in memory + simple binary file
- **No undo data**: Headers-only, no transactions
- **No status flags**: Just track if validated
- **No skip list**: Can walk pprev (small chain initially)
- **Single active chain**: No reorg logic initially (assume honest longest chain)

### File Structure:

```
include/chain/
  block_index.hpp       // CBlockIndex class
  chain.hpp             // CChain class
  block_manager.hpp     // BlockManager class

src/chain/
  block_index.cpp
  chain.cpp
  block_manager.cpp
```

### Persistence Format:

Simple binary file for now (LevelDB later):
```
headers.dat:
<num_blocks: 4 bytes>
<block0_hash: 32 bytes><block0_header: 100 bytes><height: 4 bytes><chainwork: 32 bytes>
<block1_hash: 32 bytes><block1_header: 100 bytes><height: 4 bytes><chainwork: 32 bytes>
...
```

Or JSON (easier to debug):
```json
{
  "blocks": [
    {
      "hash": "...",
      "header": {...},
      "height": 0,
      "chainwork": "...",
      "prev_hash": "0000..."
    }
  ],
  "tip_hash": "..."
}
```

## Next Steps

1. ✅ Create CHAIN_DESIGN.md (this file)
2. ⏳ Implement CBlockIndex
3. ⏳ Implement CChain
4. ⏳ Implement BlockManager
5. ⏳ Update HeaderSync to use these classes
6. ⏳ Add unit tests

---

**Why this architecture?**

- **CBlockIndex tree** allows tracking multiple forks simultaneously
- **CChain vector** gives O(1) height access for active chain
- **BlockManager** centralizes all block storage and chain selection logic
- **Separation** makes it easy to switch active chains during reorg
