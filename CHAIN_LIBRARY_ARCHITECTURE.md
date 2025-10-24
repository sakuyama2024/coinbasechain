# Chain Library Architecture - Technical Deep Dive

**Version:** 1.0.0  
**Date:** 2025-10-24  
**Scope:** Chain library (`src/chain/*`, `include/chain/*`)  
**Related Documents:** `ARCHITECTURE.md`, `SECURITY_AUDIT.md`

---

## Table of Contents

1. [Overview](#1-overview)
2. [Core Data Structures](#2-core-data-structures)
3. [Component Architecture](#3-component-architecture)
4. [Validation Pipeline](#4-validation-pipeline)
5. [Concurrency Model](#5-concurrency-model)
6. [Consensus Mechanisms](#6-consensus-mechanisms)
7. [Memory Management](#7-memory-management)
8. [Error Handling](#8-error-handling)
9. [Performance Characteristics](#9-performance-characteristics)
10. [Design Patterns](#10-design-patterns)

---

## 1. Overview

### 1.1 Purpose

The chain library implements the **core blockchain validation and consensus logic** for a headers-only chain. It provides:

- **Block header validation** (PoW, timestamps, difficulty)
- **Chain selection** (longest chain by accumulated work)
- **Reorganization handling** (switching between competing forks)
- **Consensus enforcement** (ASERT difficulty, RandomX PoW)
- **Persistence** (saving/loading blockchain state)

### 1.2 Key Design Principles

| Principle | Implementation | Rationale |
|-----------|----------------|-----------|
| **Separation of Concerns** | Validation vs. Storage vs. Selection | Modularity, testability |
| **Thread Safety** | Single recursive mutex + atomic flags | Simplicity over fine-grained locking |
| **Headers-Only** | No transaction processing | Eliminates 99% of Bitcoin complexity |
| **DoS Protection** | Multi-layered validation + orphan limits | Security without sacrificing performance |
| **Bitcoin Compatibility** | 98% wire protocol compatibility | Leverage existing tooling |

### 1.3 Library Structure

```
chain/
├── Core Validation
│   ├── chainstate_manager.{hpp,cpp}  # Orchestrates validation & chain state
│   ├── validation.{hpp,cpp}          # Header validation rules
│   ├── pow.{hpp,cpp}                 # ASERT difficulty adjustment
│   └── randomx_pow.{hpp,cpp}         # RandomX PoW implementation
│
├── Data Structures
│   ├── block.{hpp,cpp}               # CBlockHeader (100-byte header)
│   ├── block_index.{hpp,cpp}         # CBlockIndex (metadata per block)
│   ├── chain.{hpp,cpp}               # CChain (linear chain representation)
│   └── uint.{hpp,cpp}                # uint256/uint160 types
│
├── Chain Management
│   ├── block_manager.{hpp,cpp}       # Manages all known blocks
│   ├── chain_selector.{hpp,cpp}      # Selects best chain among candidates
│   └── chainparams.{hpp,cpp}         # Network parameters (mainnet/regtest)
│
├── Cryptography
│   ├── sha256.{hpp,cpp}              # Bitcoin-compatible SHA256
│   ├── arith_uint256.{hpp,cpp}       # 256-bit arithmetic for difficulty
│   └── endian.hpp                    # Endianness handling
│
└── Utilities
    ├── logging.{hpp,cpp}             # Structured logging (spdlog)
    ├── time.{hpp,cpp}                # Time utilities
    ├── timedata.{hpp,cpp}            # Network-adjusted time
    └── threadpool.{hpp,cpp}          # Thread pool for async tasks
```

---

## 2. Core Data Structures

### 2.1 CBlockHeader - Wire Format

The fundamental unit of the blockchain. Exactly **100 bytes** on the wire:

```cpp
struct CBlockHeader {
    int32_t  nVersion;        // 4 bytes  - Protocol version
    uint256  hashPrevBlock;   // 32 bytes - Parent block hash
    uint160  minerAddress;    // 20 bytes - Miner reward address (replaces merkleRoot)
    uint32_t nTime;           // 4 bytes  - Unix timestamp
    uint32_t nBits;           // 4 bytes  - Difficulty target (compact format)
    uint32_t nNonce;          // 4 bytes  - PoW nonce
    uint256  hashRandomX;     // 32 bytes - RandomX hash (PoW commitment)
};
// Total: 100 bytes (no padding, verified by static_assert)
```

**Key Design Choices:**

| Field | Bitcoin Equivalent | CoinbaseChain Change | Rationale |
|-------|-------------------|---------------------|-----------|
| `minerAddress` | `hashMerkleRoot` | **Replaced** merkle root with miner address | Headers-only: no transactions, directly encode reward destination |
| `hashRandomX` | N/A (uses SHA256d) | **Added** 32-byte RandomX hash | RandomX requires hash to be stored in header for fast verification |
| `nTime` | Same | **1-hour target** (vs. 10 min) | Lower block frequency for use case |

**Wire Format (Little-Endian):**
```
Offset  Size  Field
------  ----  -----
0       4     nVersion       (LE int32)
4       32    hashPrevBlock  (byte array, no endian conversion)
36      20    minerAddress   (byte array, no endian conversion)
56      4     nTime          (LE uint32)
60      4     nBits          (LE uint32)
64      4     nNonce         (LE uint32)
68      32    hashRandomX    (byte array, no endian conversion)
```

**Critical Invariant:** `sizeof(CBlockHeader) == 100` enforced by `static_assert` because RandomX hashing uses `memcpy(&header, sizeof(header))`.

---

### 2.2 CBlockIndex - In-Memory Metadata

Metadata for each known block header, stored in `BlockManager::m_block_index`:

```cpp
class CBlockIndex {
public:
    // Validation state
    uint32_t nStatus;                   // BlockStatus flags (VALID_TREE, FAILED_VALID, etc.)
    
    // Pointers (non-owning)
    const uint256* phashBlock;          // Points to map key (owned by BlockManager)
    CBlockIndex* pprev;                 // Parent block (owned by BlockManager)
    
    // Chain metadata (IMMUTABLE after creation - used for sorting!)
    int nHeight;                        // Height in chain (genesis = 0)
    arith_uint256 nChainWork;           // Cumulative PoW work (2^256 / target)
    
    // Header fields (stored inline)
    int32_t nVersion;
    uint160 minerAddress;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;
    uint256 hashRandomX;
    
    // Copy/move DELETED (prevents dangling phashBlock pointers)
    CBlockIndex(const CBlockIndex&) = delete;
    CBlockIndex& operator=(const CBlockIndex&) = delete;
};
```

**Memory Layout:**
- **Size:** ~120 bytes per block (header + metadata)
- **Ownership:** All instances owned by `BlockManager::m_block_index` (`std::map<uint256, CBlockIndex>`)
- **Lifetime:** Exists from creation until node shutdown (never deleted except on reindex)

**Critical Invariant:** `nHeight` and `nChainWork` are **IMMUTABLE** after block is added to candidate set. Modifying these breaks `ChainSelector`'s `std::set` ordering (undefined behavior).

**BlockStatus Flags:**
```cpp
enum BlockStatus : uint32_t {
    BLOCK_VALID_UNKNOWN = 0,    // Not yet validated
    BLOCK_VALID_HEADER  = 1,    // PoW valid, but not contextual
    BLOCK_VALID_TREE    = 2,    // Fully validated (highest level for headers-only)
    
    BLOCK_FAILED_VALID  = 32,   // Failed validation
    BLOCK_FAILED_CHILD  = 64,   // Descendant of failed block
};
```

**Status Transitions:**
```
UNKNOWN → HEADER → TREE       (validation succeeds)
   ↓         ↓        ↓
FAILED_VALID (or) FAILED_CHILD (validation fails)
```

---

### 2.3 CChain - Linear Chain View

Represents a **single linear chain** from genesis to tip as a vector:

```cpp
class CChain {
private:
    std::vector<CBlockIndex*> vChain;  // vChain[height] -> CBlockIndex*
    
public:
    CBlockIndex* Tip() const;           // Current tip (highest block)
    CBlockIndex* operator[](int height) const;  // O(1) lookup by height
    bool Contains(const CBlockIndex* pindex) const;  // O(1) membership test
    void SetTip(CBlockIndex& block);    // Rebuild entire chain by walking pprev
};
```

**Key Operations:**
- **SetTip(block):** Walks `pprev` pointers backward to genesis, rebuilds entire `vChain` vector
- **Contains(pindex):** `return vChain[pindex->nHeight] == pindex` (O(1) check)
- **FindFork(pindex):** Finds last common ancestor between chain and block

**Usage:**
- `BlockManager::m_active_chain` - The **currently active chain** (best known)
- Temporary `CChain` objects for evaluating competing forks during reorgs

---

### 2.4 arith_uint256 - Big Integer Arithmetic

256-bit unsigned integer for difficulty calculations:

```cpp
class arith_uint256 : public base_uint<256> {
    uint32_t pn[8];  // Little-endian array of 32-bit words
    
    // Arithmetic operations
    arith_uint256& operator+=(const arith_uint256& b);
    arith_uint256& operator-=(const arith_uint256& b);
    arith_uint256& operator*=(const arith_uint256& b);
    arith_uint256& operator/=(const arith_uint256& b);
    arith_uint256& operator<<=(unsigned int shift);
    arith_uint256& operator>>=(unsigned int shift);
    
    // Compact difficulty format
    arith_uint256& SetCompact(uint32_t nCompact, bool* pfNegative = nullptr, 
                             bool* pfOverflow = nullptr);
    uint32_t GetCompact(bool fNegative = false) const;
};
```

**Compact Difficulty Format (nBits):**
```
Format: 0xMMEEEEEE
- MM: Exponent (size in bytes - 3)
- EEEEEE: Mantissa (23 bits)

Examples:
- 0x1d00ffff = difficulty 1 (Bitcoin genesis)
- 0x1e0fffff = difficulty 16
```

**Critical Usage:**
- **Chain work calculation:** `work = ~target / (target + 1) + 1`
- **ASERT difficulty:** Exponential adjustment uses 512-bit intermediate arithmetic
- **DoS protection:** Compare accumulated work to reject low-work chains

---

## 3. Component Architecture

### 3.1 Component Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                      ChainstateManager                          │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                    Public Interface                       │  │
│  │  • AcceptBlockHeader()      • ActivateBestChain()        │  │
│  │  • ProcessNewBlockHeader()  • InvalidateBlock()          │  │
│  │  • GetTip()                 • IsInitialBlockDownload()   │  │
│  └───────────────────────────────────────────────────────────┘  │
│                              ▼                                  │
│  ┌────────────────────┐  ┌────────────────────┐  ┌──────────┐  │
│  │   BlockManager     │  │  ChainSelector     │  │Orphan Pool│ │
│  │                    │  │                    │  │          │  │
│  │ • Block storage    │  │ • Candidate tips   │  │• DoS     │  │
│  │ • Active chain     │  │ • Best chain       │  │  limits  │  │
│  │ • Persistence      │  │ • Pruning          │  │          │  │
│  └────────────────────┘  └────────────────────┘  └──────────┘  │
│                              ▼                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                   Validation Layer                        │  │
│  │  ┌─────────────┐  ┌─────────────┐  ┌──────────────────┐  │  │
│  │  │ CheckBlock  │  │Contextual   │  │   GetNextWork    │  │  │
│  │  │   Header    │  │  Check      │  │   Required       │  │  │
│  │  │             │  │             │  │   (ASERT)        │  │  │
│  │  └─────────────┘  └─────────────┘  └──────────────────┘  │  │
│  └───────────────────────────────────────────────────────────┘  │
│                              ▼                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                 Consensus Layer (PoW)                     │  │
│  │  ┌─────────────────────────────────────────────────────┐  │  │
│  │  │              CheckProofOfWork()                     │  │  │
│  │  │  ┌──────────────────┐  ┌──────────────────────┐    │  │  │
│  │  │  │ COMMITMENT_ONLY  │  │    FULL (RandomX)    │    │  │  │
│  │  │  │  (~1ms, DoS)     │  │    (~50ms, cache)    │    │  │  │
│  │  │  └──────────────────┘  └──────────────────────┘    │  │  │
│  │  └─────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 ChainstateManager - Central Coordinator

**Responsibility:** High-level orchestration of blockchain state

**Key Methods:**

```cpp
class ChainstateManager {
public:
    // === PRIMARY WORKFLOW ===
    
    // 1. Validate & add header to index
    CBlockIndex* AcceptBlockHeader(const CBlockHeader& header, 
                                   ValidationState& state, 
                                   int peer_id = -1);
    
    // 2. Process header (accept + activate best chain)
    bool ProcessNewBlockHeader(const CBlockHeader& header, 
                              ValidationState& state);
    
    // 3. Select and activate chain with most work
    bool ActivateBestChain(CBlockIndex* pindexMostWork = nullptr);
    
    // === QUERY INTERFACE ===
    const CBlockIndex* GetTip() const;
    bool IsInitialBlockDownload() const;
    CBlockIndex* LookupBlockIndex(const uint256& hash);
    
    // === ADMINISTRATIVE ===
    bool InvalidateBlock(const uint256& hash);
    bool Initialize(const CBlockHeader& genesis);
    bool Load(const std::string& filepath);
    bool Save(const std::string& filepath) const;
    
private:
    BlockManager block_manager_;      // Block storage & active chain
    ChainSelector chain_selector_;    // Candidate tip selection
    
    // Orphan tracking
    std::map<uint256, OrphanHeader> m_orphan_headers;
    std::map<int, int> m_peer_orphan_count;
    
    // Failed block tracking
    std::set<CBlockIndex*> m_failed_blocks;
    
    // Thread safety
    mutable std::recursive_mutex validation_mutex_;
};
```

**Thread Safety:** All public methods acquire `validation_mutex_` before accessing shared state.

---

### 3.3 BlockManager - Block Storage

**Responsibility:** Manages all known blocks and the active chain

```cpp
class BlockManager {
public:
    // Block index operations
    CBlockIndex* AddToBlockIndex(const CBlockHeader& header);
    CBlockIndex* LookupBlockIndex(const uint256& hash);
    
    // Active chain management
    CChain& ActiveChain();
    void SetActiveTip(CBlockIndex& block);
    CBlockIndex* GetTip();
    
    // Persistence
    bool Save(const std::string& filepath) const;
    bool Load(const std::string& filepath, const uint256& expected_genesis);
    
private:
    // OWNS all CBlockIndex objects
    std::map<uint256, CBlockIndex> m_block_index;
    
    // Active chain view (non-owning pointers)
    CChain m_active_chain;
};
```

**Key Implementation Details:**

1. **Block Index Storage:**
   ```cpp
   std::map<uint256, CBlockIndex> m_block_index;
   // Key: Block hash
   // Value: CBlockIndex (constructed in-place via try_emplace)
   // phashBlock points to map's key (&iter->first)
   ```

2. **AddToBlockIndex() Workflow:**
   ```cpp
   CBlockIndex* BlockManager::AddToBlockIndex(const CBlockHeader& header) {
       uint256 hash = header.GetHash();
       
       // Try insert (no-op if exists)
       auto [iter, inserted] = m_block_index.try_emplace(hash, header);
       CBlockIndex* pindex = &iter->second;
       
       // Set pointers
       pindex->phashBlock = &iter->first;  // Points to map key
       pindex->pprev = LookupBlockIndex(header.hashPrevBlock);
       
       // CRITICAL: Calculate immutable fields ONCE
       if (pindex->pprev) {
           pindex->nHeight = pindex->pprev->nHeight + 1;
           pindex->nChainWork = pindex->pprev->nChainWork + GetBlockProof(*pindex);
       } else {
           pindex->nHeight = 0;
           pindex->nChainWork = GetBlockProof(*pindex);
       }
       
       return pindex;
   }
   ```

3. **SetActiveTip() Rebuild:**
   ```cpp
   void CChain::SetTip(CBlockIndex& block) {
       vChain.clear();
       vChain.reserve(block.nHeight + 1);
       
       // Walk backwards to genesis
       CBlockIndex* pindex = &block;
       while (pindex) {
           vChain.push_back(pindex);
           pindex = pindex->pprev;
       }
       
       std::reverse(vChain.begin(), vChain.end());
   }
   ```

**Persistence Format (JSON):**
```json
{
  "version": 1,
  "genesis_hash": "000000...",
  "tip_hash": "000001...",
  "block_count": 1234,
  "blocks": [
    {
      "hash": "000000...",
      "height": 0,
      "chainwork": "0x100000...",
      "version": 1,
      "prev_hash": "000000...",
      "miner_address": "00112233...",
      "time": 1234567890,
      "bits": 0x1d00ffff,
      "nonce": 42,
      "hash_randomx": "abcdef...",
      "status": 2
    }
  ]
}
```

---

### 3.4 ChainSelector - Best Chain Selection

**Responsibility:** Maintains candidate tips and selects chain with most work

```cpp
class ChainSelector {
public:
    // Find block with most accumulated work
    CBlockIndex* FindMostWorkChain();
    
    // Add block as candidate tip (if valid leaf node)
    void TryAddBlockIndexCandidate(CBlockIndex* pindex, 
                                   const BlockManager& block_manager);
    
    // Remove stale candidates (less work than active tip)
    void PruneBlockIndexCandidates(const BlockManager& block_manager);
    
    // Best header tracking
    void UpdateBestHeader(CBlockIndex* pindex);
    CBlockIndex* GetBestHeader() const;
    
private:
    // Sorted by descending chain work
    std::set<CBlockIndex*, CBlockIndexWorkComparator> m_candidates;
    
    // Highest work header seen (may not be on active chain)
    CBlockIndex* m_best_header;
};
```

**CBlockIndexWorkComparator:**
```cpp
struct CBlockIndexWorkComparator {
    bool operator()(const CBlockIndex* pa, const CBlockIndex* pb) const {
        // Primary: Higher chain work wins
        if (pa->nChainWork != pb->nChainWork)
            return pa->nChainWork > pb->nChainWork;
        
        // Secondary: Taller chain wins (if same work)
        if (pa->nHeight != pb->nHeight)
            return pa->nHeight > pb->nHeight;
        
        // Tertiary: Lexicographic hash (deterministic tie-breaker)
        return pa->GetBlockHash() < pb->GetBlockHash();
    }
};
```

**Candidate Lifecycle:**

```
Block arrives → AcceptBlockHeader() → TryAddBlockIndexCandidate()
                                              ↓
                                    Is this a leaf node?
                                    (no children exist)
                                              ↓ yes
                                    Add to m_candidates
                                    Remove parent if was candidate
                                              ↓
                                    ActivateBestChain() → FindMostWorkChain()
                                              ↓
                                    Chain switched → PruneBlockIndexCandidates()
                                              ↓
                                    Remove: active tip, ancestors, low-work blocks
```

**Leaf Node Check:**
```cpp
// A block is a candidate only if it has NO children
bool has_children = false;
for (const auto& [hash, block_index] : block_manager.GetBlockIndex()) {
    if (block_index.pprev == pindex) {
        has_children = true;
        break;
    }
}
if (has_children) return;  // Not a candidate
```

---

## 4. Validation Pipeline

### 4.1 Three-Layer Validation

```
┌─────────────────────────────────────────────────────────────┐
│                    LAYER 1: Pre-Filtering                    │
│               (Fast DoS Protection - ~1ms)                   │
├─────────────────────────────────────────────────────────────┤
│  CheckProofOfWork(COMMITMENT_ONLY)                          │
│  • Validates: hashRandomX commitment meets nBits            │
│  • Does NOT: Compute full RandomX hash                      │
│  • Purpose: Reject invalid headers before expensive PoW     │
│  • Formula: SHA256(header || hashRandomX) < nBits           │
└─────────────────────────────────────────────────────────────┘
                            ↓ PASSED
┌─────────────────────────────────────────────────────────────┐
│              LAYER 2: Context-Free Validation                │
│              (Full PoW Verification - ~50ms)                 │
├─────────────────────────────────────────────────────────────┤
│  CheckBlockHeader()                                          │
│  • Validates: Full RandomX hash matches hashRandomX         │
│  • Does NOT: Check if nBits is correct for chain            │
│  • Purpose: Cryptographic PoW verification                  │
│  • Cached: Result stored in CBlockIndex.nStatus             │
└─────────────────────────────────────────────────────────────┘
                            ↓ PASSED
┌─────────────────────────────────────────────────────────────┐
│              LAYER 3: Contextual Validation                  │
│              (Consensus Rules - ~5ms)                        │
├─────────────────────────────────────────────────────────────┤
│  ContextualCheckBlockHeader()                                │
│  • Validates:                                                │
│    - nBits matches ASERT difficulty                         │
│    - Timestamp > Median Time Past (MTP)                     │
│    - Timestamp < now + 2 hours                              │
│    - Version >= 1                                           │
│  • Purpose: Enforce chain consensus rules                   │
│  • Security: Prevents difficulty manipulation attacks       │
└─────────────────────────────────────────────────────────────┘
                            ↓ PASSED
                    ✓ Block Accepted
```

### 4.2 AcceptBlockHeader() Workflow

```cpp
CBlockIndex* ChainstateManager::AcceptBlockHeader(
    const CBlockHeader& header, 
    ValidationState& state, 
    int peer_id) {
    
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
    
    uint256 hash = header.GetHash();
    
    // ──────────────────────────────────────────────
    // STEP 1: Duplicate Check
    // ──────────────────────────────────────────────
    CBlockIndex* pindex = block_manager_.LookupBlockIndex(hash);
    if (pindex) {
        if (pindex->nStatus & BLOCK_FAILED_MASK) {
            return nullptr;  // Known invalid, silently reject
        }
        return pindex;  // Already have it
    }
    
    // ──────────────────────────────────────────────
    // STEP 2: Fast PoW Commitment Check (DoS)
    // ──────────────────────────────────────────────
    if (!CheckProofOfWork(header, POWVerifyMode::COMMITMENT_ONLY)) {
        state.Invalid("high-hash", "commitment failed");
        return nullptr;
    }
    
    // ──────────────────────────────────────────────
    // STEP 3: Genesis Check
    // ──────────────────────────────────────────────
    if (header.hashPrevBlock.IsNull()) {
        if (hash != params_.GetConsensus().hashGenesisBlock) {
            state.Invalid("bad-genesis", "genesis hash mismatch");
            return nullptr;
        }
        state.Invalid("genesis-via-accept", 
                     "genesis must be added via Initialize()");
        return nullptr;
    }
    
    // ──────────────────────────────────────────────
    // STEP 4: Parent Existence Check
    // ──────────────────────────────────────────────
    CBlockIndex* pindexPrev = block_manager_.LookupBlockIndex(
        header.hashPrevBlock);
    if (!pindexPrev) {
        // ORPHAN: Parent not found, cache for later
        if (TryAddOrphanHeader(header, peer_id)) {
            state.Invalid("orphaned", "parent not found");
        } else {
            state.Invalid("orphan-limit", "orphan pool full");
        }
        return nullptr;
    }
    
    // ──────────────────────────────────────────────
    // STEP 5: Parent Validity Check
    // ──────────────────────────────────────────────
    if (pindexPrev->nStatus & BLOCK_FAILED_MASK) {
        state.Invalid("bad-prevblk", "parent is invalid");
        return nullptr;
    }
    
    // ──────────────────────────────────────────────
    // STEP 6: Failed Ancestor Check
    // ──────────────────────────────────────────────
    if (!pindexPrev->IsValid(BLOCK_VALID_TREE)) {
        for (CBlockIndex* failedit : m_failed_blocks) {
            if (pindexPrev->GetAncestor(failedit->nHeight) == failedit) {
                // Mark chain as BLOCK_FAILED_CHILD
                CBlockIndex* invalid_walk = pindexPrev;
                while (invalid_walk != failedit) {
                    invalid_walk->nStatus |= BLOCK_FAILED_CHILD;
                    invalid_walk = invalid_walk->pprev;
                }
                state.Invalid("bad-prevblk", "descends from invalid");
                return nullptr;
            }
        }
    }
    
    // ──────────────────────────────────────────────
    // STEP 7: Add to Index (BEFORE expensive validation)
    // ──────────────────────────────────────────────
    pindex = block_manager_.AddToBlockIndex(header);
    if (!pindex) {
        state.Error("failed to add to index");
        return nullptr;
    }
    
    // ──────────────────────────────────────────────
    // STEP 8: Contextual Validation
    // ──────────────────────────────────────────────
    int64_t adjusted_time = GetAdjustedTime();
    if (!ContextualCheckBlockHeaderWrapper(header, pindexPrev, 
                                           adjusted_time, state)) {
        pindex->nStatus |= BLOCK_FAILED_VALID;
        m_failed_blocks.insert(pindex);
        return nullptr;
    }
    
    // ──────────────────────────────────────────────
    // STEP 9: Full PoW Verification (EXPENSIVE)
    // ──────────────────────────────────────────────
    if (!CheckBlockHeaderWrapper(header, state)) {
        pindex->nStatus |= BLOCK_FAILED_VALID;
        m_failed_blocks.insert(pindex);
        return nullptr;
    }
    
    // ──────────────────────────────────────────────
    // STEP 10: Mark Valid & Update Best Header
    // ──────────────────────────────────────────────
    pindex->RaiseValidity(BLOCK_VALID_TREE);
    chain_selector_.UpdateBestHeader(pindex);
    
    // ──────────────────────────────────────────────
    // STEP 11: Process Orphan Children
    // ──────────────────────────────────────────────
    ProcessOrphanHeaders(hash);
    
    return pindex;
}
```

**Performance Characteristics:**
- **Duplicate:** O(log n) map lookup
- **PoW Commitment:** ~1ms (SHA256 only)
- **Full PoW:** ~50ms (RandomX + cache lookup)
- **Contextual:** ~5ms (ASERT calculation + MTP)

---

### 4.3 ActivateBestChain() - Chain Switching

```cpp
bool ChainstateManager::ActivateBestChain(CBlockIndex* pindexMostWork) {
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
    
    // ──────────────────────────────────────────────
    // STEP 1: Find Chain with Most Work
    // ──────────────────────────────────────────────
    if (!pindexMostWork) {
        pindexMostWork = chain_selector_.FindMostWorkChain();
    }
    if (!pindexMostWork) {
        return true;  // No competing chains
    }
    
    CBlockIndex* pindexOldTip = block_manager_.GetTip();
    
    // ──────────────────────────────────────────────
    // STEP 2: Check if Already Best
    // ──────────────────────────────────────────────
    if (pindexOldTip == pindexMostWork) {
        return true;  // Already at best tip
    }
    
    // ──────────────────────────────────────────────
    // STEP 3: Verify More Work
    // ──────────────────────────────────────────────
    if (pindexOldTip && 
        pindexMostWork->nChainWork <= pindexOldTip->nChainWork) {
        return true;  // Not enough work to switch
    }
    
    // ──────────────────────────────────────────────
    // STEP 4: Find Fork Point
    // ──────────────────────────────────────────────
    const CBlockIndex* pindexFork = 
        LastCommonAncestor(pindexOldTip, pindexMostWork);
    
    if (!pindexFork) {
        return false;  // No common ancestor (should never happen)
    }
    
    // ──────────────────────────────────────────────
    // STEP 5: Deep Reorg Protection
    // ──────────────────────────────────────────────
    int reorg_depth = pindexOldTip->nHeight - pindexFork->nHeight;
    
    if (suspicious_reorg_depth_ > 0 && 
        reorg_depth >= suspicious_reorg_depth_) {
        LOG_ERROR("CRITICAL: Suspicious reorg of {} blocks detected!", 
                 reorg_depth);
        Notifications().NotifySuspiciousReorg(reorg_depth, 
                                              suspicious_reorg_depth_ - 1);
        return false;  // HALT: refuse to reorganize
    }
    
    // ──────────────────────────────────────────────
    // STEP 6: Disconnect Old Chain
    // ──────────────────────────────────────────────
    std::vector<CBlockIndex*> disconnected_blocks;
    CBlockIndex* pindexWalk = pindexOldTip;
    
    while (pindexWalk && pindexWalk != pindexFork) {
        disconnected_blocks.push_back(pindexWalk);
        if (!DisconnectTip()) {
            LOG_ERROR("Failed to disconnect during reorg");
            return false;
        }
        pindexWalk = block_manager_.GetTip();
    }
    
    // ──────────────────────────────────────────────
    // STEP 7: Connect New Chain
    // ──────────────────────────────────────────────
    std::vector<CBlockIndex*> connect_blocks;
    pindexWalk = pindexMostWork;
    
    while (pindexWalk && pindexWalk != pindexFork) {
        connect_blocks.push_back(pindexWalk);
        pindexWalk = pindexWalk->pprev;
    }
    
    // Connect in reverse order (fork → tip)
    for (auto it = connect_blocks.rbegin(); it != connect_blocks.rend(); ++it) {
        if (!ConnectTip(*it)) {
            LOG_ERROR("Failed to connect during reorg at height {}", 
                     (*it)->nHeight);
            
            // ROLLBACK: Restore old chain
            while (block_manager_.GetTip() != pindexFork) {
                DisconnectTip();
            }
            for (auto rit = disconnected_blocks.rbegin(); 
                 rit != disconnected_blocks.rend(); ++rit) {
                ConnectTip(*rit);
            }
            return false;
        }
    }
    
    // ──────────────────────────────────────────────
    // STEP 8: Emit Notifications
    // ──────────────────────────────────────────────
    if (!disconnected_blocks.empty()) {
        LOG_WARN("REORGANIZE: Disconnect {} blocks, Connect {} blocks",
                disconnected_blocks.size(), connect_blocks.size());
    }
    
    Notifications().NotifyChainTip(pindexMostWork, pindexMostWork->nHeight);
    
    // ──────────────────────────────────────────────
    // STEP 9: Prune Stale Candidates
    // ──────────────────────────────────────────────
    chain_selector_.PruneBlockIndexCandidates(block_manager_);
    
    return true;
}
```

**Reorganization Example:**

```
Before Reorg:
    Genesis → A1 → A2 → A3 → A4* (active tip)
                 ↘ B2 → B3 → B4 → B5 (candidate, more work)
    
Fork Point: A1
Disconnect: [A4, A3, A2]
Connect:    [B2, B3, B4, B5]

After Reorg:
    Genesis → A1 → B2 → B3 → B4 → B5* (active tip)
                 ↘ A2 → A3 → A4 (orphaned fork)
```

---

## 5. Concurrency Model

### 5.1 Threading Architecture

```
┌────────────────────────────────────────────────────────────┐
│                         Main Thread                         │
│  • Application initialization                               │
│  • RPC server (Unix socket)                                 │
│  • Periodic tasks (orphan eviction, peer discovery)         │
└────────────────────────────────────────────────────────────┘
                             │
        ┌────────────────────┼────────────────────┐
        ▼                    ▼                    ▼
┌───────────────┐  ┌───────────────┐  ┌───────────────┐
│  Network I/O  │  │  Validation   │  │    Mining     │
│  Thread Pool  │  │    Thread     │  │  Thread (opt) │
│  (4 threads)  │  │               │  │               │
└───────────────┘  └───────────────┘  └───────────────┘
        │                    │                    │
        │                    ▼                    │
        │          ┌───────────────────┐          │
        │          │ ChainstateManager │          │
        │          │ validation_mutex_ │          │
        │          └───────────────────┘          │
        │                    │                    │
        └────────────────────┴────────────────────┘
```

### 5.2 Lock Hierarchy

**Single Coarse-Grained Lock:**
```cpp
class ChainstateManager {
    mutable std::recursive_mutex validation_mutex_;
    
    // All mutations to shared state protected by this lock:
    // - block_manager_
    // - chain_selector_
    // - m_orphan_headers
    // - m_failed_blocks
};
```

**Why Recursive?**
- `AcceptBlockHeader()` calls `ProcessOrphanHeaders()` which recursively calls `AcceptBlockHeader()`
- `ActivateBestChain()` calls `ConnectTip()` / `DisconnectTip()` which may call back into manager
- Alternative would require complex lock/unlock patterns

**Lock-Free Fields:**
```cpp
std::atomic<bool> m_cached_finished_ibd;  // Latch: false → true (never resets)
const ChainParams& params_;               // Immutable reference
const int suspicious_reorg_depth_;        // Immutable config
```

### 5.3 Thread Safety Guarantees

**Thread-Safe Operations:**
- ✅ `GetTip()` - Acquires lock, returns pointer (safe as long as no reorg during use)
- ✅ `LookupBlockIndex()` - Acquires lock, returns pointer
- ✅ `IsInitialBlockDownload()` - Atomic flag (fast path) + locked check (slow path)
- ✅ `AcceptBlockHeader()` - Full operation atomic under lock
- ✅ `ActivateBestChain()` - Entire reorg atomic under lock

**NOT Thread-Safe (caller must hold lock):**
- ❌ `BlockManager` methods (all assume lock held)
- ❌ `ChainSelector` methods (all assume lock held)
- ❌ `ProcessOrphanHeaders()` (assumes lock held)

**Pointer Lifetime Guarantees:**
```cpp
// SAFE: CBlockIndex* valid until node shutdown
CBlockIndex* pindex = chainstate.LookupBlockIndex(hash);
if (pindex) {
    // pindex remains valid even if lock released
    // (blocks never deleted from m_block_index)
    DoSomethingWith(pindex);
}

// UNSAFE: GetTip() result invalidated by reorg
const CBlockIndex* tip = chainstate.GetTip();
// Lock released here...
// Another thread calls ActivateBestChain()
// tip may no longer be the tip!
if (tip) {
    // RACE: tip->nHeight may differ from actual tip height
}
```

---

## 6. Consensus Mechanisms

### 6.1 RandomX Proof-of-Work

**Algorithm:** RandomX (Monero's ASIC-resistant PoW)

**Key Properties:**
- **Memory-hard:** Requires ~2GB dataset (prevents ASICs)
- **CPU-optimized:** JIT compilation for fast hashing on general CPUs
- **Epoch-based:** Dataset changes every N blocks (reduces dataset overhead)

**Verification Modes:**

```cpp
enum class POWVerifyMode {
    COMMITMENT_ONLY,  // SHA256(header || hashRandomX) < nBits (~1ms)
    FULL,             // RandomX(header) == hashRandomX (~50ms)
    MINING            // RandomX(header) → hashRandomX (for miners)
};
```

**Commitment Formula:**
```
commitment = SHA256(SHA256(header_without_randomx || hashRandomX))

Where:
- header_without_randomx: First 68 bytes (nVersion through nNonce)
- hashRandomX: 32 bytes from header.hashRandomX
- Result must be < nBits target
```

**Epoch System:**
```cpp
// Epoch calculation
uint32_t epoch = nTime / nRandomXEpochDuration;

// Seed hash (matches Unicity Alpha network)
uint256 seed = SHA256d("Alpha/RandomX/Epoch/" + std::to_string(epoch));

// Dataset initialization (2GB, cached per epoch)
randomx_cache* cache = randomx_alloc_cache(flags);
randomx_init_cache(cache, seed.data(), 32);
```

**Thread-Local VM Cache:**
```cpp
// Each thread gets its own VM (no locking required)
thread_local std::map<uint32_t, std::shared_ptr<RandomXVMWrapper>> t_vm_cache;

std::shared_ptr<RandomXVMWrapper> GetCachedVM(uint32_t epoch) {
    auto it = t_vm_cache.find(epoch);
    if (it != t_vm_cache.end()) {
        return it->second;  // Cache hit
    }
    
    // Cache miss: Create new VM for this epoch
    uint256 seed = GetSeedHash(epoch);
    randomx_cache* cache = randomx_alloc_cache(flags);
    randomx_init_cache(cache, seed.data(), 32);
    
    randomx_vm* vm = randomx_create_vm(flags, cache, nullptr);
    auto wrapper = std::make_shared<RandomXVMWrapper>(vm, cache);
    
    t_vm_cache[epoch] = wrapper;
    return wrapper;
}
```

---

### 6.2 ASERT Difficulty Adjustment

**Algorithm:** aserti3-2d (Bitcoin Cash's per-block adjustment)

**Formula:**
```
target_new = target_ref * 2^((time_diff - ideal_time) / half_life)

Where:
- target_ref: Anchor block difficulty target
- time_diff: Actual time since anchor parent
- ideal_time: Expected time = (height_diff + 1) * target_spacing
- half_life: Time for difficulty to double/halve (48 hours default)
```

**Implementation:**

```cpp
arith_uint256 CalculateASERT(
    const arith_uint256& refTarget,    // Anchor target
    int64_t nPowTargetSpacing,         // 3600s (1 hour)
    int64_t nTimeDiff,                 // Actual time since anchor
    int64_t nHeightDiff,               // Blocks since anchor
    const arith_uint256& powLimit,     // Max difficulty (easiest)
    int64_t nHalfLife) {               // 172800s (48 hours)
    
    // Exponent calculation (16-bit fixed-point)
    int64_t exponent = ((nTimeDiff - nPowTargetSpacing * (nHeightDiff + 1)) 
                       * 65536) / nHalfLife;
    
    // Split into integer and fractional parts
    int64_t shifts = exponent >> 16;
    uint16_t frac = uint16_t(exponent);
    
    // Polynomial approximation of 2^frac (0 <= frac < 1)
    // 2^x ≈ 1 + 0.695502049*x + 0.2262698*x² + 0.0782318*x³
    uint32_t factor = 65536 + 
        ((195766423245049ull * frac + 
          971821376ull * frac * frac + 
          5127ull * frac * frac * frac + 
          (1ull << 47)) >> 48);
    
    // Use 512-bit arithmetic to prevent overflow
    arith_uint512 nextTarget512(refTarget);
    nextTarget512 *= factor;
    
    // Apply integer part: multiply by 2^(shifts - 16)
    shifts -= 16;
    if (shifts <= 0) {
        nextTarget512 >>= -shifts;
    } else {
        nextTarget512 <<= shifts;
    }
    
    // Clamp to powLimit
    if (nextTarget512 > arith_uint512(powLimit)) {
        nextTarget512 = arith_uint512(powLimit);
    }
    
    // Convert back to 256-bit
    arith_uint256 nextTarget = ArithU512ToU256(nextTarget512);
    
    // Ensure target >= 1
    if (nextTarget == 0) {
        nextTarget = arith_uint256(1);
    }
    
    return nextTarget;
}
```

**Anchor Block System:**
```
Genesis                                  Current
   0 ─────────────► 144 ──────────────► 5000
                  (anchor)          (calculate from anchor)

• Anchor at height 144 (first after bootstrap)
• All difficulty calculations reference anchor
• Anchor difficulty = powLimit (easiest) for first 144 blocks
```

**Example Calculation:**
```
Scenario: Mining 1 block per hour (target), but actual is 45 minutes

Inputs:
- refTarget: 0x1d00ffff (anchor difficulty)
- nPowTargetSpacing: 3600s
- nTimeDiff: 2700s (45 min actual)
- nHeightDiff: 1
- nHalfLife: 172800s

Calculation:
ideal_time = 3600 * (1 + 1) = 7200s
time_delta = 2700 - 7200 = -4500s (blocks arriving early)
exponent = (-4500 * 65536) / 172800 = -17067 (negative = difficulty increases)

Result: Difficulty increases by ~1.7% (blocks too fast)
```

---

### 6.3 Median Time Past (MTP)

**Purpose:** Prevent timestamp manipulation by requiring block time > median of last 11 blocks

**Calculation:**
```cpp
int64_t CBlockIndex::GetMedianTimePast() const {
    int64_t pmedian[MEDIAN_TIME_SPAN];  // 11 slots
    int64_t* pbegin = &pmedian[MEDIAN_TIME_SPAN];
    int64_t* pend = &pmedian[MEDIAN_TIME_SPAN];
    
    // Collect last 11 block times (or fewer near genesis)
    const CBlockIndex* pindex = this;
    for (int i = 0; i < MEDIAN_TIME_SPAN && pindex; i++) {
        *(--pbegin) = pindex->GetBlockTime();
        pindex = pindex->pprev;
    }
    
    // Sort and return median
    std::sort(pbegin, pend);
    return pbegin[(pend - pbegin) / 2];
}
```

**Validation Rule:**
```cpp
// New block time must be strictly greater than MTP
if (header.nTime <= median_time_past) {
    return state.Invalid("time-too-old", "timestamp <= MTP");
}

// New block time must not be too far in future
if (header.nTime > adjusted_time + MAX_FUTURE_BLOCK_TIME) {
    return state.Invalid("time-too-new", "timestamp too far ahead");
}
```

**Example:**
```
Last 11 blocks: [1000, 1005, 1010, 1008, 1020, 1015, 1025, 1030, 1018, 1035, 1040]
Sorted:         [1000, 1005, 1008, 1010, 1015, 1018, 1020, 1025, 1030, 1035, 1040]
                                            ↑
                                         median
MTP = 1018

New block must have: nTime > 1018 (e.g., 1019 minimum)
```

---

## 7. Memory Management

### 7.1 Ownership Model

```
ChainstateManager
    │
    ├── BlockManager (owns)
    │      └── m_block_index: std::map<uint256, CBlockIndex>
    │             └── [OWNS all CBlockIndex instances]
    │
    ├── ChainSelector (owns)
    │      └── m_candidates: std::set<CBlockIndex*>
    │             └── [NON-OWNING pointers to BlockManager's CBlockIndex]
    │
    ├── m_active_chain: CChain
    │      └── vChain: std::vector<CBlockIndex*>
    │             └── [NON-OWNING pointers to BlockManager's CBlockIndex]
    │
    └── m_orphan_headers: std::map<uint256, OrphanHeader>
           └── [OWNS OrphanHeader structs with CBlockHeader copies]
```

**Key Rules:**
1. **BlockManager** is the **sole owner** of all `CBlockIndex` objects
2. All other components hold **non-owning pointers** to `BlockManager`'s blocks
3. **Copy/move deleted** for `CBlockIndex` to prevent pointer invalidation
4. `CBlockIndex` lifetime = node lifetime (never deleted except on reindex)

### 7.2 Memory Footprint

**Per Block:**
```
CBlockHeader:    100 bytes (wire format)
CBlockIndex:     ~120 bytes (header + metadata)
Map overhead:    ~24 bytes (std::map node)
────────────────────────────
Total:           ~244 bytes per block
```

**Total Chain Size:**
```
Blocks           Memory Usage
──────           ────────────
1,000            ~244 KB
10,000           ~2.4 MB
100,000          ~24 MB
1,000,000        ~244 MB
10,000,000       ~2.4 GB
```

**Orphan Pool:**
```
Max orphans:     1,000
Per orphan:      ~132 bytes (header + metadata)
────────────────────────────
Total:           ~132 KB max
```

**Candidate Set:**
```
Typical size:    1-10 candidates (competing forks)
Max size:        ~100 (many short forks)
Per candidate:   8 bytes (pointer)
────────────────────────────
Total:           ~800 bytes typical, ~800 bytes max
```

### 7.3 Disk Persistence

**Format:** JSON (for human readability, can migrate to binary later)

**File Size:**
```
Blocks           Disk Size (JSON)
──────           ────────────────
1,000            ~500 KB
10,000           ~5 MB
100,000          ~50 MB
1,000,000        ~500 MB
```

**Load Performance:**
```
Blocks           Load Time
──────           ─────────
10,000           ~100 ms
100,000          ~1 second
1,000,000        ~10 seconds
```

---

## 8. Error Handling

### 8.1 ValidationState Pattern

```cpp
class ValidationState {
    enum class Result { VALID, INVALID, ERROR };
    
    Result result_;
    std::string reject_reason_;
    std::string debug_message_;
    
public:
    bool Invalid(const std::string& reason, 
                const std::string& debug = "") {
        result_ = Result::INVALID;
        reject_reason_ = reason;
        debug_message_ = debug;
        return false;  // Always returns false for easy error propagation
    }
    
    bool Error(const std::string& reason, 
               const std::string& debug = "") {
        result_ = Result::ERROR;
        reject_reason_ = reason;
        debug_message_ = debug;
        return false;
    }
};
```

**Usage Pattern:**
```cpp
ValidationState state;
if (!CheckBlockHeader(header, params, state)) {
    if (state.IsInvalid()) {
        // Permanent failure: bad block
        LOG_ERROR("Block invalid: {}", state.GetRejectReason());
        // Optionally disconnect peer
    } else if (state.IsError()) {
        // Temporary failure: retry later
        LOG_WARN("Validation error: {}", state.GetRejectReason());
    }
    return false;
}
```

### 8.2 Reject Reasons

**Permanent Failures (INVALID):**
- `high-hash` - PoW hash doesn't meet target
- `bad-diffbits` - Incorrect difficulty for height
- `time-too-old` - Timestamp <= MTP
- `time-too-new` - Timestamp > now + 2 hours
- `bad-version` - Version < 1
- `bad-genesis` - Genesis hash mismatch
- `bad-prevblk` - Parent invalid or doesn't exist
- `network-expired` - Beyond network expiration height

**Temporary Failures (INVALID but retryable):**
- `orphaned` - Parent not found (cached as orphan)
- `orphan-limit` - Orphan pool full

**System Errors (ERROR):**
- `failed to add block to index` - Memory allocation failure
- Database I/O errors

### 8.3 Exception Handling

**Exceptions Thrown:**
```cpp
// RandomX failures
throw std::runtime_error("Failed to allocate RandomX cache");
throw std::runtime_error("Failed to create RandomX VM");

// Arithmetic overflows
throw uint_error("Division by zero");  // In arith_uint256

// File I/O failures
throw std::runtime_error("Failed to open file");
```

**Exception Safety:**
- ✅ **Basic guarantee:** No resource leaks, invariants preserved
- ❌ **Strong guarantee:** NOT provided (state may be partially modified)
- ⚠️ **Critical sections:** Load/Save operations use try-catch with cleanup

---

## 9. Performance Characteristics

### 9.1 Operation Complexity

| Operation | Time Complexity | Notes |
|-----------|----------------|-------|
| **LookupBlockIndex** | O(log n) | `std::map` lookup by hash |
| **AddToBlockIndex** | O(log n) | Map insertion + work calculation |
| **GetTip** | O(1) | Direct pointer access |
| **Contains (CChain)** | O(1) | `vChain[height] == pindex` check |
| **GetAncestor** | O(n) | Walks `pprev` pointers (no skip list) |
| **FindMostWorkChain** | O(log m) | Iterate sorted set (m = candidates) |
| **ActivateBestChain** | O(r) | r = reorg depth (disconnect + connect) |
| **CheckProofOfWork (COMMITMENT)** | ~1ms | SHA256 only |
| **CheckProofOfWork (FULL)** | ~50ms | RandomX + cache lookup |
| **ContextualCheck** | ~5ms | ASERT + MTP calculation |

### 9.2 Validation Throughput

**Single Header:**
```
Fast path (commitment):     1000 headers/sec (~1ms each)
Full validation (RandomX):  20 headers/sec (~50ms each)
```

**Batch Processing (1000 headers):**
```
Commitment check:   ~1 second
Full validation:    ~50 seconds
Ideal: Pipeline with 4 validation threads → ~12 seconds
```

### 9.3 Memory Access Patterns

**Cache-Friendly:**
- ✅ CChain vector (sequential access)
- ✅ Block index iteration (tree traversal)

**Cache-Hostile:**
- ❌ Random hash lookups (map traversal)
- ❌ GetAncestor (pointer chasing)

**Hot Data:**
- Active chain tip (~100 recent blocks): ~24KB
- Candidate set: <1KB
- Orphan pool: ~132KB

---

## 10. Design Patterns

### 10.1 Patterns Used

**Facade Pattern:**
```cpp
// ChainstateManager provides simple interface hiding complexity
ChainstateManager::ProcessNewBlockHeader(header, state);
  └──► AcceptBlockHeader(header, state)
        ├──► CheckProofOfWork()
        ├──► ContextualCheckBlockHeader()
        └──► ProcessOrphanHeaders()
  └──► ActivateBestChain()
        ├──► FindMostWorkChain()
        ├──► DisconnectTip() × N
        └──► ConnectTip() × M
```

**Strategy Pattern:**
```cpp
// POWVerifyMode selects verification strategy
CheckProofOfWork(header, POWVerifyMode::COMMITMENT_ONLY);  // Fast DoS
CheckProofOfWork(header, POWVerifyMode::FULL);             // Full verify
CheckProofOfWork(header, POWVerifyMode::MINING);           // Compute hash
```

**Observer Pattern:**
```cpp
// Notifications for chain state changes
Notifications().NotifyBlockConnected(header, pindex);
Notifications().NotifyBlockDisconnected(header, pindex);
Notifications().NotifyChainTip(pindex, height);
```

**Template Method:**
```cpp
// AcceptBlockHeader defines skeleton, virtual methods for testing
class ChainstateManager {
protected:
    virtual bool CheckProofOfWork(...) const;  // Override in tests
    virtual bool CheckBlockHeaderWrapper(...) const;
};
```

**RAII (Resource Acquisition Is Initialization):**
```cpp
std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
// Lock released automatically on scope exit (even if exception thrown)
```

### 10.2 Anti-Patterns Avoided

❌ **God Object:** Split into BlockManager, ChainSelector, ValidationState  
❌ **Singleton:** ChainstateManager is a regular class (dependency injection)  
❌ **Mutable Shared State:** All mutations serialized by mutex  
❌ **Complex Inheritance:** Prefer composition (e.g., ChainSelector HAS-A set, not IS-A set)

---

## Conclusion

The chain library implements a **clean, well-separated architecture** for headers-only blockchain validation:

**Strengths:**
- ✅ Clear separation of concerns (storage, selection, validation)
- ✅ Thread-safe with simple locking model
- ✅ Comprehensive DoS protection
- ✅ Efficient data structures (O(log n) lookups, O(1) tip access)
- ✅ Bitcoin-compatible patterns (proven in production)

**Areas for Improvement:**
- ⚠️ Single coarse-grained lock (potential bottleneck at high throughput)
- ⚠️ No skip list in GetAncestor (O(n) vs. O(log n))
- ⚠️ JSON persistence (slower than binary format)
- ⚠️ Recursive orphan processing (stack depth risk, see SECURITY_AUDIT.md)

**Related Documents:**
- `ARCHITECTURE.md` - Overall system architecture
- `SECURITY_AUDIT.md` - Security vulnerabilities and fixes
- `PROTOCOL_SPECIFICATION.md` - Wire protocol details
