# CoinbaseChain - Quick Reference Guide

## Directory Structure

```
coinbasechain-full/
├── include/                      # Public header files (33 files)
│   ├── app/                      # Application framework
│   ├── chain/                    # Blockchain data structures
│   ├── consensus/                # Consensus rules (ASERT)
│   ├── crypto/                   # Cryptographic primitives
│   ├── mining/                   # Block mining
│   ├── network/                  # P2P networking
│   ├── primitives/               # Block header definition
│   ├── rpc/                      # JSON-RPC interface
│   ├── sync/                     # Header synchronization
│   ├── util/                     # Utilities
│   ├── validation/               # Block validation pipeline
│   ├── notifications.hpp         # Event notifications
│   └── uint.hpp                  # 256/160-bit integers
│
├── src/                          # Implementation files (32 files)
│   ├── app/                      # Application setup & lifecycle
│   ├── chain/                    # Block storage & chain management
│   ├── consensus/                # Difficulty adjustment
│   ├── crypto/                   # SHA256, RandomX
│   ├── mining/                   # CPU miner implementation
│   ├── network/                  # P2P protocol implementation
│   ├── primitives/               # Header serialization
│   ├── rpc/                      # RPC endpoint implementation
│   ├── sync/                     # Header sync protocol
│   ├── util/                     # Utility implementations
│   ├── validation/               # Validation functions
│   ├── main.cpp                  # Program entry point
│   ├── cli.cpp                   # CLI client
│   └── notifications.cpp         # Event system
│
├── test/                         # Test files (13+ test suites)
│   ├── block_tests.cpp           # Header serialization
│   ├── block_index_tests.cpp     # CBlockIndex operations
│   ├── chain_tests.cpp           # CChain operations
│   ├── header_sync_tests.cpp     # Header sync logic
│   ├── validation_tests.cpp      # Validation pipeline
│   ├── threading_tests.cpp       # Thread safety
│   ├── addr_manager_tests.cpp    # Address manager
│   ├── dos_protection_tests.cpp  # DoS protection
│   ├── uint_tests.cpp            # Uint256/160 tests
│   ├── files_tests.cpp           # File I/O
│   ├── persistence_tests.cpp     # Save/load
│   ├── stress_threading_tests.cpp# Concurrent access
│   ├── network/                  # Network simulation tests
│   └── catch_amalgamated.cpp     # Catch2 framework
│
├── tools/                        # Utility programs
│   ├── genesis_miner/            # Mine genesis block
│   └── attack_node/              # Network attack simulation
│
├── cmake/                        # CMake build configuration
├── build/                        # Build output directory (after cmake build)
├── CMakeLists.txt                # Build configuration
├── README.md                     # Quick start guide
├── CODEBASE_STRUCTURE.md         # This architecture overview
└── [Other documentation]
```

## Component Dependency Graph

```
app::Application (entry point)
├── network::NetworkManager
│   ├── network::PeerManager
│   │   └── network::Peer (TCP connections)
│   ├── network::AddressManager (peer discovery)
│   ├── sync::HeaderSync
│   │   └── validation::ChainstateManager
│   └── sync::BanMan
├── validation::ChainstateManager
│   ├── chain::BlockManager
│   │   └── chain::CBlockIndex (metadata)
│   ├── validation::ChainSelector
│   └── chain::CChain (active chain)
├── mining::CPUMiner
│   └── chain::ChainParams
├── rpc::RPCServer
└── chain::ChainParams (consensus rules)

crypto::SHA256, crypto::RandomXPoW (used by all)
util::* (threading, files, logging)
```

## Key Classes & Their Purposes

### Blockchain Core
| Class | Location | Purpose |
|-------|----------|---------|
| `CBlockHeader` | `primitives/block.h` | 100-byte header structure |
| `CBlockIndex` | `chain/block_index.hpp` | Metadata about one header |
| `CChain` | `chain/chain.hpp` | Active chain as vector (O(1) height access) |
| `BlockManager` | `chain/block_manager.hpp` | Persistent storage of all headers |
| `ChainstateManager` | `validation/chainstate_manager.hpp` | Entry point for validation |
| `ChainSelector` | `validation/chain_selector.hpp` | Best chain selection |

