# CoinbaseChain - Headers-Only Blockchain Implementation

A simplified, headers-only blockchain implementation inspired by Bitcoin/Unicity, built with modern C++20 and Boost.Asio.

## Project Status

**Core Infrastructure Complete ✅**

### Completed Components:

1. **Message Protocol** (`network/protocol.hpp`, `network/message.hpp`)
   - Bitcoin-compatible wire protocol
   - Message serialization/deserialization
   - VarInt encoding
   - All basic message types (VERSION, VERACK, PING, PONG, ADDR, HEADERS, etc.)

2. **Peer Management** (`network/peer.hpp`)
   - Asynchronous TCP connections with Boost.Asio
   - Bitcoin protocol handshake (VERSION/VERACK)
   - Message framing and parsing
   - Ping/pong keepalive
   - Connection state management

3. **Testing Framework**
   - Catch2 v3.5.2 integrated
   - Block header serialization tests
   - File utility tests
   - 36 passing assertions across 4 test cases

4. **Address Manager** (`network/addr_manager.hpp`)
   - Tried/new table separation
   - Peer address tracking
   - Stale address cleanup
   - Random selection with preference for tried addresses
   - TODO: Bucket-based anti-sybil protection

5. **File Persistence** (`util/files.hpp`)
   - Crash-safe atomic file writes
   - Temp file → fsync → atomic rename pattern
   - Cross-platform support (Linux, macOS, Windows)
   - Default data directory management

6. **Peer Manager** (`network/peer_manager.hpp`)
   - Connection lifecycle management
   - Inbound/outbound connection limits
   - Peer tracking and queries
   - Periodic maintenance
   - TODO: Advanced features from Unicity (see NETWORK_MISSING.md)

7. **Network Manager** (`network/network_manager.hpp`)
   - Multi-threaded IO with Boost.Asio
   - Automatic outbound connection attempts
   - Inbound connection acceptance
   - Message routing (ADDR/GETADDR handling)
   - Periodic maintenance tasks
   - TODO: Bandwidth limits, connection types, DNS seeding (see NETWORK_MISSING.md)

8. **Application Framework** (`app/application.hpp`)
   - Component initialization and coordination
   - Configuration management
   - Signal handling (SIGINT/SIGTERM)
   - Graceful shutdown
   - Command-line argument parsing

### Pending Components:

9. **Header Sync** (Phase 6)
10. **Block Sync** (Phase 7)
11. **Block Relay** (Phase 8)
12. **Integration Testing** (Phase 9)

## Building

```bash
# Configure
cmake -B build -S .

# Build
cmake --build build

# Run tests
./build/coinbasechain_tests

# Run the node
./build/bin/coinbasechain --help
```

## Usage

```bash
# Start with default settings (outbound-only)
./build/bin/coinbasechain

# Start with inbound connections enabled
./build/bin/coinbasechain --listen --port=8333

# Custom data directory
./build/bin/coinbasechain --datadir=/path/to/data

# Show all options
./build/bin/coinbasechain --help
```

## Architecture

### Terminology

CoinbaseChain is a headers-only blockchain (no transactions). We follow Bitcoin Core conventions:

- **Header**: The data structure `CBlockHeader` containing version, prev hash, miner address, timestamp, nBits, nonce, and RandomX hash
- **Block**: Conceptual term for a unit in the blockchain. In our case, a "block" is just a header (no transaction data).
- **Block index**: Metadata about a header (`CBlockIndex`) including validation status, chainwork, and tree links
- **Block hash**: The hash of a header (used to identify blocks)
- **Block height**: Position in the chain (genesis = 0)
- **Blockchain**: The sequence of headers forming the chain

**When in doubt:** Use "header" for the data structure, "block" for conceptual references.

### Block Header Structure

CoinbaseChain uses Unicity's block header format (100 bytes):
- `nVersion` (4 bytes) - Block version
- `hashPrevBlock` (32 bytes) - Previous block hash
- `minerAddress` (20 bytes) - Miner's address (replaces merkle root)
- `nTime` (4 bytes) - Unix timestamp
- `nBits` (4 bytes) - Difficulty target
- `nNonce` (4 bytes) - Proof-of-work nonce
- `hashRandomX` (32 bytes) - RandomX PoW hash

**Key difference from Bitcoin:** Uses `minerAddress` instead of `hashMerkleRoot` since there are no transactions.

### Component Hierarchy

```
Application
├── NetworkManager (coordinator)
│   ├── PeerManager (connection lifecycle)
│   │   └── Peer (individual connections)
│   └── AddressManager (peer discovery)
└── Data Directory (persistence)
```

### Threading Model

- **Main thread**: Application lifecycle, signal handling
- **IO thread pool**: Boost.Asio proactor pattern (default: 4 threads)
  - Asynchronous socket I/O
  - Message processing
  - Timer callbacks

## Design Philosophy

**Simplified vs Unicity/Bitcoin Core:**

We intentionally split Unicity's monolithic components for better modularity:

- **Unicity PeerManager**: 82+ methods, all-in-one (message processing, sync, validation)
- **Our design**: Separate PeerManager (lifecycle), HeaderSync (sync logic), NetworkManager (coordination)

- **Unicity CConnman**: 78 methods, 4,655 lines, 6 dedicated threads
- **Our NetworkManager**: ~15 methods, ~400 lines, thread pool + Boost.Asio

See `NETWORK_MISSING.md` for detailed comparison of missing features.

## Testing

```bash
# Run all tests
./build/coinbasechain_tests

# Run specific test
./build/coinbasechain_tests "[block]"

# Verbose output
./build/coinbasechain_tests -s
```

## Project Structure

```
coinbasechain/
├── include/
│   ├── app/                 # Application framework
│   ├── network/             # Networking components
│   ├── primitives/          # Block headers
│   └── util/                # Utilities
├── src/
│   ├── app/                 # Application implementation
│   ├── crypto/              # SHA-256
│   ├── network/             # Networking implementation
│   ├── primitives/          # Block header implementation
│   └── util/                # File utilities
├── test/                    # Catch2 tests
├── CMakeLists.txt
├── README.md
└── NETWORK_MISSING.md       # Comparison with Unicity
```

## Dependencies

- **C++20** compiler (GCC 10+, Clang 12+, MSVC 2019+)
- **CMake 3.20+**
- **Boost** (Asio, System)
- **Catch2** (bundled, header-only)

## TODO

See inline TODO comments in headers for production improvements:
- `network/addr_manager.hpp`: Bucket-based anti-sybil protection
- `network/peer_manager.hpp`: Misbehavior scoring, scheduled tasks
- `NETWORK_MISSING.md`: Full list of missing CConnman features

## License

Based on Bitcoin Core / Unicity (MIT License)
