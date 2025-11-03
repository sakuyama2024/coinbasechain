# Network Library Architecture

## Overview

The coinbasechain network layer implements a Bitcoin Core-inspired P2P networking stack with a modern C++ architecture. The design emphasizes correctness, thread safety, and production readiness through careful resource management and clear component boundaries.

## Core Architecture: 3-Manager Pattern

The network layer is organized around three domain-specific managers coordinated by NetworkManager:

```
┌─────────────────────────────────────────────────────────────┐
│                     NetworkManager                          │
│  (Coordinator, io_context owner, message routing)          │
└─────────────────────────────────────────────────────────────┘
                           │
         ┌─────────────────┼─────────────────┐
         ▼                 ▼                 ▼
┌────────────────┐ ┌──────────────┐ ┌─────────────────┐
│ PeerLifecycle  │ │   Peer       │ │  Blockchain     │
│   Manager      │ │  Discovery   │ │    Sync         │
│                │ │   Manager    │ │   Manager       │
└────────────────┘ └──────────────┘ └─────────────────┘
         │                 │                 │
         ▼                 ▼                 ▼
  ┌──────────┐    ┌──────────────┐  ┌──────────────┐
  │Connection│    │AddressManager│  │HeaderSync    │
  │  State   │    │AnchorManager │  │BlockRelay    │
  └──────────┘    └──────────────┘  └──────────────┘
```

### 1. NetworkManager (Top-Level Coordinator)

**Responsibilities:**
- Owns and manages `boost::asio::io_context` (the reactor)
- Routes messages via MessageDispatcher (handler registry pattern)
- Coordinates the 3 managers
- Owns lifecycle (start/stop)
- Manages periodic tasks (timers: connect, maintenance, feeler, sendmessages)
- Self-connection prevention (nonce checking)

**Key Design Decisions:**
- **Single-threaded reactor**: `io_threads` MUST be 1 in production (0 = external io_context for tests)
- **Shared io_context ownership**: Uses `std::shared_ptr<io_context>` to ensure lifetime safety
- **Exception safety**: All operations that can throw (SaveAnchors, LoadAnchors) are wrapped in try/catch
- **TOCTOU prevention**: `running_` flag is re-checked before critical operations to prevent races with stop()

**Threading Model:**
```cpp
// Reactor layer: single-threaded (no strands needed)
Config::io_threads = 1;  // All handlers serialized

// Application layer: can be multi-threaded
// - Validation, mining, RPC run on separate threads
// - They interact with NetworkManager via thread-safe interfaces
```

**File:** `include/network/network_manager.hpp`, `src/network/network_manager.cpp`

---

### 2. PeerLifecycleManager (Connection Lifecycle)

**Responsibilities:**
- Manages peer connection state machine (DISCONNECTED → CONNECTING → CONNECTED → READY)
- Handles inbound connection acceptance
- Handles outbound connection initiation (via callback to NetworkManager)
- VERSION/VERACK handshake orchestration
- Connection limits (MAX_OUTBOUND_FULL_RELAY_CONNECTIONS, MAX_INBOUND_CONNECTIONS)
- Peer eviction logic
- Ban/discourage enforcement
- Anchor connection management
- Nonce collision detection (self-connection prevention)

**Connection Backoff:**
When connection attempts fail due to persistent errors (banned/discouraged addresses), PeerLifecycleManager calls `discovery_manager_->Failed()` to trigger exponential backoff in AddressManager. This prevents infinite retry loops.

**File:** `include/network/peer_lifecycle_manager.hpp`, `src/network/peer_lifecycle_manager.cpp`

---

### 3. PeerDiscoveryManager (Address Discovery)

**Responsibilities:**
- Peer address discovery and management
- Delegates to AddressManager (addr.dat persistence, tried/new tables, selection algorithm)
- Delegates to AnchorManager (anchors.json persistence for eclipse attack resistance)
- Handles ADDR/GETADDR messages
- Address announcement logic
- Feeler connection address selection

**Owned Components:**
- **AddressManager**: Bitcoin Core's address selection algorithm (tried/new tables, collision-resistant selection)
- **AnchorManager**: Stores 2 block-relay-only anchor connections across restarts

**File:** `include/network/peer_discovery_manager.hpp`, `src/network/peer_discovery_manager.cpp`

---

### 4. BlockchainSyncManager (Header & Block Sync)

**Responsibilities:**
- Initial Block Download (IBD) coordination
- Header sync state machine
- Block relay and announcement
- Sync peer selection and rotation
- Stall detection and timeout handling