### Networking
| Class | Location | Purpose |
|-------|----------|---------|
| `Peer` | `network/peer.hpp` | Single TCP connection |
| `PeerManager` | `network/peer_manager.hpp` | Connection lifecycle |
| `AddressManager` | `network/addr_manager.hpp` | Peer discovery (tried/new tables) |
| `HeaderSync` | `sync/header_sync.hpp` | GETHEADERS/HEADERS protocol |
| `NetworkManager` | `network/network_manager.hpp` | Top-level network coordinator |
| `BanMan` | `sync/banman.hpp` | Misbehavior tracking |

### Validation
| Function | Location | Purpose |
|----------|----------|---------|
| `CheckBlockHeader()` | `validation/validation.hpp` | Full RandomX PoW verification |
| `CheckHeadersPoW()` | `validation/validation.hpp` | Fast commitment-only check |
| `ContextualCheckBlockHeader()` | `validation/validation.hpp` | Verify nBits and timestamps |
| `GetNextWorkRequired()` | `consensus/pow.hpp` | ASERT difficulty calculation |

### Utilities
| Class | Location | Purpose |
|-------|----------|---------|
| `ThreadPool` | `util/threadpool.hpp` | Parallel work distribution |
| `LogManager` | `util/logging.hpp` | Structured logging (spdlog) |
| `FileUtilities` | `util/files.hpp` | Atomic file writes |
| `uint256` | `uint.hpp` | 256-bit unsigned integer |
| `uint160` | `uint.hpp` | 160-bit unsigned integer |

## Build & Execution

### Build the project
```bash
cmake -B build -S .
cmake --build build
```

### Run the node
```bash
# Full help
./build/bin/coinbasechain --help

# Basic node (outbound connections only)
./build/bin/coinbasechain

# Node with listening enabled
./build/bin/coinbasechain --listen --port=8333

# Easy mining (regtest)
./build/bin/coinbasechain --regtest

# Verbose logging
./build/bin/coinbasechain --verbose
```

### Run tests
```bash
# All tests
./build/coinbasechain_tests

# Specific test category
./build/coinbasechain_tests "[block]"

# Verbose output
./build/coinbasechain_tests -s

# Network simulation tests
./build/network_tests
```

### Run CLI client
```bash
./build/bin/coinbasechain-cli getblockcount
./build/bin/coinbasechain-cli getblockhash 0
```

## Data Directory Structure

```
~/.coinbasechain/                # Default data directory
├── blocks/
│   └── headers.json             # All known headers (JSON format)
├── peers.json                   # Known peer addresses
├── banlist.json                 # Banned peers (with expiration)
├── anchors.json                 # Last 2 outbound peers
└── debug.log                    # Application logs
```

## Thread Safety

### Locks & Mutexes
- `ChainstateManager::validation_mutex_` - All blockchain state
- `HeaderSync::mutex_` - Header sync state
- `BanMan::mutex_` - Ban list
- `AddressManager::mutex_` - Peer addresses
- `Boost.Asio io_context` - Network I/O ordering

### Locking Order (prevents deadlock)
```
ChainstateManager::validation_mutex_ (ACQUIRED FIRST)
    ↓
HeaderSync::mutex_ (ACQUIRED SECOND)
    ↓
(other component mutexes)
```
**Never acquire in reverse order!**

## Key Validation Flow

```
1. Peer receives HEADERS message
   ↓
2. HeaderSync::ProcessHeaders()
   - Fast check: CheckHeadersPoW() (commitment-only, ~50x faster)
   - Fast check: CheckHeadersAreContinuous() (chain linkage)
   ↓
3. For each valid header:
   - ChainstateManager::AcceptBlockHeader()
   - Add to BlockManager (BEFORE expensive validation)
   ↓
4. Full validation (if not in IBD):
   - CheckBlockHeader() (FULL RandomX verification, ~1ms)
   - ContextualCheckBlockHeader() (CRITICAL - verify nBits)
   ↓
5. Chain selection:
   - ChainSelector::TryAddBlockIndexCandidate()
   - ChainstateManager::ActivateBestChain()
   - Emit notification if tip changed
```

## Anti-DoS Design

**Layer 1: Cheap checks (no caching needed)**
- Commitment-only PoW (50x faster than full verification)
- These reject spam without wasting resources

