# Bitcoin-Equivalent Networking Layer Implementation Plan

**Project:** CoinbaseChain P2P Network Stack
**Start Date:** 2025-10-12
**Status:** ✅ **Core Implementation Complete** (2025-10-13)
**Progress:** Phases 1-6 Complete | Persistence Implemented | Active Development

---

## 🎯 Current Status (2025-10-13)

### ✅ Completed Features

**Networking Core:**
- ✅ Protocol foundation (message types, serialization)
- ✅ Peer connection management (async I/O with Boost.Asio)
- ✅ Address manager with tried/new buckets
- ✅ Peer manager with connection limits
- ✅ Network manager (top-level coordinator)
- ✅ Header synchronization (GETHEADERS/HEADERS)
- ✅ Self-connection prevention (nLocalHostNonce)
- ✅ Proper logging (spdlog to debug.log)

**Persistence:**
- ✅ BlockManager persistence (JSON, 10 min periodic + shutdown)
- ✅ AddressManager persistence (JSON, 15 min periodic + shutdown)
- ✅ Load on startup, save on shutdown + periodic
- ✅ Comprehensive tests for persistence

**Chain Management:**
- ✅ CBlockIndex with tree structure
- ✅ CChain with O(1) height access
- ✅ BlockManager with block index
- ✅ Header validation and chain selection
- ✅ Genesis block initialization

### 🚧 In Progress

- ⏳ Anchors (save last 2 outbound peers, eclipse attack resistance)

### 📋 Next Priorities

1. **Anchors** (1-2 hours) - Save/load last outbound peers for reconnection
2. **Inactivity Timeout** (1-2 hours) - 20min TIMEOUT_INTERVAL
3. **DNS Seeding** (2-3 hours) - Bootstrap from DNS seeds
4. **Bandwidth Tracking** (1-2 hours) - Stats for bytes sent/received
5. **Testing** - End-to-end network testing

### 📂 Project Structure (Actual)

## Project Structure

```
src/
├── network/
│   ├── network_manager.hpp/cpp    [Not Started]
│   ├── peer_manager.hpp/cpp        [Not Started]
│   ├── peer.hpp/cpp                [Not Started]
│   ├── message.hpp/cpp             [Not Started]
│   ├── protocol.hpp/cpp            [Not Started]
│   └── addr_manager.hpp/cpp        [Not Started]
├── sync/
│   ├── block_sync.hpp/cpp          [Not Started]
│   └── header_sync.hpp/cpp         [Not Started]
└── relay/
    └── block_relay.hpp/cpp         [Not Started]
```

---

## Phase 1: Protocol Foundation ⏸️
**Status:** Not Started
**Files:** `protocol.hpp/cpp`, `message.hpp/cpp`
**Estimated Time:** 2-3 days

### Objectives
Define the P2P protocol message types and serialization framework similar to Bitcoin's message system.

### Tasks
- [ ] Define protocol constants
  - [ ] Magic bytes for network identification
  - [ ] Protocol version numbers
  - [ ] Service flags (NODE_NETWORK, NODE_WITNESS, etc.)
  - [ ] Default ports and network identifiers

- [ ] Implement message header structure
  - [ ] 24-byte header: magic (4) + command (12) + length (4) + checksum (4)
  - [ ] Command name handling (null-padded strings)
  - [ ] Message length validation

- [ ] Define message types (enums/constants)
  - [ ] Handshake: VERSION, VERACK
  - [ ] Connectivity: PING, PONG, ADDR, GETADDR
  - [ ] Inventory: INV, GETDATA, NOTFOUND
  - [ ] Block sync: GETHEADERS, HEADERS, GETBLOCKS, BLOCK
  - [ ] Transaction: TX, MEMPOOL

- [ ] Create serialization framework
  - [ ] Leverage existing `endian.h` for wire format
  - [ ] Implement VarInt encoding/decoding
  - [ ] Create serializable base class/concept
  - [ ] Add buffer management utilities