**Owned Components:**
- **HeaderSyncManager**: Manages GETHEADERS/HEADERS protocol, sync peer selection, stall detection
- **BlockRelayManager**: Manages INV/GETDATA for blocks, announcement queues, relay policy

**Key Patterns:**
- **Single sync peer during IBD**: Prevents resource exhaustion attacks
- **Sync peer rotation on stall**: 120-second timeout triggers peer switch
- **Block announcement batching**: SendMessages pattern (1-second flush interval)

**File:** `include/network/blockchain_sync_manager.hpp`, `src/network/blockchain_sync_manager.cpp`

---

## Message Flow

### Inbound Message Processing

```
Network Socket
      │
      ▼
Transport::receive()
      │
      ▼
Peer::on_message_received()
      │
      ▼
NetworkManager::handle_message()
      │
      ├─► Check running_ flag (TOCTOU prevention)
      │
      ├─► VERSION: Check nonce collision
      │   └─► If collision: disconnect + remove_peer
      │
      ├─► Re-check running_ before dispatch
      │
      ▼
MessageDispatcher::Dispatch()
      │
      └─► Route to appropriate manager:
          ├─► VERACK      → PeerLifecycleManager::HandleVerack()
          ├─► ADDR        → PeerDiscoveryManager::HandleAddr()
          ├─► GETADDR     → PeerDiscoveryManager::HandleGetAddr()
          ├─► HEADERS     → BlockchainSyncManager::HandleHeaders()
          ├─► INV         → BlockchainSyncManager::HandleInv()
          ├─► GETDATA     → BlockchainSyncManager::HandleGetData()
          └─► PING/PONG   → PeerLifecycleManager (heartbeat)
```

### Outbound Message Sending

```
Manager (e.g., HeaderSyncManager)
      │
      ▼
peer->send_message(msg)
      │
      ▼
Peer::send()
      │
      ▼
Transport::send()
      │
      ▼
Network Socket
```

---

## Connection Lifecycle

### Outbound Connection

```
1. PeerLifecycleManager::ConnectToAnchors() or periodic connect_timer_
        │
        ▼
2. discovery_manager_->SelectAddressToConnect()
        │
        ▼
3. NetworkManager::connect_to_with_permissions()
        │
        ├─► Check: not banned/discouraged
        ├─► Check: connection slots available
        ├─► Check: not already connected
        │
        ▼
4. transport_->connect(address, port)
        │
        ▼
5. On success: PeerLifecycleManager::HandleOutboundConnection()
        │
        ├─► Create Peer object
        ├─► Set state = CONNECTING
        ├─► Send VERSION message
        │
        ▼
6. Peer sends VERSION
        │
        ▼
7. NetworkManager::handle_message() receives VERSION
        │
        ├─► Check nonce collision (self-connection)
        ├─► MessageDispatcher routes to handler
        │
        ▼
8. PeerLifecycleManager::HandleVersion()
        │
        ├─► Validate protocol version
        ├─► Store peer info (services, height, user_agent)
        ├─► Send VERACK
        │
        ▼
9. Receive VERACK from peer
        │
        ▼
10. PeerLifecycleManager::HandleVerack()
        │
        ├─► Set state = READY
        ├─► Mark successfully_connected = true
        ├─► Trigger post-handshake logic:
        │   ├─► discovery_manager_->Good(addr)  // Mark address as working
        │   ├─► Send GETADDR (address discovery)
        │   └─► sync_manager_->OnPeerReady()    // Consider for sync peer
        │
        ▼
11. Connection established - peer can send/receive protocol messages
```

### Inbound Connection

```
1. transport_->listen() accepts connection
        │
        ▼
2. Callback: PeerLifecycleManager::HandleInboundConnection()
        │
        ├─► Check: not banned/discouraged
        ├─► Check: inbound slots available
        ├─► Check: can_accept_inbound_from() (rate limiting)
        │
        ▼
3. Create Peer object, set state = CONNECTING
        │
        ▼
4. Wait for VERSION from remote peer
        │
        ▼
5-11. Same as outbound steps 7-11
```

---

## Reactor Pattern (Event Loop)

NetworkManager uses the **single-threaded reactor pattern** via `boost::asio::io_context`:

```cpp
// Reactor thread (started in NetworkManager::start())
void reactor_thread() {
    while (running_) {
        io_context_->run();  // Blocks, processing events as they arrive
    }
}
```

