# CoinbaseChain - Codebase Architecture Overview

**Project:** Headers-Only Blockchain Implementation
**Language:** C++20
**Build System:** CMake 3.20+
**Total Code:** ~10,000 lines of source code
**Status:** Core infrastructure complete with 9+ architectural subsystems

---

## 1. PROJECT OVERVIEW

### Core Mission
CoinbaseChain is a simplified, headers-only blockchain implementation inspired by Bitcoin and Unicity. It synchronizes and validates block headers (100-byte Unicity format) using RandomX Proof-of-Work, without transaction processing.

### Key Design Principles
1. **Headers-only chain** - No transactions, transaction validation, or UTXO set
2. **Modern C++20** - Type-safe, efficient memory management
3. **Modular architecture** - Separated concerns with clear dependencies
4. **Simplified vs Bitcoin** - Intentional omissions for easier maintenance and understanding

### Architecture Comparison to Bitcoin
| Aspect | Bitcoin Core | CoinbaseChain | Difference |
|--------|-------------|---------------|-----------|
| **Block Data** | Full blocks + transactions | Headers only | ~99% size reduction |
| **Consensus** | SHA-256 + ASERT | RandomX + ASERT | Modern PoW algorithm |
| **Main Mutex** | Global `cs_main` | `validation_mutex_` in ChainstateManager | More localized locking |
| **Connection Threads** | 6+ dedicated threads | Boost.Asio thread pool (default 4) | Simpler threading model |
| **Validation Levels** | BLOCK_VALID_HEADER, _TREE, _CHAIN, _SCRIPTS | BLOCK_VALID_HEADER, _TREE only | Simplified state machine |

---

## 2. MAIN SOURCE DIRECTORIES & PURPOSES

### `/include` - Public Interfaces
```
include/
├── app/                    # Application framework and lifecycle
│   └── application.hpp     # Main Application class, config, initialization
├── chain/                  # Blockchain data structures and storage
│   ├── block_index.hpp     # CBlockIndex (metadata per header)
│   ├── block_manager.hpp   # Persistent block storage and lookup
│   ├── chain.hpp           # CChain (active chain as vector)
│   └── chainparams.hpp     # Network parameters (genesis, consensus rules)
├── consensus/              # Consensus rules
│   └── pow.hpp             # ASERT difficulty adjustment
├── crypto/                 # Cryptographic primitives
│   └── randomx_pow.hpp     # RandomX proof-of-work verification
├── mining/                 # Block mining
│   └── miner.hpp           # CPU miner for test blocks
├── network/                # Peer-to-peer networking
│   ├── network_manager.hpp # Top-level network coordinator
│   ├── peer_manager.hpp    # Connection lifecycle management
│   ├── peer.hpp            # Individual peer connections
│   ├── message.hpp         # Wire message types
│   ├── protocol.hpp        # Protocol constants, magic bytes
│   └── addr_manager.hpp    # Peer address discovery/storage
├── primitives/             # Fundamental blockchain structures
│   └── block.h             # CBlockHeader (100-byte header)
├── rpc/                    # JSON-RPC interface
│   ├── rpc_server.hpp      # RPC endpoint server
│   └── rpc_client.hpp      # RPC client for CLI
├── sync/                   # Header synchronization
│   ├── header_sync.hpp     # GETHEADERS/HEADERS protocol
│   ├── peer_manager.hpp    # Peer misbehavior tracking
│   └── banman.hpp          # Ban list management
├── util/                   # Utilities
│   ├── files.hpp           # File I/O, persistence
│   ├── logging.hpp         # Structured logging (spdlog)
│   ├── threadpool.hpp      # Thread pool for parallel work
│   ├── fs_lock.hpp         # Filesystem locking
│   ├── sync.hpp            # Thread synchronization helpers
│   ├── threadsafety.hpp    # Annotations for thread safety
│   ├── time.hpp            # Time utilities
│   └── macros.hpp          # Utility macros
├── validation/             # Block validation
│   ├── validation.hpp      # Validation functions, state machine
│   ├── chainstate_manager.hpp  # High-level coordinator
│   └── chain_selector.hpp  # Best chain selection
├── notifications.hpp       # Event notifications
├── uint.hpp                # 256-bit and 160-bit unsigned integers
└── rpc_client.hpp
```

### `/src` - Implementations
Mirrors `/include` directory structure with `.cpp` files implementing the corresponding headers.