- [ ] Implement message checksumming
  - [ ] Double SHA-256 implementation (first 4 bytes)
  - [ ] Checksum validation on receive

- [ ] Create message payload classes
  - [ ] VersionMessage (version, services, timestamp, addr_recv, addr_from, nonce, user_agent, start_height)
  - [ ] PingMessage / PongMessage (nonce)
  - [ ] AddrMessage (vector of (timestamp, services, IP, port))
  - [ ] InvMessage / GetDataMessage (vector of inventory items)

### Dependencies
- Existing `endian.h` utilities
- SHA-256 implementation (add if needed)

### Success Criteria
- Can serialize/deserialize all basic message types
- Messages pass checksum validation
- Wire format matches Bitcoin protocol specification

---

## Phase 2: Peer Connection ⏸️
**Status:** Not Started
**Files:** `peer.hpp/cpp`
**Estimated Time:** 3-4 days

### Objectives
Implement individual peer connection handling with full async I/O using Boost.Asio.

### Tasks
- [ ] Create Peer class skeleton
  - [ ] Constructor accepting `tcp::socket` and `io_context`
  - [ ] Member variables: socket, state, peer_info, buffers
  - [ ] Use `std::enable_shared_from_this` for async operations

- [ ] Implement connection state machine
  - [ ] States: CONNECTING, VERSION_SENT, VERACK_RECEIVED, CONNECTED, DISCONNECTING, DISCONNECTED
  - [ ] State transition validation
  - [ ] State change callbacks/signals

- [ ] Add async message reading
  - [ ] Read header (24 bytes) asynchronously
  - [ ] Validate magic bytes and checksum
  - [ ] Read payload based on header length
  - [ ] Deserialize into appropriate message type
  - [ ] Chain reads for continuous operation

- [ ] Add async message writing
  - [ ] Serialize message to buffer
  - [ ] Compute and add checksum
  - [ ] Write asynchronously with completion handler
  - [ ] Queue messages if write in progress

- [ ] Implement version handshake
  - [ ] Send VERSION on connect
  - [ ] Wait for VERSION from peer
  - [ ] Send VERACK after receiving VERSION
  - [ ] Transition to CONNECTED state after both VERACK

- [ ] Add ping/pong keepalive
  - [ ] Send PING every N seconds
  - [ ] Track last received time
  - [ ] Disconnect if no response within timeout
  - [ ] Respond to incoming PING with PONG

- [ ] Handle timeouts and errors
  - [ ] Connection timeout (initial handshake)
  - [ ] Read/write timeouts
  - [ ] Handle `boost::system::error_code`
  - [ ] Graceful shutdown on error

- [ ] Add peer metadata tracking
  - [ ] IP address and port
  - [ ] Advertised services
  - [ ] Protocol version
  - [ ] User agent string
  - [ ] Best known block height
  - [ ] Connection time and last activity

### Dependencies
- Phase 1 (message serialization)
- Boost.Asio

### Success Criteria
- Can establish connection with test peer
- Completes version handshake successfully
- Can send and receive all message types
- Properly handles disconnections and errors

---

## Phase 3: Address Manager ⏸️
**Status:** Not Started
**Files:** `addr_manager.hpp/cpp`
**Estimated Time:** 2-3 days

### Objectives
Manage peer addresses for discovery, similar to Bitcoin's AddrMan with "new" and "tried" buckets.

### Tasks
- [ ] Define address storage structures
  - [ ] AddressInfo: IP, port, timestamp, services, last_attempt, last_success
  - [ ] Bucket-based storage (hash table approach)
  - [ ] "New" table: addresses we haven't connected to
  - [ ] "Tried" table: addresses we've successfully connected to

- [ ] Implement address selection logic
  - [ ] Probabilistic selection (prefer tried, sometimes pick new)
  - [ ] IP diversity (avoid clustering in same /16 subnet)
  - [ ] Recency weighting (prefer recently seen addresses)
  - [ ] Service filtering (require specific service flags)