**Events processed by reactor:**
- Socket readable (message received)
- Socket writable (can send message)
- Socket connection complete
- Socket disconnected
- Timer expired (connect_timer_, maintenance_timer_, feeler_timer_, sendmessages_timer_)

**Why single-threaded?**
- Handlers never run concurrently → no data races
- No locks needed for shared state (peer_manager_, discovery_manager_, sync_manager_)
- Predictable execution order
- Matches Bitcoin Core's design
- Simpler, easier to reason about

**Multi-threaded alternative (not implemented):**
If `io_threads > 1`, would need:
- Strands for all timer handlers
- Strands for all message handlers
- Strands for all shared state access (peer_manager_, discovery_manager_, sync_manager_)

---

## Resource Management & Lifecycle

### Start Sequence

```cpp
bool NetworkManager::start() {
    // 1. Acquire lock, check not already running
    std::unique_lock<std::mutex> lock(start_stop_mutex_);

    // 2. Set running_ = true
    running_.store(true, std::memory_order_release);

    // 3. Start transport (listening socket if enabled)
    transport_->run();

    // 4. Create work guard (keeps io_context alive)
    work_guard_ = make_work_guard(*io_context_);

    // 5. Create timers
    connect_timer_ = make_unique<steady_timer>(*io_context_);
    maintenance_timer_ = make_unique<steady_timer>(*io_context_);
    feeler_timer_ = make_unique<steady_timer>(*io_context_);
    sendmessages_timer_ = make_unique<steady_timer>(*io_context_);

    // 6. Start io_threads (if owned io_context)
    for (size_t i = 0; i < config_.io_threads; ++i) {
        io_threads_.emplace_back([this]() { io_context_->run(); });
    }

    // 7. Load anchors from disk (exception-safe)
    LoadAnchors(datadir + "/anchors.json");

    // 8. Start NAT traversal (UPnP)
    if (nat_manager_) nat_manager_->Start(port);

    // 9. Schedule periodic tasks
    schedule_next_connection_attempt();
    schedule_next_maintenance();
    schedule_next_feeler();
    schedule_next_sendmessages();

    return true;
}
```

### Stop Sequence

```cpp
void NetworkManager::stop() {
    // IMPORTANT: This method may BLOCK for several seconds if timer handlers are slow

    // 1. Set running_ = false (prevents new operations)
    running_.store(false, std::memory_order_release);

    // 2. Cancel all timers
    connect_timer_->cancel();
    maintenance_timer_->cancel();
    feeler_timer_->cancel();
    sendmessages_timer_->cancel();

    // 3. Save anchors (exception-safe - MUST NOT throw)
    try {
        SaveAnchors(datadir + "/anchors.json");
    } catch (...) {
        LOG_ERROR("Failed to save anchors");
    }

    // 4. Disconnect all peers (clean TCP shutdown)
    peer_manager_->Shutdown();        // Disable callbacks first (UAF prevention)
    peer_manager_->disconnect_all();

    // 5. Stop transport (close listening socket)
    transport_->stop();

    // 6. Stop NAT manager
    if (nat_manager_) nat_manager_->Stop();

    // 7. Stop io_context (safe now - all sockets closed, all callbacks run)
    io_context_->stop();

    // 8. Release work guard (allows io_threads to exit)
    work_guard_.reset();

    // 9. Join all io_threads
    for (auto& thread : io_threads_) {
        thread.join();  // May block if handler is slow
    }
    io_threads_.clear();

    // 10. Reset io_context for potential restart
    io_context_->restart();

    // 11. Mark fully stopped, signal waiters
    fully_stopped_ = true;
    stop_cv_.notify_all();
}
```

### Exception Safety

All operations that can throw are protected:

```cpp
// SaveAnchors - called from destructor path (stop())
try {
    SaveAnchors(filepath);
} catch (const std::exception& e) {
    LOG_ERROR("Failed to save anchors: {}", e.what());
} catch (...) {
    LOG_ERROR("Unknown exception saving anchors");
}

// LoadAnchors - called during startup
try {
    auto anchors = discovery_manager_->LoadAnchors(filepath);
    // ... use anchors ...
} catch (const std::exception& e) {
    LOG_ERROR("Failed to load anchors: {}", e.what());
    return false;  // Continue with empty anchors
}
```

**Rationale:**
- `stop()` is called from destructor → MUST NOT throw
- Disk full, corrupted JSON, permission denied → log and continue gracefully

---

## Thread Safety

### Atomic Operations