```
src/
├── app/                    # Application initialization
├── chain/                  # Blockchain storage
├── consensus/              # PoW difficulty
├── crypto/                 # Hashing and RandomX
├── mining/                 # Block mining
├── network/                # P2P networking
├── primitives/             # Header serialization
├── rpc/                    # RPC endpoints
├── sync/                   # Header sync protocol
├── util/                   # Utility implementations
├── validation/             # Header validation and chain selection
├── main.cpp                # Entry point
├── cli.cpp                 # CLI client
├── test.cpp                # Test runner
└── notifications.cpp       # Event system
```

### `/test` - Test Suites
```
test/
├── block_tests.cpp         # Header serialization tests
├── block_index_tests.cpp   # CBlockIndex tests
├── chain_tests.cpp         # CChain tests
├── header_sync_tests.cpp   # Header sync logic tests
├── validation_tests.cpp    # Validation pipeline tests
├── threading_tests.cpp     # Thread safety tests
├── addr_manager_tests.cpp  # Address manager tests
├── dos_protection_tests.cpp# DoS attack mitigation tests
├── uint_tests.cpp          # 256/160-bit integer tests
├── files_tests.cpp         # File I/O tests
├── persistence_tests.cpp   # Persistence tests
├── stress_threading_tests.cpp  # Concurrent access tests
├── network/                # Network simulation harness
│   ├── simulated_network.cpp   # Simulated P2P network
│   ├── simulated_node.cpp      # Virtual node for testing
│   ├── network_tests.cpp       # E2E network tests
│   └── mock_chainstate.cpp     # Mock validation layer
└── catch_amalgamated.cpp   # Catch2 test framework
```

### `/tools` - Utility Programs
```
tools/
├── genesis_miner/          # Tool to mine genesis block
└── attack_node/            # Network attack simulation tool
```

---

## 3. CORE BLOCKCHAIN COMPONENTS

### 3.1 Block Structures (`primitives/block.h`)

**CBlockHeader (100 bytes - Unicity Format)**
```
Layout (little-endian scalar fields, no endian-swap for hashes):
├── nVersion (4 bytes)           - Block version
├── hashPrevBlock (32 bytes)      - Previous block hash (SHA256)
├── minerAddress (20 bytes)       - Miner address (replaces merkle root)
├── nTime (4 bytes)               - Unix timestamp
├── nBits (4 bytes)               - Difficulty target (compact format)
├── nNonce (4 bytes)              - Proof-of-work nonce
└── hashRandomX (32 bytes)        - RandomX hash for PoW verification
```

**Key Simplifications from Bitcoin:**
- No transactions → no merkle root
- Direct miner address field instead of address derivation
- RandomX instead of SHA256d PoW
- Fixed 100-byte size (Bitcoin: variable due to transactions)

**Serialization:** 
- Fixed-size binary format (100 bytes exactly)
- No variable-length fields
- Custom serialization/deserialization in `src/primitives/block.cpp`

---

### 3.2 Block Index & Chain Management

**CBlockIndex** (`include/chain/block_index.hpp`)
- Metadata about a single header
- Tracks: hash pointer, parent pointer, height, chain work, validation status
- **Key property:** Copy/move operations DELETED to prevent dangling pointers
- Owned by `BlockManager`'s `std::map<uint256, CBlockIndex>`