- [ ] Add address management operations
  - [ ] `Add()`: Add new address to "new" table
  - [ ] `Good()`: Move address from "new" to "tried" on successful connection
  - [ ] `Attempt()`: Mark address as attempted
  - [ ] `Select()`: Select address for outbound connection
  - [ ] `Connected()`: Mark address as currently connected

- [ ] Implement address aging and eviction
  - [ ] Remove stale addresses (old timestamps)
  - [ ] Evict worst addresses when tables full
  - [ ] Score addresses based on success rate

- [ ] Add DNS seed support
  - [ ] DNS resolver for seed hostnames
  - [ ] Parse A/AAAA records into addresses
  - [ ] Bootstrap from DNS seeds on first run

- [ ] Implement serialization for persistence
  - [ ] Save to disk periodically
  - [ ] Load on startup
  - [ ] Version-aware format

- [ ] Handle ADDR message processing
  - [ ] Parse received ADDR messages
  - [ ] Add addresses to manager
  - [ ] Relay addresses to other peers (with rate limiting)

### Dependencies
- Phase 1 (protocol/messages)

### Success Criteria
- Can bootstrap from DNS seeds
- Maintains diverse set of peer addresses
- Successfully selects peers for connection
- Persists and restores address state

---

## Phase 4: Peer Manager ⏸️
**Status:** Not Started
**Files:** `peer_manager.hpp/cpp`
**Estimated Time:** 3-4 days

### Objectives
Manage the lifecycle of multiple peer connections with connection limits and misbehavior tracking.

### Tasks
- [ ] Create peer tracking structures
  - [ ] Map of peer_id → shared_ptr<Peer>
  - [ ] Separate tracking for inbound vs outbound
  - [ ] Connection state counters

- [ ] Implement connection limits
  - [ ] Max inbound connections (e.g., 125)
  - [ ] Max outbound connections (e.g., 8)
  - [ ] Max feeler connections (1-2 for testing)
  - [ ] Per-subnet limits (avoid Sybil attacks)

- [ ] Add automatic peer selection
  - [ ] Query AddrManager for outbound candidates
  - [ ] Initiate async connection attempts
  - [ ] Handle connection failures and retry
  - [ ] Maintain target outbound connection count

- [ ] Implement misbehavior tracking
  - [ ] Misbehavior score per peer (0-100)
  - [ ] Increment on protocol violations
  - [ ] Automatic ban when threshold exceeded
  - [ ] Ban duration and expiration
  - [ ] Persist ban list to disk

- [ ] Add connection retry logic
  - [ ] Exponential backoff for failed connections
  - [ ] Track last attempt time in AddrManager
  - [ ] Avoid hammering unresponsive peers

- [ ] Implement peer eviction
  - [ ] Evict worst peers when at connection limit
  - [ ] Metrics: ping time, last activity, misbehavior score
  - [ ] Protect certain peers (e.g., longest-connected)

- [ ] Add peer state coordination
  - [ ] Broadcast messages to all peers
  - [ ] Query peer capabilities
  - [ ] Track which inventory items each peer has
  - [ ] Coordinate parallel requests

### Dependencies
- Phase 2 (Peer)
- Phase 3 (AddrManager)

### Success Criteria
- Maintains stable peer connections
- Automatically reconnects when peers disconnect
- Enforces connection limits
- Detects and bans misbehaving peers

---

## Phase 5: Network Manager ⏸️
**Status:** Not Started
**Files:** `network_manager.hpp/cpp`
**Estimated Time:** 2-3 days

### Objectives
Top-level network coordinator integrating all networking components.

### Tasks
- [ ] Create NetworkManager class
  - [ ] Owns io_context for async operations
  - [ ] Aggregates PeerManager and AddrManager
  - [ ] Provides start/stop interface