```cpp
std::atomic<bool> running_;  // Fast-path check without lock

// Usage pattern in handle_message():
if (!running_.load(std::memory_order_acquire)) {
    return false;  // Early exit
}

// ... do work ...

// Re-check before critical operation (TOCTOU prevention)
if (!running_.load(std::memory_order_acquire)) {
    return false;  // Another thread called stop()
}
peer->disconnect();
peer_manager_->remove_peer(peer_id);
```

**Memory ordering:**
- `memory_order_release` on write (in stop()): Ensures all preceding writes visible to readers
- `memory_order_acquire` on read (in handle_message()): Ensures we see all writes before the store

### Mutexes

```cpp
mutable std::mutex start_stop_mutex_;  // Protects start/stop from concurrent calls
std::condition_variable stop_cv_;      // Signals when stop() completes

// Usage:
std::unique_lock<std::mutex> lock(start_stop_mutex_);
// ... modify running_, fully_stopped_ ...
```

### Strand-Free Design

Because `io_threads = 1`, handlers never run concurrently. This allows lock-free access to:
- `peer_manager_` (Peer connection state)
- `discovery_manager_` (Address tables)
- `sync_manager_` (Header sync state)

All accessed from reactor thread only.

---

## io_context Lifetime Management

### Problem: Dangling io_context

If NetworkManager holds a **reference** to external io_context:
```cpp
boost::asio::io_context& io_context_;  // DANGEROUS

~NetworkManager() {
    // Timer destructors access io_context_
    connect_timer_.reset();  // May call io_context methods
}
// If caller destroyed their io_context, this is UB!
```

### Solution: Shared Ownership

```cpp
std::shared_ptr<boost::asio::io_context> io_context_;  // SAFE

// Constructor:
NetworkManager(shared_ptr<io_context> external_io_context)
    : io_context_(external_io_context ? external_io_context
                                       : make_shared<io_context>())
{ }

// Now io_context is kept alive until:
// - NetworkManager is destroyed AND
// - All timers are destroyed AND
// - Any other shared_ptr holders release
```

**For tests with stack-allocated io_context:**
```cpp
// SimulatedNode owns io_context on stack
boost::asio::io_context io_context_;

// Create non-owning shared_ptr (aliasing constructor)
auto io_ptr = std::shared_ptr<io_context>(
    std::shared_ptr<void>{},  // Empty control block
    &io_context_              // Pointer to stack object
);

NetworkManager net(..., io_ptr);  // Safe: SimulatedNode outlives NetworkManager
```

---

## Connection Retry & Backoff

### Problem: Infinite Retry Loop

Old code:
```cpp
auto result = connect_to_with_permissions(addr);
// Ignored! If addr is banned, we retry forever
```

### Solution: Track Failures

```cpp
auto result = connect_to_with_permissions(addr);

if (result == ConnectionResult::AddressBanned ||
    result == ConnectionResult::AddressDiscouraged) {
    // Mark as failed → triggers exponential backoff in AddressManager
    discovery_manager_->Failed(addr);
}
```

**AddressManager backoff:**
- 1st failure: Retry after ~1 hour
- 2nd failure: Retry after ~4 hours
- 3rd failure: Retry after ~16 hours
- etc. (exponential)

**Note:** Transient failures (NoSlotsAvailable, AlreadyConnected) are NOT marked as failed, because they may succeed on next attempt.

---

## Periodic Tasks (Timers)

### 1. Connection Timer (every 5 seconds)
```cpp
void schedule_next_connection_attempt() {
    connect_timer_->expires_after(config_.connect_interval);  // 5s
    connect_timer_->async_wait([this]() {
        if (!running_) return;

        // Select address from AddressManager
        auto addr = discovery_manager_->SelectAddressToConnect();
        if (addr) {
            connect_to(*addr);
        }

        schedule_next_connection_attempt();  // Re-schedule
    });
}
```

### 2. Maintenance Timer (every 30 seconds)
```cpp
void run_maintenance() {
    // - Process peer timeouts
    // - Evict stale connections
    // - Update ban/discourage states
    peer_manager_->process_periodic();
}
```

### 3. Feeler Timer (every 2 minutes + jitter)
```cpp
void schedule_next_feeler() {
    auto delay = compute_feeler_delay();  // 2 min ± random jitter
    feeler_timer_->expires_after(delay);
    feeler_timer_->async_wait([this]() {
        if (!running_) return;

        // Test an address from "new" table
        auto addr = discovery_manager_->SelectFeelerAddress();
        if (addr) {
            connect_to(*addr);
        }

        schedule_next_feeler();
    });
}
```