**Layer 2: Add to index BEFORE expensive checks**
- Once added to index, result is cached
- Never recompute expensive checks for same header

**Layer 3: Expensive validation (result cached)**
- Full RandomX PoW verification (~1ms per header)
- Contextual checks (ASERT difficulty)
- Result cached in CBlockIndex::nStatus

**Result:** Attackers cannot force repeated expensive work

## Performance Characteristics

| Operation | Time |
|-----------|------|
| Header serialization | < 1 µs |
| SHA256 hash | ~ 1 µs |
| RandomX PoW verification | ~ 1 ms (full) |
| RandomX commitment check | ~ 20 µs (commitment-only) |
| ASERT difficulty calculation | < 1 µs |
| CChain height lookup | O(1) |
| CBlockIndex ancestor lookup | O(n) [TODO: add skip list] |
| Block index insertion | O(log n) |
| Chain activation | O(reorg depth) |

## Consensus Rules

- **Block time:** ~2 minutes
- **PoW algorithm:** RandomX (CPU-friendly ASIC-resistant)
- **Difficulty adjustment:** ASERT (per-block exponential)
- **ASERT half-life:** Configurable (typically 13.33 hours = 400 blocks)
- **Max future block time:** 2 hours
- **Median time span:** 11 previous blocks

## Network Protocol

**Message Frame Format**
```
24-byte header:
├── magic (4 bytes)          - Network identifier
├── command (12 bytes)       - Message type (null-padded)
├── length (4 bytes)         - Payload size (little-endian)
└── checksum (4 bytes)       - SHA256d (first 4 bytes)

Payload: Variable
```

**Message Types (Headers-Only)**
- VERSION, VERACK - Handshake
- PING, PONG - Keepalive
- ADDR, GETADDR - Peer discovery
- GETHEADERS, HEADERS - Header sync
- INV, GETDATA - Inventory (optional for block relay)

**Connection Limits**
- Outbound: 8 connections (by default)
- Inbound: 125 connections (by default)
- Per-IP limit: 1 inbound per IP

## Memory Usage

**Per header:** ~120 bytes
- CBlockIndex overhead: ~100 bytes
- Map node overhead: ~20 bytes

**Scaling:**
- 1M headers: ~120 MB
- 10M headers: ~1.2 GB (typical desktop)
- 100M headers: ~12 GB (requires server hardware)

## Documentation Files

| File | Purpose |
|------|---------|
| `README.md` | Quick start guide |
| `CODEBASE_STRUCTURE.md` | Detailed architecture (this) |
| `QUICK_REFERENCE.md` | Quick lookup (this file) |
| `PROJECT_PLAN.md` | Development phases & timeline |
| `LOCKING_ORDER.md` | Thread safety & deadlock prevention |
| `IBD_ANALYSIS.md` | Initial Block Download design |
| `NETWORK_MISSING.md` | Features deferred from Bitcoin Core |
| `CONSENSUS_FIXES.md` | Consensus rule implementation notes |
| `SERIALIZATION_SPECIFICATION.md` | Wire format details |

## Common Tasks

### Add a new message type
1. Define in `network/protocol.hpp` (message ID)
2. Create handler in `network/message.hpp`
3. Implement serialization in `network/message.cpp`
4. Register handler in `NetworkManager::setup_peer_message_handler()`

### Add validation rule
1. Implement check function in `validation/validation.cpp`
2. Add call to `ChainstateManager::AcceptBlockHeader()` or `CheckBlockHeader()`
3. Add test in `test/validation_tests.cpp`

### Debug thread issues
1. Check `LOCKING_ORDER.md` for lock ordering
2. Use GUARDED_BY annotations (see `util/threadsafety.hpp`)
3. Build with ThreadSanitizer: `cmake -DSANITIZE=thread -B build`
4. Run: `./build/coinbasechain_tests`

### Performance profiling
1. Build with optimizations: `cmake -DCMAKE_BUILD_TYPE=Release`
2. Use perf or similar profiler
3. Look for hot paths in:
   - `ChainstateManager::AcceptBlockHeader()`
   - `CheckBlockHeader()` (RandomX verification)
   - `HeaderSync::ProcessHeaders()`