- [ ] Implement inbound connection handling
  - [ ] Create tcp::acceptor bound to listen port
  - [ ] Accept connections asynchronously
  - [ ] Create Peer objects for accepted sockets
  - [ ] Register with PeerManager

- [ ] Implement outbound connection initiation
  - [ ] Request addresses from AddrManager
  - [ ] Create and connect tcp::socket
  - [ ] Create Peer objects on successful connect
  - [ ] Handle connection errors

- [ ] Add message routing
  - [ ] Register message handlers for different message types
  - [ ] Route incoming messages to appropriate subsystems
  - [ ] Sync components: header_sync, block_sync
  - [ ] Relay components: block_relay

- [ ] Implement network lifecycle
  - [ ] Start(): Initialize components, start accepting, connect to peers
  - [ ] Stop(): Gracefully disconnect all peers, stop io_context
  - [ ] Restart logic for error recovery

- [ ] Add network statistics API
  - [ ] Total bytes sent/received
  - [ ] Number of active peers (inbound/outbound)
  - [ ] Message counts by type
  - [ ] Ban statistics

- [ ] Implement work scheduling
  - [ ] Periodic tasks (save addresses, send pings, evict peers)
  - [ ] Use boost::asio::steady_timer
  - [ ] Configurable intervals

### Dependencies
- Phase 4 (PeerManager)
- All previous phases

### Success Criteria
- Can start and stop network operations cleanly
- Accepts and maintains multiple peer connections
- Routes messages to appropriate handlers
- Provides network status visibility

---

## Phase 6: Header Sync ⏸️
**Status:** Not Started
**Files:** `header_sync.hpp/cpp`
**Estimated Time:** 4-5 days

### Objectives
Implement headers-first synchronization for rapid initial sync.

### Tasks
- [ ] Create header chain storage
  - [ ] In-memory chain of headers
  - [ ] Map of block_hash → header
  - [ ] Track best header and active chain tip

- [ ] Implement GETHEADERS request logic
  - [ ] Build locator (exponentially spaced hashes)
  - [ ] Send to multiple peers in parallel
  - [ ] Track in-flight requests

- [ ] Add HEADERS message processing
  - [ ] Parse received headers (up to 2000 per message)
  - [ ] Validate PoW for each header
  - [ ] Verify header chain linkage (prev_hash)
  - [ ] Check timestamp rules
  - [ ] Request more headers if batch is full (2000)

- [ ] Implement parallel header download
  - [ ] Request from multiple peers simultaneously
  - [ ] Deduplicate overlapping ranges
  - [ ] Handle out-of-order receipt

- [ ] Add chain reorganization detection
  - [ ] Detect when received header isn't best chain
  - [ ] Compute fork point
  - [ ] Switch to longer chain
  - [ ] Notify dependent components

- [ ] Track sync state
  - [ ] Initial Block Download (IBD) mode
  - [ ] Headers-synced state (caught up to peers)
  - [ ] Progress reporting (percentage/height)

- [ ] Implement checkpoints (optional)
  - [ ] Hardcoded block hash checkpoints
  - [ ] Validate headers against checkpoints
  - [ ] Fast reject invalid chains

### Dependencies
- Phase 5 (NetworkManager for message routing)
- Block header definition (need to create)

### Success Criteria
- Can download entire header chain from genesis
- Validates headers correctly
- Handles chain reorgs
- Signals when headers are synced

---

## Phase 7: Block Sync ⏸️
**Status:** Not Started
**Files:** `block_sync.hpp/cpp`
**Estimated Time:** 4-5 days

### Objectives
Download and validate full blocks after headers are synced.

### Tasks
- [ ] Implement block request pipeline
  - [ ] Queue of blocks to download (ordered by height)
  - [ ] Request blocks via GETDATA messages
  - [ ] Track which peer has which blocks (from INV)
  - [ ] Parallel requests to different peers