**Feeler connections:** Short-lived test connections to addresses in the "new" table, used to:
- Verify addresses are still reachable
- Move good addresses to "tried" table
- Prevent eclipse attacks (test many addresses)

### 4. SendMessages Timer (every 1 second)
```cpp
void run_sendmessages() {
    // Flush pending block announcements (Bitcoin Core pattern)
    flush_block_announcements();
}
```

**Block announcement batching:**
- Blocks are added to peer announcement queues immediately when mined/received
- Actual INV messages are sent on 1-second intervals
- Prevents spam, reduces bandwidth

---

## Bitcoin Core Alignment

The design closely follows Bitcoin Core patterns:

| Bitcoin Core | coinbasechain | Notes |
|--------------|---------------|-------|
| `CConnman` | `NetworkManager` | Top-level coordinator |
| `CNode` | `Peer` | Per-connection state |
| `CAddrMan` | `AddressManager` | Tried/new tables, collision-resistant selection |
| `PeerManager::ProcessMessage()` | `MessageDispatcher::Dispatch()` | Message routing |
| `SendMessages()` | `run_sendmessages()` | Periodic flush of announcements |
| Feeler connections | Feeler timer | Eclipse attack resistance |
| Anchors (anchors.dat) | Anchors (anchors.json) | Eclipse attack resistance |
| Single-threaded `CConnman` | Single-threaded `NetworkManager` | No strands needed |
| `CConnman::Stop()` sequence | `NetworkManager::stop()` sequence | Peers → transport → io_context |

---

## Testing Architecture

### SimulatedNetwork (test/network/infra/)

Provides deterministic, fully-controlled network simulation:

```cpp
SimulatedNetwork network(seed);

SimulatedNode node1(1, &network);
SimulatedNode node2(2, &network);

node1.ConnectTo(node2.GetId());
network.AdvanceTime(1000);  // Deterministic time

// Inject faults
SimulatedNetwork::NetworkConditions lossy;
lossy.packet_loss_rate = 0.5;  // 50% packet loss
network.SetLinkConditions(node1, node2, lossy);
```

**Key features:**
- Uses REAL NetworkManager, PeerLifecycleManager, etc.
- Only Transport is simulated (NetworkBridgedTransport)
- Deterministic event ordering (no real async)
- Fault injection (packet loss, latency, reordering, partitions)
- Command tracking (count GETHEADERS, HEADERS, etc.)

### TestOrchestrator

High-level test orchestration:

```cpp
TestOrchestrator orchestrator(&network);

orchestrator.WaitForConnection(node1, node2);
orchestrator.WaitForSync(node, expected_height);
orchestrator.AssertHeight(node, 100);
orchestrator.AssertPeerCount(node, 5);
```

### AttackSimulatedNode

Adversarial testing (DoS, protocol violations):

```cpp
AttackSimulatedNode attacker(666, &network);

attacker.SendOversizedHeaders(victim_id, 2001);  // > MAX_HEADERS_SIZE
attacker.SendNonContinuousHeaders(victim_id, tip);
attacker.SendInvalidPoWHeaders(victim_id, tip, 10);
attacker.SendOrphanHeaders(victim_id, 100);
```

**Test categories:**
- DoS resistance (low_work_headers_tests.cpp, oversized messages)
- Protocol adversarial (header_sync_adversarial_tests.cpp)
- Eclipse attack resistance (feeler_anchor_tests.cpp)
- Synchronization (sync_ibd_tests.cpp, reorg_tests.cpp)
- Scale tests (100+ nodes)

---

## Configuration

### NetworkManager::Config

```cpp
struct Config {
    uint32_t network_magic;              // REQUIRED: 0xd9b4bef9 (mainnet), etc.
    uint16_t listen_port;                // REQUIRED: 8333 (mainnet), 18444 (regtest)
    bool listen_enabled;                 // Accept inbound connections
    bool enable_nat;                     // UPnP NAT traversal
    size_t io_threads;                   // MUST be 1 (production) or 0 (tests)
    std::string datadir;                 // For anchors.json, addr.dat

    std::chrono::seconds connect_interval;          // Default: 5s
    std::chrono::seconds maintenance_interval;      // Default: 30s
    double feeler_max_delay_multiplier;            // Default: 3.0 (cap at 6 min)

    std::optional<uint64_t> test_nonce;  // Override for tests (deterministic)
};
```