**CChain** (`include/chain/chain.hpp`)
- O(1) access to blocks by height: `chain[height]` returns header at height
- Represented as `std::vector<CBlockIndex*>` (doesn't own the CBlockIndex objects)
- Used for the "active chain" (best known chain)

**BlockManager** (`include/chain/block_manager.hpp`)
- Central store for all known headers (including forks, orphans)
- Owns `std::map<uint256, CBlockIndex>` - the map is the single authority
- Provides: lookup by hash, add new header, get active chain, persistence
- **Thread Safety:** No internal mutex - relies on ChainstateManager's `validation_mutex_`

**Block Validation Status**
```cpp
enum BlockStatus {
    BLOCK_VALID_UNKNOWN      = 0    // Not validated
    BLOCK_VALID_HEADER       = 1    // PoW is valid
    BLOCK_VALID_TREE         = 2    // Parent found, timestamps valid (highest for headers-only)
    BLOCK_FAILED_VALID       = 32   // Failed validation
    BLOCK_FAILED_CHILD       = 64   // Descends from invalid block
};
```

---

### 3.3 Validation Pipeline (`include/validation/`)

**Layered Validation Architecture** (3 layers for DoS protection):

1. **Layer 1: Fast Pre-filtering (P2P headers sync)**
   - `CheckHeadersPoW()` - Commitment-only PoW (~50x faster, no RandomX compute)
   - `CheckHeadersAreContinuous()` - Chain structure validation
   - Purpose: Quick rejection of spam during sync

2. **Layer 2: Full Context-Free Validation (before chain acceptance)**
   - `CheckBlockHeader()` - FULL RandomX verification (~1ms per header)
   - Verifies PoW meets the nBits target but NOT that nBits is correct
   - Security: Cryptographically valid PoW

3. **Layer 3: Contextual Validation (requires parent block)**
   - `ContextualCheckBlockHeader()` - CRITICAL FOR SECURITY
   - Validates nBits matches ASERT difficulty expectation
   - Validates timestamps follow chain rules
   - Without this: attackers could mine at arbitrarily low difficulty!

**ChainstateManager** - Orchestrator (`include/validation/chainstate_manager.hpp`)
- Entry point for processing all new headers (from network or mining)
- Owns `BlockManager` and `ChainSelector`
- Provides: `AcceptBlockHeader()`, `ProcessNewBlockHeader()`, `ActivateBestChain()`
- **Thread Safety:** Uses `std::recursive_mutex validation_mutex_`
  - Protects all internal state
  - Allows public methods to call other public methods atomically
  - Fast path `IsInitialBlockDownload()` uses atomic for post-IBD check

**Anti-DoS Design**
```
Cheap check (commitment) → Add to index → Expensive check (full PoW)
                          ↓
                    Cache result
                          ↓
                   Never re-compute for same header
```

---

### 3.4 Consensus Rules (`include/consensus/`)

**GetNextWorkRequired()** - ASERT Difficulty Adjustment
- Per-block exponential adjustment
- Based on Bitcoin Cash aserti3-2d algorithm
- Responsive to hashrate changes
- Maintains ~2-minute block times

**Constants from ChainParams**
- Genesis block hash and header
- Target block time (2 minutes for Unicity)
- ASERT half-life (configurable)
- Minimum chain work (for eclipse attack protection)

---

### 3.5 Mining (`include/mining/`)

**CPUMiner**
- Generates valid block headers with RandomX PoW
- Selects tip of best chain and creates new header
- Submits via `ChainstateManager` validation pipeline
- Used for: genesis block generation, testing, benchmarking

---

## 4. NETWORKING ARCHITECTURE

### 4.1 Network Stack (`include/network/`)

**Protocol Stack Layers** (bottom to top):
1. **TCP Sockets** - Boost.Asio
2. **Peer** - Individual connection state machine
3. **PeerManager** - Connection lifecycle
4. **AddressManager** - Peer discovery
5. **HeaderSync** - Block header synchronization
6. **BanMan** - Misbehavior tracking
7. **NetworkManager** - Orchestrator

### 4.2 Key Components

**Peer** (`include/network/peer.hpp`)
- Wraps single TCP connection to a peer
- **State Machine:** DISCONNECTED → CONNECTING → VERSION_SENT → VERACK_SENT → CONNECTED
- **Async Operations:** Read/write message headers and payloads using Boost.Asio
- **Message Framing:** 24-byte header (magic, command, length, checksum) + payload
- **Keepalive:** PING/PONG every 30 seconds

**PeerManager** (`include/network/peer_manager.hpp`)
- Manages pool of active peer connections
- Enforces connection limits (default: 8 outbound, 125 inbound)
- Tracks peer state and provides queries
- Message routing to handlers

**AddressManager** (`include/network/addr_manager.hpp`)
- Maintains database of known peer addresses
- **Tried table:** Peers we've successfully connected to
- **New table:** Peers we know about but haven't connected to
- **Selection:** Probabilistic, prefers tried addresses
- **Persistence:** Saves/loads to peers.json

**HeaderSync** (`include/sync/header_sync.hpp`)
- Implements P2P header synchronization
- **Protocol:** GETHEADERS (request) ↔ HEADERS (response)
- **Batches:** Up to 2000 headers per message
- **Validation:** Calls ChainstateManager for all validation
- **Thread-safe:** Protects state with mutex

**NetworkManager** (`include/network/network_manager.hpp`)
- Top-level coordinator
- Manages `io_context` thread pool (Boost.Asio)
- Owns `PeerManager`, `AddressManager`, `HeaderSync`, `BanMan`
- Handles: inbound acceptance, outbound connection attempts, message routing
- Periodic tasks: connection attempts, maintenance, peer saving

### 4.3 Protocol Definition (`include/network/protocol.hpp`)

**Message Types**
- Handshake: VERSION, VERACK
- Connectivity: PING, PONG, ADDR, GETADDR
- Inventory: INV, GETDATA
- Block sync: GETHEADERS, HEADERS
- Relay: BLOCK, INV (for blocks)

**Network Magic Bytes**
- MAINNET: 0xD9B4BEF9
- TESTNET: 0x0709110B
- REGTEST: 0xFABFB5DA (easy mining for testing)

---

## 5. SYNCHRONIZATION & CONSENSUS

### 5.1 Header Synchronization (`include/sync/header_sync.hpp`)

**Sync Flow**
1. **Bootstrap:** Genesis block initialized
2. **Request:** Send GETHEADERS with locator (exponentially-spaced hashes)
3. **Receive:** Get up to 2000 HEADERS from peer
4. **Validate:** Each header checked (commitment PoW, continuity, timestamps)
5. **Accept:** Valid headers added to chain
6. **Loop:** If batch full (2000), request more; else mark synced

**DoS Protection in Sync**
- Commitment-only PoW for fast filtering
- Continuous chain check (no gaps)
- Timestamp rules
- Anti-work threshold (reject work much lower than tip)

### 5.2 Chain Selection

**ChainSelector** (`include/validation/chain_selector.hpp`)
- Maintains set of candidate headers that could be chain tips
- Compares competing chains by total work (cumulative `nChainWork`)
- Activates best chain via `ActivateBestChain()`
- Handles: chain reorgs, orphan blocks, competing tips

---

## 6. PERSISTENCE & STORAGE

### 6.1 Block Persistence (`include/chain/block_manager.hpp`)

**Format:** JSON with all headers
**Location:** `<datadir>/blocks/headers.json`
**Triggers:** 
- Periodic save (10 minutes)
- Graceful shutdown
- Explicit save calls

**Load:** Reconstructs entire block index from JSON

### 6.2 Peer Address Persistence

**Format:** JSON with address list
**Location:** `<datadir>/peers.json`
**Triggers:** 
- Periodic save (15 minutes)
- Graceful shutdown

**Ban List Persistence** (`include/sync/banman.hpp`)
**Format:** JSON with ban entries and expiration times
**Location:** `<datadir>/banlist.json`

### 6.3 Data Directory

**Default:** `~/.coinbasechain/`
**Structure:**
```
~/.coinbasechain/
├── blocks/
│   └── headers.json        # All known headers
├── peers.json              # Known peer addresses
├── banlist.json            # Banned peers
├── anchors.json            # Last 2 outbound peers (for eclipse resistance)
└── debug.log               # Application logs
```

---

## 7. APPLICATION FRAMEWORK

### 7.1 Application Lifecycle (`include/app/application.hpp`)

**Class: Application**

**Initialization Steps (in order):**
1. `init_datadir()` - Create/validate data directory
2. `init_randomx()` - Load RandomX dataset
3. `init_chain()` - Load blockchain from disk or create genesis
4. `init_network()` - Start NetworkManager (peer connections)
5. `init_rpc()` - Start JSON-RPC server

**Running State:**
- Signal handlers for SIGINT/SIGTERM
- Component coordination during sync
- Periodic saves (headers, addresses)

**Shutdown:**
- Graceful stop of all components
- Save persistent state
- Cleanup resources

**Configuration** (AppConfig)
```cpp
struct AppConfig {
    filesystem::path datadir;           // Data directory
    NetworkManager::Config network_config;
    ChainType chain_type;               // MAIN, TESTNET, REGTEST
    int par_threads;                    // Parallel RandomX threads
    int suspicious_reorg_depth;         // Reorg depth limit
    bool verbose;                       // Debug logging
};
```

### 7.2 Command-Line Interface

**Executable:** `./build/bin/coinbasechain`

**Usage:**
```bash
coinbasechain [--datadir=PATH] [--port=PORT] [--listen] [--threads=N] 
              [--par=N] [--regtest] [--testnet] [--verbose] [--help]
```

**Common Options:**
- `--listen` - Enable inbound connections (default: outbound-only)
- `--port=PORT` - Listen port (default: 8333 mainnet, 18333 testnet)
- `--regtest` - Easy mining for testing
- `--datadir` - Custom data directory
- `--threads` - Number of IO threads in pool

---

## 8. UTILITY SYSTEMS

### 8.1 Logging (`include/util/logging.hpp`)

**Framework:** spdlog (modern C++ logging)
**Output:** 
- Console (INFO level)
- File: `<datadir>/debug.log` (DEBUG level)
- Structured format with timestamp, level, component

### 8.2 Threading (`include/util/threadpool.hpp`)

**ThreadPool** - Work-stealing thread pool for parallel tasks
- Default: Number of CPU cores
- Used for parallel RandomX verification

**Synchronization** (`include/util/sync.hpp`)
- GUARDED_BY macro for thread safety annotations
- Lock guards and assertions

### 8.3 File I/O (`include/util/files.hpp`)

**Crash-Safe Writes:**
1. Write to temporary file
2. fsync() to ensure durability
3. Atomic rename to final location
4. Prevents corruption on crash

**Operations:**
- `get_default_datadir()` - Platform-specific data directory
- `CreateDirectory()` - Safe directory creation
- Atomic file writes

---

## 9. ARCHITECTURAL PATTERNS & DESIGN DECISIONS

### 9.1 Memory Management

**Ownership Model:**
```
Application
├── BlockManager (owned)
│   └── std::map<uint256, CBlockIndex>  (CBlockIndex owned)
├── NetworkManager (owned)
│   ├── PeerManager
│   │   └── std::map<peer_id, shared_ptr<Peer>>
│   ├── AddressManager
│   └── HeaderSync
└── ChainstateManager (owned)
    └── BlockManager (reference)
```

**Pointer Safety:**
- `CBlockIndex` copy/move DELETED - prevents dangling `phashBlock` pointers
- `Peer` wrapped in `shared_ptr` - handles async lifetime
- Most other objects: stack-allocated or unique_ptr

### 9.2 Thread Safety

**Locking Strategy:**
- `ChainstateManager::validation_mutex_` (recursive_mutex) - protects all chain state
- HeaderSync has own mutex - protects sync state
- BanMan has own mutex - protects ban list
- AddressManager has own mutex - protects peer addresses
- NetworkManager relies on Boost.Asio strand for message ordering

**Locking Order** (prevents deadlock):
```
ChainstateManager::validation_mutex_ 
    ↓
HeaderSync::mutex_
```
Never acquire in reverse order!

### 9.3 Async I/O Model

**Framework:** Boost.Asio (proactor pattern)
**Thread Pool:** Configurable (default 4 threads)
**Advantages:**
- Single io_context coordinates all network I/O
- No thread-per-connection overhead
- Efficient polling of many sockets

**Message Flow:**
```
tcp::socket → Peer::read_header() → validate checksum/length
           → Peer::read_payload() → deserialize to Message
           → Message handler → BlockManager or HeaderSync
           → ChainstateManager::AcceptBlockHeader()
           → Notification to HeaderSync
```

---

## 10. COMPARED TO BITCOIN-LIKE CODEBASES

### Simplifications

| Aspect | Bitcoin | CoinbaseChain | Why Simplified |
|--------|---------|---------------|----------------|
| **Block Data** | Full blocks | Headers only | No transaction validation needed |
| **PoW Algorithm** | SHA256d | RandomX | Specified by Unicity |
| **Sync Strategy** | Headers-first then blocks | Headers-only | Simpler, faster for our use case |
| **Memory Model** | UTXO set | None | Headers-only chain |
| **Transaction Pool** | Full mempool | None | No transactions |
| **Script Validation** | SCRIPT check levels | None | Headers-only |
| **Main Mutex** | Single global `cs_main` | `validation_mutex_` in ChainstateManager | More localized |
| **Threading** | 6+ dedicated threads | Asio thread pool (4) | Event-driven vs thread-per-task |

### Maintained Concepts

| Concept | Implemented | Notes |
|---------|-------------|-------|
| **Block Index Tree** | Yes | Tree structure via pprev pointers |
| **Chain Selection** | Yes | Cumulative work comparison |
| **Reorg Handling** | Yes | Disconnect/connect blocks during chain switch |
| **Anti-DoS** | Yes | Cheap checks before expensive ones |
| **Peer Management** | Yes | Connection limits, tried/new addresses |
| **Checkpoints** | Partial | Can validate against checkpoints |
| **Ban List** | Yes | Persistent ban management |
| **Signal Handling** | Yes | Graceful shutdown on SIGINT/SIGTERM |

---

## 11. KEY ENTRY POINTS & COMPILATION

### Main Entry Point
**File:** `src/main.cpp`
```cpp
int main(int argc, char* argv[]) {
    // 1. Parse CLI arguments
    coinbasechain::app::AppConfig config;
    
    // 2. Initialize logging
    LogManager::Initialize(...);
    
    // 3. Create Application
    Application app(config);
    
    // 4. Initialize all components
    app.initialize();
    app.start();
    
    // 5. Run until shutdown
    app.wait_for_shutdown();
    
    return 0;
}
```

### Build Configuration
**File:** `CMakeLists.txt`

**Library Dependencies:**
- boost (Asio, System)
- randomx (from Unicity fork)
- spdlog (logging)
- nlohmann_json (serialization)
- Catch2 (testing)
- Google Test (network simulation)

**Libraries Built:**
1. `util` - Files, logging, threading
2. `crypto` - SHA256, RandomX
3. `primitives` - Block header
4. `network` - P2P protocol
5. `consensus` - PoW rules
6. `validation` - Header validation
7. `notifications` - Event system
8. `chain` - Blockchain storage
9. `sync` - Header synchronization
10. `mining` - Block mining
11. `rpc` - JSON-RPC interface
12. `app` - Application framework

**Executables:**
- `coinbasechain` - Main node
- `coinbasechain-cli` - RPC client
- `coinbasechain_tests` - Test suite
- `network_tests` - Network simulation tests

---

## 12. CODE STATISTICS

- **Total Lines:** ~9,965 lines of C++ source
- **Test Coverage:** 13+ test files with 50+ test cases
- **Headers:** 33 header files
- **Implementation:** 32 C++ source files
- **Standard:** C++20 (requires GCC 10+, Clang 12+, MSVC 2019+)

---

## 13. TESTING INFRASTRUCTURE

### Unit Tests (Catch2)
- Block serialization/deserialization
- CBlockIndex tree operations
- CChain operations
- Header validation
- Chain selection
- Persistence/loading
- Address manager
- DoS protection
- Threading safety
- Uint256/Uint160 operations

### Integration Tests (Google Test)
- Simulated network with multiple nodes
- Header sync scenarios
- Chain reorg handling
- Ban/disconnect handling

---

## 14. DOCUMENTATION

**Analysis Documents** (in repository root):
- `README.md` - Quick start and overview
- `PROJECT_PLAN.md` - Full development plan
- `NETWORK_MISSING.md` - Features deferred from Bitcoin Core
- `IBD_ANALYSIS.md` - Initial Block Download design
- `LOCKING_ORDER.md` - Thread safety and deadlock prevention
- `SERIALIZATION_SPECIFICATION.md` - Wire format details
- `CONSENSUS_FIXES.md` - Consensus rule implementation notes
- `HASH_ENDIANNESS_FIX.md` - Endianness handling for hashes

---

## 15. QUICK START

```bash
# Build
cmake -B build -S .
cmake --build build

# Run node with listening enabled
./build/bin/coinbasechain --listen --verbose

# Run tests
./build/coinbasechain_tests

# Run CLI client
./build/bin/coinbasechain-cli getblockcount
```

---

## Summary

CoinbaseChain presents a **clean, modular implementation** of a headers-only blockchain that maintains the core architectural patterns of Bitcoin while significantly simplifying the codebase through intentional omissions. The project demonstrates:

1. **Clean separation of concerns** - 9+ independent subsystems with clear interfaces
2. **Production-quality patterns** - Anti-DoS, thread safety, persistence, graceful shutdown
3. **Educational value** - Well-commented code ideal for learning blockchain concepts
4. **Modern C++** - Type-safe, efficient memory management, minimal dynamic allocation
5. **Comprehensive testing** - Unit, integration, and stress tests across all components

The architecture successfully balances simplicity (headers-only, no transactions) with robustness (same consensus rules, validation pipeline, and peer management as Bitcoin).