- [ ] Add BLOCK message processing
  - [ ] Parse received block data
  - [ ] Validate block structure
  - [ ] Validate all transactions in block
  - [ ] Check block against header

- [ ] Implement block validation
  - [ ] Merkle root verification
  - [ ] Transaction validation (signatures, inputs exist, no double-spends)
  - [ ] Coinbase validation
  - [ ] Block size limits

- [ ] Handle orphan blocks
  - [ ] Store blocks received before parent
  - [ ] Connect when parent arrives
  - [ ] Limit orphan pool size

- [ ] Coordinate with header sync
  - [ ] Only download blocks for best header chain
  - [ ] Handle reorgs (disconnect blocks, download new chain)

- [ ] Add block storage interface
  - [ ] Write validated blocks to disk
  - [ ] Index by height and hash
  - [ ] Implement block retrieval

- [ ] Implement block download scheduling
  - [ ] Prioritize recent blocks
  - [ ] Download in parallel windows (e.g., 128 blocks)
  - [ ] Handle peer disconnections mid-download

### Dependencies
- Phase 6 (HeaderSync)
- Block and Transaction definitions (need to create)
- Storage layer (need to create)

### Success Criteria
- Downloads all blocks from genesis to tip
- Validates blocks correctly
- Handles reorgs during sync
- Persists blocks to storage

---

## Phase 8: Block Relay ⏸️
**Status:** Not Started
**Files:** `block_relay.hpp/cpp`
**Estimated Time:** 3-4 days

### Objectives
Efficiently relay new blocks to peers with minimal bandwidth.

### Tasks
- [ ] Implement block announcement
  - [ ] Announce new blocks via INV messages
  - [ ] Track which peers we've announced to
  - [ ] Avoid duplicate announcements

- [ ] Add compact block relay (BIP152-style)
  - [ ] SendCompactBlock message type
  - [ ] Short transaction IDs (6 bytes)
  - [ ] Prefilled transactions (coinbase, etc.)
  - [ ] Reconstruction from mempool

- [ ] Implement unsolicited block handling
  - [ ] Accept blocks pushed by peers
  - [ ] Validate before further relay
  - [ ] Add to block sync if behind

- [ ] Add transaction relay tracking
  - [ ] Track which transactions each peer knows
  - [ ] Determine which transactions to include in compact block
  - [ ] Request missing transactions via GETDATA

- [ ] Implement relay policy
  - [ ] Don't relay invalid blocks
  - [ ] Don't relay to peers who already have it
  - [ ] Rate limiting to prevent DoS

- [ ] Add block download request deduplication
  - [ ] Track in-flight block requests
  - [ ] Avoid requesting same block from multiple peers
  - [ ] Timeout and retry if peer doesn't respond

- [ ] Support legacy relay mode
  - [ ] Full block relay for older protocol versions
  - [ ] Fallback if compact block reconstruction fails

### Dependencies
- Phase 7 (BlockSync)
- Mempool component (need to create for compact blocks)

### Success Criteria
- Can relay new blocks to all peers efficiently
- Compact blocks reduce bandwidth by >90%
- Handles both legacy and compact relay
- No duplicate relays

---

## Phase 9: Integration & Testing ⏸️
**Status:** Not Started
**Estimated Time:** 5-7 days

### Objectives
Validate the complete networking stack with comprehensive testing.

### Tasks
- [ ] Component integration testing
  - [ ] Test all components working together
  - [ ] Verify message flow through entire stack
  - [ ] Test with real Bitcoin testnet/regtest

- [ ] Stress testing
  - [ ] Many simultaneous peer connections (100+)
  - [ ] High message throughput
  - [ ] Memory usage under load
  - [ ] Message flood handling

- [ ] Network partition testing
  - [ ] Simulate network splits
  - [ ] Verify recovery and convergence
  - [ ] Test reorg handling