**Security note:**
- `network_magic` and `listen_port` have NO defaults
- Must be set explicitly based on chain type
- Prevents accidental mainnet/testnet/regtest confusion

---

## Component Files

### Core Components
- `network_manager.{hpp,cpp}` - Top-level coordinator
- `peer_lifecycle_manager.{hpp,cpp}` - Connection lifecycle
- `peer_discovery_manager.{hpp,cpp}` - Address discovery coordination
- `blockchain_sync_manager.{hpp,cpp}` - Header/block sync coordination

### Peer & Message Infrastructure
- `peer.{hpp,cpp}` - Per-connection state
- `message.{hpp,cpp}` - Message serialization/deserialization
- `message_dispatcher.{hpp,cpp}` - Handler registry pattern
- `protocol.{hpp,cpp}` - Protocol constants, magic bytes, commands

### Discovery Components
- `address_manager.{hpp,cpp}` - Bitcoin Core's address selection algorithm
- `anchor_manager.{hpp,cpp}` - Anchor persistence for eclipse resistance

### Sync Components
- `header_sync_manager.{hpp,cpp}` - GETHEADERS/HEADERS protocol, sync peer selection
- `block_relay_manager.{hpp,cpp}` - INV/GETDATA for blocks

### Transport Abstraction
- `transport.{hpp,cpp}` - Abstract interface for TCP vs simulated
- `real_transport.cpp` - Production TCP implementation
- `test/network/infra/network_bridged_transport.cpp` - Simulation implementation

### Testing Infrastructure
- `test/network/infra/simulated_network.{hpp,cpp}` - Deterministic network simulator
- `test/network/infra/simulated_node.{hpp,cpp}` - Uses REAL components + simulated transport
- `test/network/infra/attack_simulated_node.{hpp,cpp}` - Adversarial test harness
- `test/network/infra/test_orchestrator.{hpp,cpp}` - High-level test helpers

---

## Production Readiness Checklist

- [x] Exception safety (SaveAnchors/LoadAnchors wrapped in try/catch)
- [x] TOCTOU prevention (running_ re-checked before critical ops)
- [x] io_context lifetime safety (shared_ptr ownership)
- [x] Connection retry backoff (Failed() on banned/discouraged)
- [x] stop() blocking documented (may block if handlers slow)
- [x] Single-threaded reactor enforced (io_threads = 1)
- [x] Thread-safe start/stop (mutex-protected, idempotent)
- [x] Clean shutdown sequence (peers → transport → io_context)
- [x] Self-connection prevention (nonce checking)
- [x] DoS resistance (MAX_HEADERS_SIZE, continuous header validation, low-work rejection)
- [x] Eclipse attack resistance (anchors, feeler connections)
- [x] Bitcoin Core alignment (CConnman patterns, CAddrMan algorithm)

---

## Future Enhancements

### Potential Improvements (Not Currently Required)

1. **Multi-threaded reactor support**
   - Add strands to all timer/message handlers
   - Benchmark to verify performance improvement justifies complexity

2. **Connection pooling**
   - Reuse Peer objects for reconnections
   - Reduce allocation overhead

3. **BIP324 v2 P2P transport**
   - Encrypted connections
   - Authentication

4. **Tor/I2P support**
   - Anonymous routing
   - Privacy enhancement

5. **DNSSEC anchor bootstrapping**
   - Fetch initial peers via DNS
   - Reduce reliance on hardcoded seed nodes

---

## References

- **Bitcoin Core:** `src/net.cpp` (CConnman), `src/net_processing.cpp` (PeerManager)
- **BIP14:** User-agent format
- **BIP31:** PONG message
- **BIP37:** Bloom filters (not implemented)
- **BIP61:** Reject message (deprecated in Bitcoin Core)
- **BIP155:** addr v2 (IPv6, Tor, I2P support)

---

## Summary

The coinbasechain network layer is a production-ready, Bitcoin Core-inspired P2P stack with:

- **Clear separation of concerns:** 3-manager architecture (lifecycle, discovery, sync)
- **Thread safety:** Single-threaded reactor eliminates data races
- **Resource safety:** RAII, exception handling, shared ownership
- **Correctness:** TOCTOU prevention, atomic operations, clean shutdown
- **Testability:** Fully simulated network for deterministic testing
- **DoS resistance:** Message size limits, rate limiting, ban/discourage
- **Eclipse resistance:** Anchors, feeler connections, address diversity

The design prioritizes correctness and maintainability over premature optimization, following Bitcoin Core's proven patterns while using modern C++ idioms (smart pointers, RAII, type safety).