- [ ] Error condition testing
  - [ ] Malformed messages
  - [ ] Protocol violations
  - [ ] Resource exhaustion
  - [ ] Peer misbehavior

- [ ] Performance profiling
  - [ ] Identify bottlenecks
  - [ ] Optimize hot paths
  - [ ] Reduce memory allocations
  - [ ] Benchmark throughput

- [ ] Security audit
  - [ ] Review DoS attack vectors
  - [ ] Validate input sanitization
  - [ ] Check resource limits
  - [ ] Test ban/disconnect logic

- [ ] Documentation
  - [ ] API documentation
  - [ ] Architecture overview
  - [ ] Usage examples
  - [ ] Configuration guide

- [ ] Example applications
  - [ ] Simple node that syncs headers
  - [ ] Full node that syncs and relays blocks
  - [ ] Network monitoring tool

### Dependencies
- All previous phases

### Success Criteria
- All tests pass consistently
- Can sync with Bitcoin testnet
- No memory leaks or crashes
- Acceptable performance metrics
- Complete documentation

---

## Additional Components Needed

### Blockchain Data Structures
- [ ] Block header definition
- [ ] Block definition
- [ ] Transaction definition (input, output, witness)
- [ ] Merkle tree implementation

### Cryptography
- [ ] SHA-256 implementation
- [ ] RIPEMD-160 (for addresses)
- [ ] ECDSA signature verification (secp256k1)
- [ ] Hash functions (Hash160, Hash256)

### Storage Layer
- [ ] Key-value store interface
- [ ] Block index
- [ ] UTXO set
- [ ] Mempool

### Utilities
- [ ] VarInt encoding/decoding
- [ ] Base58 encoding/decoding
- [ ] Bech32 encoding (for SegWit addresses)
- [ ] Logging framework
- [ ] Configuration system

---

## Current Phase Progress

### Phase 1: Protocol Foundation
```
Progress: 0/6 major tasks complete
Status: ⏸️ Not Started
Next Actions:
  1. Define protocol constants in protocol.hpp
  2. Implement message header structure
  3. Create serialization framework
```

---

## Notes & Decisions

### Architecture Decisions
- **Async I/O:** All network operations use Boost.Asio with callbacks/futures
- **Threading Model:** Single io_context thread initially, can expand to thread pool
- **Memory Management:** Use shared_ptr for Peer objects due to async lifetime
- **Error Handling:** Use error_code pattern, no exceptions in async paths

### Design Patterns
- **Observer Pattern:** For peer state changes and message notifications
- **Strategy Pattern:** For different message handlers
- **Factory Pattern:** For creating message objects from wire data

### Technical Debt
- Track items that need refactoring here as we build

---

## Timeline Estimates

| Phase | Estimated Duration | Dependencies |
|-------|-------------------|--------------|
| Phase 1 | 2-3 days | None |
| Phase 2 | 3-4 days | Phase 1 |
| Phase 3 | 2-3 days | Phase 1 |
| Phase 4 | 3-4 days | Phase 2, 3 |
| Phase 5 | 2-3 days | Phase 4 |
| Phase 6 | 4-5 days | Phase 5 |
| Phase 7 | 4-5 days | Phase 6 |
| Phase 8 | 3-4 days | Phase 7 |
| Phase 9 | 5-7 days | All phases |
| **Total** | **28-38 days** | - |

---

## References

- [Bitcoin Protocol Documentation](https://en.bitcoin.it/wiki/Protocol_documentation)
- [Bitcoin Core Source Code](https://github.com/bitcoin/bitcoin)
- [BIP152 - Compact Block Relay](https://github.com/bitcoin/bips/blob/master/bip-0152.mediawiki)
- [Boost.Asio Documentation](https://www.boost.org/doc/libs/release/doc/html/boost_asio.html)
- [Bitcoin Developer Guide](https://developer.bitcoin.org/devguide/)

---

**Last Updated:** 2025-10-12
**Next Review:** After Phase 1 completion
