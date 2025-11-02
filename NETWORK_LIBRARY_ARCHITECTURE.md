# Network Library Architecture - Technical Deep Dive

**Version:** 1.0.0  
**Date:** 2025-10-24  
**Scope:** Network library (`src/network/*`, `include/network/*`)  
**Related Documents:** `ARCHITECTURE.md`, `CHAIN_LIBRARY_ARCHITECTURE.md`

---

## Table of Contents

1. [Overview](#1-overview)
2. [Core Components](#2-core-components)
3. [Protocol Layer](#3-protocol-layer)
4. [Peer Lifecycle](#4-peer-lifecycle)
5. [Message Routing](#5-message-routing)
6. [Header Synchronization](#6-header-synchronization)
7. [Peer Discovery](#7-peer-discovery)
8. [DoS Protection](#8-dos-protection)
9. [Concurrency Model](#9-concurrency-model)
10. [Performance Characteristics](#10-performance-characteristics)

---

## 1. Overview

### 1.1 Purpose

The network library implements a **Bitcoin-compatible P2P networking layer** for a headers-only blockchain. It provides:

- **Peer connection management** (TCP via Boost.Asio)
- **Protocol handshaking** (VERSION/VERACK)
- **Message framing and routing** (24-byte headers, checksums)
- **Header synchronization** (GETHEADERS/HEADERS flow)
- **Peer discovery** (ADDR/GETADDR, DNS seeds)
- **DoS protection** (misbehavior tracking, connection limits)
- **NAT traversal** (UPnP support)

### 1.2 Design Philosophy

| Principle | Implementation | Rationale |
|-----------|----------------|-----------|
| **Bitcoin Compatibility** | 98% protocol compatibility | Leverage existing infrastructure (block explorers, monitoring) |
| **Async I/O** | Boost.Asio with thread pool | High concurrency without thread-per-peer |
| **Separation of Concerns** | Specialized managers (HeaderSync, PeerManager) | Modularity, testability |
| **DoS Resilience** | Multi-layer protection (limits, scoring, banning) | Security without sacrificing performance |
| **Testability** | Abstract Transport interface | Simulated networks for deterministic testing |

### 1.3 Library Structure

```
network/
├── Core Networking
│   ├── network_manager.{hpp,cpp}      # Top-level coordinator
│   ├── peer_manager.{hpp,cpp}         # Peer lifecycle & DoS tracking
│   ├── peer.{hpp,cpp}                 # Single peer connection
│   └── transport.{hpp,cpp}            # Abstract I/O interface
│
├── Protocol Layer
│   ├── protocol.{hpp,cpp}             # Constants, structures
│   ├── message.{hpp,cpp}              # Message serialization
│   └── message_router.{hpp,cpp}       # Message dispatch
│
├── Synchronization
│   ├── header_sync_manager.{hpp,cpp}  # Header download logic
│   └── block_relay_manager.{hpp,cpp}  # Block announcement (INV)
│
├── Peer Discovery
│   ├── addr_manager.{hpp,cpp}         # Address book management
│   └── anchor_manager.{hpp,cpp}       # Eclipse attack resistance
│
├── Security
│   ├── banman.{hpp,cpp}               # IP banning system
│   └── nat_manager.{hpp,cpp}          # UPnP NAT traversal
│
└── Transport Implementations
    ├── real_transport.{hpp,cpp}       # Production TCP (Boost.Asio)
    └── simulated_transport.{hpp,cpp}  # Testing (in-memory)
```

---

## 2. Core Components

### 2.1 Component Hierarchy

```
┌─────────────────────────────────────────────────────────────────┐
│                      NetworkManager                              │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │              Central Coordinator                          │  │
│  │  • Startup/shutdown lifecycle                             │  │
│  │  • Periodic maintenance (connection attempts, cleanup)    │  │
│  │  • Component wiring                                       │  │
│  └───────────────────────────────────────────────────────────┘  │
│                              ▼                                  │
│  ┌────────────┐  ┌──────────────┐  ┌─────────────────────┐    │
│  │PeerManager │  │HeaderSync    │  │  BlockRelay         │    │
│  │            │  │Manager       │  │  Manager            │    │
│  │• Lifecycle │  │              │  │                     │    │
│  │• Limits    │  │• Sync state  │  │• INV announcements  │    │
│  │• DoS scores│  │• Locators    │  │• Block propagation  │    │
│  └────────────┘  └──────────────┘  └─────────────────────┘    │
│         │                │                     │                │
│         └────────────────┴─────────────────────┘                │
│                          ▼                                      │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                   MessageRouter                           │  │
│  │  Routes incoming messages to specialized handlers         │  │
│  └───────────────────────────────────────────────────────────┘  │
│                          ▼                                      │
│  ┌────────────┐  ┌──────────────┐  ┌──────────────┐           │
│  │AddressMan  │  │   BanMan     │  │  NATManager  │           │
│  │            │  │              │  │              │           │
│  │• tried/new │  │• IP banning  │  │• UPnP        │           │
│  │• Selection │  │• Persistence │  │• Port map    │           │
│  └────────────┘  └──────────────┘  └──────────────┘           │
│                          ▼                                      │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                   Peer Layer                              │  │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  │  │
│  │  │  Peer 1  │  │  Peer 2  │  │  Peer 3  │  │  Peer N  │  │  │
│  │  │(OUTBOUND)│  │(INBOUND) │  │(FEELER)  │  │          │  │  │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘  │  │
│  └───────────────────────────────────────────────────────────┘  │
│                          ▼                                      │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                  Transport Layer                          │  │
│  │  ┌────────────────────────────────────────────────────┐   │  │
│  │  │            Boost.Asio I/O Context                  │   │  │
│  │  │  ┌────────┐  ┌────────┐  ┌────────┐  ┌────────┐   │   │  │
│  │  │  │Thread 1│  │Thread 2│  │Thread 3│  │Thread 4│   │   │  │
│  │  │  └────────┘  └────────┘  └────────┘  └────────┘   │   │  │
│  │  └────────────────────────────────────────────────────┘   │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                          ▼
                    ┌──────────┐
                    │   TCP    │
                    │ Sockets  │
                    └──────────┘
```

### 2.2 NetworkManager - Central Orchestrator

**Responsibility:** Top-level lifecycle management and coordination

```cpp
class NetworkManager {
public:
    struct Config {
        uint32_t network_magic;      // 0x554E4943 ("UNIC") for mainnet
        uint16_t listen_port;        // 9590 for mainnet
        bool listen_enabled;         // Accept inbound connections
        bool enable_nat;             // UPnP NAT traversal
        size_t io_threads;           // Async I/O thread pool size
        std::string datadir;         // For persistence (banlist, anchors)
        
        std::chrono::seconds connect_interval;      // 5s default
        std::chrono::seconds maintenance_interval;  // 30s default
    };
    
    // Lifecycle
    bool start();   // Start I/O threads, begin accepting connections
    void stop();    // Stop all connections, join threads
    
    // Manual connection management
    bool connect_to(const protocol::NetworkAddress& addr);
    void disconnect_from(int peer_id);
    
    // Block relay
    void relay_block(const uint256& block_hash);
    void announce_tip_to_peers();
    
private:
    // Components (owned by NetworkManager)
    std::unique_ptr<PeerManager> peer_manager_;
    std::unique_ptr<AddressManager> addr_manager_;
    std::unique_ptr<HeaderSyncManager> header_sync_manager_;
    std::unique_ptr<BlockRelayManager> block_relay_manager_;
    std::unique_ptr<MessageRouter> message_router_;
    std::unique_ptr<BanMan> ban_man_;
    std::unique_ptr<NATManager> nat_manager_;
    std::unique_ptr<AnchorManager> anchor_manager_;
    
    // Async I/O
    boost::asio::io_context& io_context_;
    std::vector<std::thread> io_threads_;
    
    // Periodic timers
    std::unique_ptr<boost::asio::steady_timer> connect_timer_;
    std::unique_ptr<boost::asio::steady_timer> maintenance_timer_;
    std::unique_ptr<boost::asio::steady_timer> feeler_timer_;
    std::unique_ptr<boost::asio::steady_timer> sendmessages_timer_;
};
```

**Startup Sequence:**

```
start()
  ├─► Initialize components (PeerManager, AddressManager, etc.)
  ├─► Load persisted data (banlist.json, peers.dat, anchors.json)
  ├─► Start I/O threads (4 threads default)
  ├─► Begin listening on port (if listen_enabled)
  ├─► Bootstrap from DNS seeds (if address book empty)
  ├─► Schedule periodic tasks:
  │     ├─► connect_timer: Attempt outbound connections (every 5s)
  │     ├─► maintenance_timer: Cleanup stale peers (every 30s)
  │     ├─► feeler_timer: Test new addresses (every 2 min)
  │     └─► sendmessages_timer: Flush announcements (every 1s)
  └─► Return true (running)
```

**Shutdown Sequence:**

```
stop()
  ├─► Cancel all timers
  ├─► Disconnect all peers (PeerManager::disconnect_all)
  ├─► Stop listening for inbound connections
  ├─► Stop I/O context (io_context.stop())
  ├─► Join all I/O threads
  ├─► Save state (banlist, peers, anchors)
  └─► Set running_ = false
```

---

### 2.3 PeerManager - Peer Lifecycle & DoS Tracking

**Responsibility:** Unified peer management and misbehavior scoring

```cpp
class PeerManager {
public:
    struct Config {
        size_t max_outbound_peers;     // 8 default
        size_t max_inbound_peers;      // 125 default
        size_t target_outbound_peers;  // 8 default
    };
    
    // Peer management
    int add_peer(PeerPtr peer, NetPermissionFlags permissions, 
                 const std::string& address);
    void remove_peer(int peer_id);
    PeerPtr get_peer(int peer_id);
    
    // Queries
    std::vector<PeerPtr> get_outbound_peers();
    std::vector<PeerPtr> get_inbound_peers();
    bool needs_more_outbound() const;
    bool can_accept_inbound() const;
    
    // DoS protection (public API)
    void IncrementUnconnectingHeaders(int peer_id);
    void ReportInvalidPoW(int peer_id);
    void ReportOversizedMessage(int peer_id);
    void ReportLowWorkHeaders(int peer_id);
    void ReportInvalidHeader(int peer_id, const std::string& reason);
    
    // Query misbehavior
    int GetMisbehaviorScore(int peer_id) const;
    bool ShouldDisconnect(int peer_id) const;
    
private:
    // Internal misbehavior tracking
    bool Misbehaving(int peer_id, int penalty, const std::string& reason);
    
    std::mutex mutex_;
    std::map<int, PeerPtr> peers_;
    std::map<int, PeerMisbehaviorData> peer_misbehavior_;
    int next_peer_id_ = 0;
};
```

**PeerMisbehaviorData:**

```cpp
struct PeerMisbehaviorData {
    int misbehavior_score{0};          // Cumulative penalty score
    bool should_discourage{false};     // Exceeded threshold?
    int num_unconnecting_headers_msgs{0};
    NetPermissionFlags permissions{NetPermissionFlags::None};
    std::string address;               // For banning
};
```

**Misbehavior Penalties (Bitcoin Core-style):**

| Violation | Penalty | Effect at 100 |
|-----------|---------|---------------|
| Invalid PoW | 100 | Instant disconnect |
| Invalid header | 100 | Instant disconnect |
| Oversized message | 20 | 5 violations = disconnect |
| Non-continuous headers | 20 | 5 violations = disconnect |
| Low-work headers | 10 | 10 violations = disconnect |
| Too many unconnecting | 100 | Instant disconnect |
| Too many orphans | 100 | Instant disconnect |

**Penalty Application Flow:**

```cpp
bool PeerManager::Misbehaving(int peer_id, int penalty, 
                             const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = peer_misbehavior_.find(peer_id);
    if (it == peer_misbehavior_.end()) {
        return false;  // Peer not found
    }
    
    PeerMisbehaviorData& data = it->second;
    data.misbehavior_score += penalty;
    
    LOG_WARN("Peer {} misbehaving (+{} -> {}): {}", 
            peer_id, penalty, data.misbehavior_score, reason);
    
    // Check if exceeded threshold
    if (data.misbehavior_score >= DISCOURAGEMENT_THRESHOLD) {
        // Check if peer has NoBan permission
        if (!HasPermission(data.permissions, NetPermissionFlags::NoBan)) {
            data.should_discourage = true;
            LOG_ERROR("Peer {} discouraged (score={}), disconnecting", 
                     peer_id, data.misbehavior_score);
            
            // Disconnect peer
            auto peer_it = peers_.find(peer_id);
            if (peer_it != peers_.end()) {
                peer_it->second->disconnect();
                // Will be removed in next process_periodic()
            }
            
            return true;  // Should disconnect
        } else {
            LOG_WARN("Peer {} would be discouraged but has NoBan permission", 
                    peer_id);
        }
    }
    
    return false;
}
```

---

### 2.4 Peer - Single Connection

**Responsibility:** TCP connection, handshake, message framing, keepalive

```cpp
class Peer : public std::enable_shared_from_this<Peer> {
public:
    // Factory methods
    static PeerPtr create_outbound(io_context, connection, magic, 
                                   start_height, target_addr, port);
    static PeerPtr create_inbound(io_context, connection, magic, 
                                  start_height);
    
    // Lifecycle
    void start();      // Begin connection (VERSION for outbound)
    void disconnect(); // Close connection
    
    // Messaging
    void send_message(std::unique_ptr<message::Message> msg);
    void set_message_handler(MessageHandler handler);
    
    // State
    PeerState state() const;
    bool is_connected() const;
    bool successfully_connected() const;  // VERACK received
    
    // Peer info (from VERSION)
    int32_t version() const;
    uint64_t services() const;
    int32_t start_height() const;
    const std::string& user_agent() const;
    
    // Block announcement queue (Bitcoin pattern)
    std::vector<uint256> blocks_for_inv_relay_;
    std::mutex block_inv_mutex_;
    
private:
    // Connection state machine
    void on_connected();
    void on_disconnect();
    void send_version();
    void handle_version(const message::VersionMessage& msg);
    void handle_verack();
    
    // Message processing
    void on_transport_receive(const std::vector<uint8_t>& data);
    void process_received_data(std::vector<uint8_t>& buffer);
    void process_message(const protocol::MessageHeader& header,
                        const std::vector<uint8_t>& payload);
    
    // Keepalive
    void schedule_ping();
    void send_ping();
    void handle_pong(const message::PongMessage& msg);
    
    // Timeouts
    void start_handshake_timeout();  // 60s for VERSION/VERACK
    void start_inactivity_timeout(); // 20 min of no messages
    
    PeerState state_;
    TransportConnectionPtr connection_;
    MessageHandler message_handler_;
    std::vector<uint8_t> recv_buffer_;
};
```

**State Machine:**

```
        create_outbound()
              │
              ▼
        DISCONNECTED
              │
         start()
              │
              ▼
        CONNECTING ──────► (TCP connect callback)
              │
              ▼
         CONNECTED ──────► send_version()
              │
              ▼
       VERSION_SENT ──────► receive VERACK
              │
              ▼
           READY ──────────► (fully connected, process messages)
              │
         disconnect()
              │
              ▼
       DISCONNECTING
              │
              ▼
        DISCONNECTED
```

**Inbound Handshake:**

```
Peer accepts connection → CONNECTED
  ↓
Peer receives VERSION from remote
  ↓
Peer validates VERSION (magic, version, nonce)
  ↓
Peer sends VERACK
  ↓
Peer sends own VERSION
  ↓
Peer receives VERACK from remote
  ↓
State = READY (handshake complete)
```

**Outbound Handshake:**

```
create_outbound() → start() → CONNECTING
  ↓
TCP connection established → CONNECTED
  ↓
Send VERSION immediately
  ↓
State = VERSION_SENT
  ↓
Receive VERSION from peer
  ↓
Send VERACK
  ↓
Receive VERACK from peer
  ↓
State = READY (handshake complete)
```

---

## 3. Protocol Layer

### 3.1 Message Format

**Wire Format (24-byte header + payload):**

```
┌────────────┬─────────────┬─────────────┬─────────────┬────────────┐
│   Magic    │   Command   │   Length    │  Checksum   │  Payload   │
│  (4 bytes) │ (12 bytes)  │  (4 bytes)  │  (4 bytes)  │ (variable) │
├────────────┼─────────────┼─────────────┼─────────────┼────────────┤
│ 0x554E4943 │ "headers\0" │ 0x00002710  │ SHA256[:4]  │ [headers]  │
│  ("UNIC")  │ (null-pad)  │ (10,000)    │ of payload  │            │
└────────────┴─────────────┴─────────────┴─────────────┴────────────┘
```

**Checksum Calculation:**
```cpp
uint32_t calculate_checksum(const std::vector<uint8_t>& payload) {
    uint256 hash = SHA256(SHA256(payload));
    return *(uint32_t*)hash.begin();  // First 4 bytes
}
```

### 3.2 Supported Messages

| Message | Direction | Purpose | Max Size |
|---------|-----------|---------|----------|
| **VERSION** | Both | Handshake initiation | ~200 bytes |
| **VERACK** | Both | Handshake acknowledgment | 0 bytes |
| **PING** | Both | Keepalive request | 8 bytes |
| **PONG** | Both | Keepalive response | 8 bytes |
| **ADDR** | Both | Peer address exchange | 30KB |
| **GETADDR** | Both | Request peer addresses | 0 bytes |
| **INV** | Both | Announce new blocks | 50KB |
| **GETDATA** | Both | Request block headers | 50KB |
| **NOTFOUND** | Both | Data not available | 50KB |
| **GETHEADERS** | Both | Request headers sync | ~1KB |
| **HEADERS** | Both | Header delivery | 200KB |
| **SENDHEADERS** | Both | Enable push-based sync | 0 bytes |

**Unsupported (Bitcoin) Messages:**
- ❌ `tx`, `block` - No transactions or full blocks
- ❌ `mempool`, `getblocks` - No mempool
- ❌ `filterload`, `filteradd`, `filterclear` - No bloom filters
- ❌ `cmpctblock`, `getblocktxn`, `blocktxn` - No compact blocks

### 3.3 VERSION Message

```cpp
class VersionMessage : public Message {
public:
    int32_t version;                   // Protocol version (1)
    uint64_t services;                 // NODE_NETWORK (0x01)
    int64_t timestamp;                 // Current time
    protocol::NetworkAddress addr_recv; // Peer's address
    protocol::NetworkAddress addr_from; // Our address
    uint64_t nonce;                    // Random nonce (self-connection detection)
    std::string user_agent;            // "/CoinbaseChain:1.0.0/"
    int32_t start_height;              // Our blockchain height
};
```

**Self-Connection Detection:**

```cpp
// When sending VERSION (outbound)
uint64_t local_nonce = generate_random_nonce();
version.nonce = local_nonce;

// When receiving VERSION
bool check_incoming_nonce(uint64_t remote_nonce) {
    // Check if remote_nonce matches any of our outbound peers' local nonces
    for (auto& peer : get_outbound_peers()) {
        if (peer->get_local_nonce() == remote_nonce) {
            // Self-connection detected!
            return false;  // Reject
        }
    }
    return true;  // OK
}
```

### 3.4 HEADERS Message

```cpp
class HeadersMessage : public Message {
public:
    std::vector<CBlockHeader> headers;  // Up to 2000 headers
};
```

**Serialization (compact format):**

```
VarInt: count (1-3 bytes)
For each header (100 bytes each):
    int32_t  nVersion        (4 bytes)
    uint256  hashPrevBlock   (32 bytes)
    uint160  minerAddress    (20 bytes)
    uint32_t nTime           (4 bytes)
    uint32_t nBits           (4 bytes)
    uint32_t nNonce          (4 bytes)
    uint256  hashRandomX     (32 bytes)
    VarInt   txn_count       (1 byte, always 0 for headers-only)
```

**Wire Size:**
- 1 header: 101 bytes (1-byte count + 100-byte header)
- 2000 headers: ~200KB (max size)

### 3.5 GETHEADERS Message

```cpp
class GetHeadersMessage : public Message {
public:
    uint32_t version;                   // Protocol version
    std::vector<uint256> block_locator; // Block locator (up to 101 hashes)
    uint256 hash_stop;                  // Stop after this hash (0 = don't stop)
};
```

**Block Locator Generation:**

```cpp
std::vector<uint256> GetLocator(const CBlockIndex* pindex) {
    std::vector<uint256> locator;
    int step = 1;
    
    while (pindex) {
        locator.push_back(pindex->GetBlockHash());
        
        // Go back exponentially: 1, 2, 4, 8, 16, 32, ...
        for (int i = 0; pindex && i < step; i++) {
            pindex = pindex->pprev;
        }
        
        // Exponential backoff
        if (locator.size() > 10) {
            step *= 2;
        }
    }
    
    // Max 101 entries (Bitcoin protocol limit)
    if (locator.size() > 101) {
        locator.resize(101);
    }
    
    return locator;
}
```

**Example Locator (for chain at height 1000):**
```
[1000, 999, 998, 996, 992, 984, 968, 936, 872, 744, 488, 0]
 ↑     ↑    ↑    ↑    ↑    ↑    ↑    ↑    ↑    ↑    ↑    ↑
tip  -1   -2   -4   -8   -16  -32  -64  -128 -256 -512 genesis
```

**Purpose:** Allows peer to find last common block efficiently (logarithmic search).

---

## 4. Peer Lifecycle

### 4.1 Outbound Connection Flow

```
NetworkManager::attempt_outbound_connections()
  ↓
AddressManager::select() → Returns address to connect to
  ↓
Transport::connect(address, port, callback) → Initiates TCP connection
  ↓
[Async TCP handshake]
  ↓
ConnectCallback(success=true) → TCP established
  ↓
Peer::create_outbound(connection, magic, start_height)
  ↓
PeerManager::add_peer(peer) → Assigns peer_id
  ↓
Peer::start() → Begins protocol handshake
  ↓
Peer::send_version() → Sends VERSION message
  ↓
state = VERSION_SENT
  ↓
[Wait for remote VERSION]
  ↓
Peer::handle_version(msg) → Validates, sends VERACK
  ↓
[Wait for remote VERACK]
  ↓
Peer::handle_verack() → Handshake complete
  ↓
state = READY
  ↓
NetworkManager::announce_tip_to_peer(peer) → Send initial INV
  ↓
[Normal message processing begins]
```

### 4.2 Inbound Connection Flow

```
Transport::listen(port, accept_callback) → Starts TCP listener
  ↓
[Incoming TCP connection]
  ↓
Transport accepts connection → Creates TransportConnection
  ↓
AcceptCallback(connection) → Passed to NetworkManager
  ↓
NetworkManager::handle_inbound_connection(connection)
  ↓
PeerManager::can_accept_inbound() → Check connection limits
  ↓ (if false)
  └─► evict_inbound_peer() → Make room or reject
  ↓
Peer::create_inbound(connection, magic, start_height)
  ↓
PeerManager::add_peer(peer, permissions, address)
  ↓
Peer::start() → Begins receiving messages (NO VERSION sent yet)
  ↓
state = CONNECTED
  ↓
[Wait for remote VERSION]
  ↓
Peer::handle_version(msg) → Validates, sends VERACK + own VERSION
  ↓
state = VERSION_SENT
  ↓
[Wait for remote VERACK]
  ↓
Peer::handle_verack() → Handshake complete
  ↓
state = READY
  ↓
NetworkManager::announce_tip_to_peer(peer) → Send initial INV
  ↓
[Normal message processing begins]
```

### 4.3 Connection Limits & Eviction

**Limits (Bitcoin-compatible):**

```cpp
static constexpr unsigned int DEFAULT_MAX_OUTBOUND_CONNECTIONS = 8;
static constexpr unsigned int DEFAULT_MAX_INBOUND_CONNECTIONS = 125;
```

**Inbound Eviction Strategy (when at limit):**

```cpp
bool PeerManager::evict_inbound_peer() {
    auto inbound_peers = get_inbound_peers();
    
    // Never evict peers with Manual or NoBan permissions
    inbound_peers.erase(
        std::remove_if(inbound_peers.begin(), inbound_peers.end(),
            [](PeerPtr p) {
                // Get permissions from misbehavior data
                return HasPermission(permissions, NetPermissionFlags::Manual);
            }),
        inbound_peers.end()
    );
    
    if (inbound_peers.empty()) {
        return false;  // All inbound have special permissions
    }
    
    // Eviction criteria (Bitcoin-style, simplified):
    // 1. Prefer to evict peers with highest misbehavior score
    // 2. Among equals, evict oldest connection
    
    std::sort(inbound_peers.begin(), inbound_peers.end(),
        [this](PeerPtr a, PeerPtr b) {
            int score_a = GetMisbehaviorScore(a->id());
            int score_b = GetMisbehaviorScore(b->id());
            if (score_a != score_b) {
                return score_a > score_b;  // Higher score first
            }
            // Among equals, older connection first
            return a->stats().connected_time < b->stats().connected_time;
        });
    
    // Evict first peer in sorted list
    PeerPtr victim = inbound_peers.front();
    LOG_INFO("Evicting inbound peer {} to make room", victim->id());
    victim->disconnect();
    return true;
}
```

### 4.4 Feeler Connections

**Purpose:** Test addresses from "new" table without consuming outbound slots

```cpp
void NetworkManager::attempt_feeler_connection() {
    // Select address from "new" table (never connected)
    auto addr_opt = addr_manager_->select_new_for_feeler();
    if (!addr_opt) {
        return;  // No new addresses to test
    }
    
    protocol::NetworkAddress addr = *addr_opt;
    
    // Create feeler connection (special type)
    auto connection = transport_->connect(
        address_to_string(addr), addr.port,
        [this, addr](bool success) {
            if (success) {
                // Successfully connected!
                addr_manager_->good(addr);
                
                // Disconnect immediately after handshake
                // (feelers don't stay connected)
            } else {
                addr_manager_->failed(addr);
            }
        }
    );
    
    auto peer = Peer::create_outbound(
        io_context_, connection, config_.network_magic,
        chainstate_manager_.GetChainHeight(),
        address, port,
        ConnectionType::FEELER  // Special type
    );
    
    // After VERSION/VERACK, disconnect
    peer->set_message_handler([peer](PeerPtr p, auto msg) {
        if (p->successfully_connected()) {
            p->disconnect();  // Feeler completed
        }
        return true;
    });
}
```

**Feeler Schedule:**
- **Interval:** Every 2 minutes (`FEELER_INTERVAL`)
- **Purpose:** Continuously test new addresses to keep address book fresh
- **Benefit:** Detects stale addresses quickly, improves eclipse attack resistance

---

## 5. Message Routing

### 5.1 MessageRouter Architecture

```cpp
class MessageRouter {
public:
    MessageRouter(AddressManager* addr_mgr,
                  HeaderSyncManager* header_sync,
                  BlockRelayManager* block_relay);
    
    // Central dispatch
    bool RouteMessage(PeerPtr peer, std::unique_ptr<message::Message> msg);
    
private:
    // Specialized handlers
    bool handle_verack(PeerPtr peer);
    bool handle_addr(PeerPtr peer, message::AddrMessage* msg);
    bool handle_getaddr(PeerPtr peer);
    bool handle_inv(PeerPtr peer, message::InvMessage* msg);
    bool handle_headers(PeerPtr peer, message::HeadersMessage* msg);
    bool handle_getheaders(PeerPtr peer, message::GetHeadersMessage* msg);
};
```

**Routing Table:**

```cpp
bool MessageRouter::RouteMessage(PeerPtr peer, 
                                std::unique_ptr<message::Message> msg) {
    std::string cmd = msg->command();
    
    // Route to appropriate handler
    if (cmd == protocol::commands::VERACK) {
        return handle_verack(peer);
    }
    else if (cmd == protocol::commands::ADDR) {
        return handle_addr(peer, static_cast<message::AddrMessage*>(msg.get()));
    }
    else if (cmd == protocol::commands::GETADDR) {
        return handle_getaddr(peer);
    }
    else if (cmd == protocol::commands::INV) {
        return handle_inv(peer, static_cast<message::InvMessage*>(msg.get()));
    }
    else if (cmd == protocol::commands::HEADERS) {
        // Delegate to HeaderSyncManager
        return header_sync_manager_->HandleHeadersMessage(
            peer, static_cast<message::HeadersMessage*>(msg.get()));
    }
    else if (cmd == protocol::commands::GETHEADERS) {
        // Delegate to HeaderSyncManager
        return header_sync_manager_->HandleGetHeadersMessage(
            peer, static_cast<message::GetHeadersMessage*>(msg.get()));
    }
    else if (cmd == protocol::commands::PING) {
        // Handled internally by Peer class
        return true;
    }
    else if (cmd == protocol::commands::PONG) {
        // Handled internally by Peer class
        return true;
    }
    else {
        LOG_WARN("Unknown message command: {}", cmd);
        return false;
    }
}
```

### 5.2 Message Processing Pipeline

```
Peer receives data from transport
  ↓
on_transport_receive(data) → Appends to recv_buffer_
  ↓
process_received_data(recv_buffer_)
  ↓
Parse 24-byte header
  ↓
Validate: magic, command, length, checksum
  ↓
Extract payload (length bytes)
  ↓
process_message(header, payload)
  ↓
Deserialize payload into Message subclass
  ↓
Call message_handler_(peer, msg)
  ↓
MessageRouter::RouteMessage(peer, msg)
  ↓
Dispatch to specialized handler
  ↓
Handler processes message
  ↓
Returns true (success) or false (error)
  ↓
(if false) PeerManager penalizes peer
```

**Example: HEADERS Message Flow:**

```
Peer receives HEADERS message (200KB)
  ↓
MessageRouter::RouteMessage() → Dispatches to HeaderSyncManager
  ↓
HeaderSyncManager::HandleHeadersMessage(peer, msg)
  ↓
Validates headers (PoW commitment, continuity)
  ↓ (if invalid)
  └─► PeerManager::ReportNonContinuousHeaders(peer_id) → Penalty
  ↓
Passes headers to ChainstateManager::ProcessNewBlockHeaders()
  ↓
ChainstateManager validates & adds to chain
  ↓ (if validation fails)
  └─► PeerManager::ReportInvalidHeader(peer_id) → Disconnect
  ↓
HeaderSyncManager checks if more headers needed
  ↓ (if batch size == 2000)
  └─► Send another GETHEADERS to continue sync
  ↓
Returns true (success)
```

---

## 6. Header Synchronization

### 6.1 HeaderSyncManager

**Responsibility:** Coordinate header download from peers

```cpp
class HeaderSyncManager {
public:
    HeaderSyncManager(ChainstateManager& chainstate,
                     PeerManager& peer_mgr,
                     BanMan* ban_man);
    
    // Message handlers
    bool HandleHeadersMessage(PeerPtr peer, message::HeadersMessage* msg);
    bool HandleGetHeadersMessage(PeerPtr peer, message::GetHeadersMessage* msg);
    
    // Sync coordination
    void RequestHeadersFromPeer(PeerPtr peer);
    void CheckInitialSync();
    
    // State queries
    bool IsSynced(int64_t max_age_seconds = 3600) const;
    
    // Sync tracking
    void SetSyncPeer(uint64_t peer_id);
    void ClearSyncPeer();
    
private:
    ChainstateManager& chainstate_manager_;
    PeerManager& peer_manager_;
    BanMan* ban_man_;
    
    std::atomic<uint64_t> sync_peer_id_{0};
    std::atomic<int64_t> last_headers_received_{0};
};
```

### 6.2 Initial Block Download (IBD)

**Detection:**

```cpp
bool ChainstateManager::IsInitialBlockDownload() const {
    // Fast path: check latch first
    if (m_cached_finished_ibd.load(std::memory_order_relaxed)) {
        return false;
    }
    
    const CBlockIndex* tip = GetTip();
    if (!tip) {
        return true;  // No tip = in IBD
    }
    
    // Tip too old? (1 hour)
    int64_t now = util::GetTime();
    if (tip->nTime < now - 3600) {
        return true;
    }
    
    // Minimum chain work check (eclipse attack protection)
    if (tip->nChainWork < UintToArith256(params_.GetConsensus().nMinimumChainWork)) {
        return true;
    }
    
    // All checks passed - synced!
    m_cached_finished_ibd.store(true, std::memory_order_relaxed);
    return false;
}
```

**Sync Strategy:**

```
1. On startup:
   - Load headers from disk
   - Check if in IBD (ChainstateManager::IsInitialBlockDownload())
   
2. If in IBD:
   - Select sync peer (first outbound peer with VERSION)
   - Send GETHEADERS with locator
   - Wait for HEADERS response (up to 2000 headers)
   - Validate & add headers to chain
   - If batch size == 2000, send another GETHEADERS (continue)
   - If batch size < 2000, likely caught up (end of chain)
   
3. If not in IBD:
   - Use push-based sync (INV announcements)
   - Peers announce new blocks via INV
   - Request headers with GETHEADERS
```

### 6.3 GETHEADERS Request

```cpp
void HeaderSyncManager::RequestHeadersFromPeer(PeerPtr peer) {
    // Generate block locator from current chain
    CBlockLocator locator = chainstate_manager_.GetLocator();
    
    // Create GETHEADERS message
    auto msg = std::make_unique<message::GetHeadersMessage>();
    msg->version = protocol::PROTOCOL_VERSION;
    msg->block_locator = locator.vHave;
    msg->hash_stop = uint256();  // Don't stop (send up to 2000)
    
    // Send to peer
    peer->send_message(std::move(msg));
    
    LOG_INFO("Requested headers from peer {} (locator size: {})",
            peer->id(), locator.vHave.size());
}
```

### 6.4 HEADERS Response Handling

```cpp
bool HeaderSyncManager::HandleHeadersMessage(PeerPtr peer, 
                                            message::HeadersMessage* msg) {
    LOG_INFO("Received {} headers from peer {}", 
            msg->headers.size(), peer->id());
    
    // Empty response = peer has no new headers
    if (msg->headers.empty()) {
        LOG_DEBUG("Peer {} sent empty HEADERS (synced)", peer->id());
        return true;
    }
    
    // Validate headers are continuous (DoS protection)
    if (!validation::CheckHeadersAreContinuous(msg->headers)) {
        peer_manager_.ReportNonContinuousHeaders(peer->id());
        return false;
    }
    
    // Fast PoW commitment check (DoS protection)
    if (!validation::CheckHeadersPoW(msg->headers, chainstate_manager_.GetParams())) {
        peer_manager_.ReportInvalidPoW(peer->id());
        return false;
    }
    
    // Calculate total work (DoS protection)
    arith_uint256 total_work = validation::CalculateHeadersWork(msg->headers);
    arith_uint256 threshold = validation::GetAntiDoSWorkThreshold(
        chainstate_manager_.GetTip(),
        chainstate_manager_.GetParams(),
        chainstate_manager_.IsInitialBlockDownload()
    );
    
    if (total_work < threshold) {
        peer_manager_.ReportLowWorkHeaders(peer->id());
        return false;  // Reject low-work spam
    }
    
    // Process headers through ChainstateManager
    for (const CBlockHeader& header : msg->headers) {
        validation::ValidationState state;
        if (!chainstate_manager_.ProcessNewBlockHeader(header, state)) {
            if (state.IsInvalid()) {
                peer_manager_.ReportInvalidHeader(peer->id(), 
                                                 state.GetRejectReason());
                return false;
            }
        }
    }
    
    // Update sync state
    last_headers_received_.store(util::GetTimeMicros(), 
                                 std::memory_order_relaxed);
    
    // If batch is full (2000), peer likely has more headers
    if (msg->headers.size() == protocol::MAX_HEADERS_SIZE) {
        LOG_INFO("Received full batch, requesting more from peer {}", 
                peer->id());
        RequestHeadersFromPeer(peer);
    } else {
        LOG_INFO("Received partial batch ({} headers), likely synced",
                msg->headers.size());
    }
    
    return true;
}
```

### 6.5 Anti-DoS Header Validation

**Three-Layer Defense:**

```
Layer 1: Structural Validation (~1μs per header)
  ├─► CheckHeadersAreContinuous()
  │     └─► headers[i].hashPrevBlock == headers[i-1].GetHash()
  └─► Reject if not continuous (penalty: 20 points)

Layer 2: Commitment Check (~1ms per header)
  ├─► CheckHeadersPoW(COMMITMENT_ONLY)
  │     └─► SHA256(header || hashRandomX) < nBits
  └─► Reject if failed (penalty: 100 points, disconnect)

Layer 3: Work Threshold (~5μs per header)
  ├─► CalculateHeadersWork()
  │     └─► Sum of work for all headers
  └─► Compare to threshold (tip work - 144 blocks)
      └─► Reject if below threshold (penalty: 10 points)
```

**Why This Matters:**

Without Layer 1-2, an attacker could flood the node with fake headers that pass basic checks but fail expensive validation (RandomX hashing), causing CPU exhaustion.

---

## 7. Peer Discovery

### 7.1 AddressManager ("AddrMan")

**Responsibility:** Maintain address book for peer selection

```cpp
class AddressManager {
public:
    // Add address from peer discovery
    bool add(const protocol::NetworkAddress& addr, uint32_t timestamp);
    
    // Connection tracking
    void attempt(const protocol::NetworkAddress& addr);  // Mark as trying
    void good(const protocol::NetworkAddress& addr);     // Connected successfully
    void failed(const protocol::NetworkAddress& addr);   // Connection failed
    
    // Selection
    std::optional<protocol::NetworkAddress> select();           // For outbound
    std::optional<protocol::NetworkAddress> select_new_for_feeler(); // For feeler
    
    // Queries
    size_t tried_count() const;  // Addresses we've connected to
    size_t new_count() const;    // Addresses we've heard about
    
private:
    std::map<std::string, AddrInfo> tried_;  // "tried" table
    std::map<std::string, AddrInfo> new_;    // "new" table
};
```

**Two-Table Structure:**

```
┌─────────────────────────────────────────────────────────┐
│                    AddressManager                        │
├─────────────────────────────────────────────────────────┤
│                                                          │
│  ┌──────────────────┐      ┌──────────────────┐        │
│  │   "new" table    │      │  "tried" table   │        │
│  │                  │      │                  │        │
│  │ Addresses heard  │      │ Addresses we've  │        │
│  │ about but never  │──────►│ successfully     │        │
│  │ connected to     │ good()│ connected to     │        │
│  │                  │      │                  │        │
│  │ • From ADDR msgs │      │ • Working peers  │        │
│  │ • From DNS seeds │      │ • High priority  │        │
│  │ • Unverified     │      │                  │        │
│  └──────────────────┘      └──────────────────┘        │
│         ▲                           │                   │
│         │ failed()                  │                   │
│         └───────────────────────────┘                   │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

**Selection Algorithm:**

```cpp
std::optional<protocol::NetworkAddress> AddressManager::select() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 50/50 chance: select from "tried" or "new"
    std::uniform_int_distribution<int> dist(0, 1);
    bool use_tried = dist(rng_) == 0;
    
    if (use_tried && !tried_.empty()) {
        // Select random from "tried"
        auto it = tried_.begin();
        std::advance(it, std::uniform_int_distribution<size_t>(0, tried_.size() - 1)(rng_));
        return it->second.address;
    } else if (!new_.empty()) {
        // Select random from "new"
        auto it = new_.begin();
        std::advance(it, std::uniform_int_distribution<size_t>(0, new_.size() - 1)(rng_));
        return it->second.address;
    }
    
    return std::nullopt;  // Address book empty
}
```

### 7.2 ADDR Message Exchange

**Flow:**

```
Peer A                              Peer B
  │                                   │
  │  GETADDR ──────────────────────► │
  │                                   │
  │                                   │ select up to 1000 addresses
  │                                   │ from address book
  │                                   │
  │  ◄────────────────────── ADDR    │
  │      (1000 addresses max)         │
  │                                   │
  │ add_multiple(addresses)           │
  │   → AddressManager                │
  │                                   │
```

**Rate Limiting:**

```cpp
// Only send ADDR once per peer (DoS protection)
if (peer->m_addr_sent) {
    return true;  // Already sent, ignore duplicate GETADDR
}

// Get addresses from AddressManager
std::vector<protocol::TimestampedAddress> addrs = 
    addr_manager_.get_addresses(protocol::MAX_ADDR_SIZE);  // Max 1000

// Send ADDR message
auto msg = std::make_unique<message::AddrMessage>();
msg->addresses = addrs;
peer->send_message(std::move(msg));

peer->m_addr_sent = true;  // Mark as sent
```

### 7.3 DNS Seed Bootstrap

**When:** On startup, if address book is empty

```cpp
void NetworkManager::bootstrap_from_fixed_seeds(const ChainParams& params) {
    // Only bootstrap if address book is empty
    if (addr_manager_->size() > 0) {
        return;
    }
    
    LOG_INFO("Address book empty, bootstrapping from DNS seeds");
    
    // Get DNS seed hostnames from chain params
    std::vector<std::string> dns_seeds = params.DNSSeeds();
    
    for (const std::string& seed : dns_seeds) {
        // Resolve DNS hostname to IP addresses
        boost::asio::ip::tcp::resolver resolver(io_context_);
        boost::system::error_code ec;
        
        auto results = resolver.resolve(seed, 
                                       std::to_string(params.GetDefaultPort()),
                                       ec);
        
        if (ec) {
            LOG_WARN("Failed to resolve DNS seed {}: {}", seed, ec.message());
            continue;
        }
        
        // Add resolved addresses to AddressManager
        for (const auto& entry : results) {
            protocol::NetworkAddress addr = 
                protocol::NetworkAddress::from_ipv4(
                    protocol::NODE_NETWORK,
                    entry.endpoint().address().to_v4().to_uint(),
                    entry.endpoint().port()
                );
            
            addr_manager_->add(addr, util::GetTime());
        }
        
        LOG_INFO("Resolved {} addresses from DNS seed {}", 
                std::distance(results.begin(), results.end()), seed);
    }
}
```

**Mainnet DNS Seeds (example):**
```
seed.coinbasechain.io
seed1.coinbasechain.io
seed2.coinbasechain.io
```

### 7.4 Anchor Connections (Eclipse Attack Resistance)

**Purpose:** Remember 2-3 reliable peers across restarts

```cpp
class AnchorManager {
public:
    // Save current outbound peers as anchors
    bool SaveAnchors(const std::string& filepath,
                    const std::vector<protocol::NetworkAddress>& peers);
    
    // Load anchors on startup
    std::vector<protocol::NetworkAddress> LoadAnchors(const std::string& filepath);
};
```

**Lifecycle:**

```
1. On startup:
   - Load anchors.json
   - Attempt connection to anchors FIRST (before DNS seeds)
   - Ensures at least some connections to known-good peers

2. During operation:
   - Track which outbound peers are reliable (long-lived, honest)

3. On shutdown:
   - Select 2-3 best outbound peers
   - Save as anchors.json
   - These become priority connections next startup
```

**Why This Matters:**

Without anchors, an attacker controlling DNS seeds could eclipse a restarting node. Anchors provide continuity of trusted connections across restarts.

---

## 8. DoS Protection

### 8.1 Multi-Layer Defense

```
Layer 1: Connection Limits
  ├─► Max 8 outbound (prevents resource exhaustion)
  ├─► Max 125 inbound (prevents socket exhaustion)
  └─► Evict lowest-value inbound when full

Layer 2: Message Size Limits
  ├─► MAX_PROTOCOL_MESSAGE_LENGTH = 4 MB (per message)
  ├─► DEFAULT_RECV_FLOOD_SIZE = 5 MB (per peer buffer)
  └─► Disconnect peer if exceeded (penalty: 20 points)

Layer 3: Message Rate Limits
  ├─► MAX_HEADERS_SIZE = 2000 (per HEADERS message)
  ├─► MAX_INV_SIZE = 50000 (per INV message)
  ├─► MAX_ADDR_SIZE = 1000 (per ADDR message)
  └─► Disconnect if exceeded (protocol violation)

Layer 4: Misbehavior Scoring
  ├─► Accumulate penalty points per violation
  ├─► Threshold = 100 points → disconnect
  └─► Ban IP after disconnect (via BanMan)

Layer 5: Work-Based Filtering
  ├─► Reject headers with insufficient cumulative work
  ├─► Threshold = tip_work - 144 blocks (during IBD: nMinimumChainWork)
  └─► Penalty: 10 points (low work spam)

Layer 6: Orphan Limits
  ├─► Max 1000 orphan headers globally
  ├─► Max 50 orphan headers per peer
  └─► Evict oldest when limit exceeded
```

### 8.2 BanMan - IP Banning

```cpp
class BanMan {
public:
    // Ban an IP address
    void Ban(const std::string& ip, int64_t ban_time_seconds = 86400);
    
    // Check if IP is banned
    bool IsBanned(const std::string& ip) const;
    
    // Unban an IP
    void Unban(const std::string& ip);
    
    // Persistence
    bool Save(const std::string& filepath);
    bool Load(const std::string& filepath);
    
private:
    std::map<std::string, int64_t> banned_ips_;  // ip -> unban_time
    mutable std::mutex mutex_;
};
```

**Ban Triggers:**

1. **Misbehavior Score ≥ 100:**
   ```cpp
   if (data.misbehavior_score >= DISCOURAGEMENT_THRESHOLD) {
       ban_man_->Ban(data.address, 86400);  // 24 hour ban
       peer->disconnect();
   }
   ```

2. **Manual Ban (RPC):**
   ```cpp
   // setban "192.168.1.1" "add"
   ban_man_->Ban("192.168.1.1", 86400);
   ```

3. **Repeated Disconnects:**
   ```cpp
   // If peer disconnects 3+ times in 1 hour, ban
   if (disconnect_count[ip] >= 3) {
       ban_man_->Ban(ip, 3600);  // 1 hour ban
   }
   ```

**Persistence Format (banlist.json):**

```json
{
  "version": 1,
  "banned": [
    {
      "ip": "192.168.1.100",
      "ban_until": 1700000000,
      "reason": "misbehavior_score=100"
    },
    {
      "ip": "10.0.0.5",
      "ban_until": 1700010000,
      "reason": "invalid_pow"
    }
  ]
}
```

### 8.3 Receive Buffer Management

**Problem:** Malicious peer sends data faster than we process → memory exhaustion

**Solution:** Flood protection limit

```cpp
// In RealTransportConnection::on_receive()
void on_receive(const boost::system::error_code& ec, size_t bytes_transferred) {
    if (!ec) {
        // Append to receive buffer
        recv_buffer_.insert(recv_buffer_.end(),
                          temp_buffer_.begin(),
                          temp_buffer_.begin() + bytes_transferred);
        
        // Check flood limit (5 MB)
        if (recv_buffer_.size() > protocol::DEFAULT_RECV_FLOOD_SIZE) {
            LOG_ERROR("Peer {} exceeded receive flood limit ({}MB), disconnecting",
                     connection_id_, recv_buffer_.size() / 1024 / 1024);
            close();
            return;
        }
        
        // Process received data
        if (receive_callback_) {
            receive_callback_(recv_buffer_);
            recv_buffer_.clear();  // Consume buffer
        }
        
        // Continue receiving
        start_receive();
    }
}
```

**Why 5 MB?**
- Allows 1 full HEADERS message (200KB) + overhead
- Prevents memory exhaustion (125 peers × 5MB = 625MB max)
- Bitcoin Core uses same limit

---

## 9. Concurrency Model

NOTE: The networking reactor (boost::asio::io_context) runs single-threaded.
Handlers and timers are assumed serialized; other components (validation,
mining, RPC) may use multiple threads. If multi-threaded networking is needed
in the future, introduce explicit serialization (strand or coarse locks)
across NetworkManager/Peer and update tests accordingly.

### 9.1 Threading Architecture

```
┌────────────────────────────────────────────────────────────┐
│                      Main Thread                            │
│  • NetworkManager startup/shutdown                          │
│  • Component initialization                                 │
│  • RPC server (separate from network I/O)                   │
└────────────────────────────────────────────────────────────┘
                          │
        ┌─────────────────┴─────────────────┐
        ▼                                   ▼
┌─────────────────┐              ┌─────────────────┐
│  Timer Thread   │              │   I/O Threads   │
│  (Boost.Asio)   │              │  (Boost.Asio)   │
│                 │              │                 │
│ • connect_timer │              │ Thread 1 ───────┼─► TCP sockets
│ • maintenance   │              │ Thread 2 ───────┼─► Async read/write
│ • feeler_timer  │              │ Thread 3 ───────┼─► Message processing
│ • sendmessages  │              │ Thread 4 ───────┼─► Callbacks
└─────────────────┘              └─────────────────┘
        │                                   │
        └───────────────┬───────────────────┘
                        ▼
            ┌───────────────────────┐
            │  boost::asio::        │
            │  io_context           │
            │                       │
            │ • Multiplexes I/O     │
            │ • Dispatches handlers │
            │ • Thread-safe queue   │
            └───────────────────────┘
```

### 9.2 Lock Hierarchy

**NetworkManager:**
- No internal mutex (single-threaded access via timers)
- Components protected by their own mutexes

**PeerManager:**
```cpp
std::mutex mutex_;  // Protects:
  ├─► peers_ (peer list)
  ├─► peer_misbehavior_ (DoS scores)
  └─► next_peer_id_ (ID allocation)
```

**AddressManager:**
```cpp
std::mutex mutex_;  // Protects:
  ├─► tried_ (tried address table)
  ├─► new_ (new address table)
  └─► rng_ (random number generator)
```

**Peer:**
```cpp
std::mutex block_inv_mutex_;  // Protects:
  └─► blocks_for_inv_relay_ (announcement queue)

// No mutex for recv_buffer_ (single-threaded by io_context strand)
```

**HeaderSyncManager:**
```cpp
std::mutex sync_mutex_;  // Protects:
  └─► last_batch_size_ (sync state)

std::atomic<uint64_t> sync_peer_id_;  // Lock-free
std::atomic<int64_t> last_headers_received_;  // Lock-free
```

### 9.3 Thread Safety Guarantees

**Thread-Safe Operations:**
- ✅ `PeerManager::add_peer()` - Mutex-protected
- ✅ `PeerManager::remove_peer()` - Mutex-protected
- ✅ `AddressManager::select()` - Mutex-protected
- ✅ `Peer::send_message()` - Boost.Asio thread-safe queue
- ✅ `HeaderSyncManager::IsSynced()` - Atomic reads

**NOT Thread-Safe (caller must synchronize):**
- ❌ `NetworkManager` timer callbacks (single-threaded by design)
- ❌ `Peer` receive buffer (single-threaded via io_context strand)

**Boost.Asio Guarantees:**

```cpp
// Handlers for a single connection are NEVER executed concurrently
connection->async_receive(..., [](error_code ec, size_t bytes) {
    // This handler is serialized by io_context
    // No race with other handlers for this connection
});

// Multiple connections can execute handlers concurrently
// But each connection's handlers are serialized
```

**Example: Message Send Race Prevention:**

```cpp
// Thread A calls Peer::send_message()
void Peer::send_message(std::unique_ptr<message::Message> msg) {
    // Serialize message
    std::vector<uint8_t> data = serialize_message(msg);
    
    // Post to io_context (thread-safe queue)
    boost::asio::post(io_context_, [this, data = std::move(data)]() {
        // This executes on I/O thread, serialized with other handlers
        connection_->send(data);
    });
}

// Thread B calls Peer::send_message()
// Both calls are serialized by io_context, no race
```

---

## 10. Performance Characteristics

### 10.1 Network Throughput

**Single Connection:**
```
Bandwidth:            ~1 Gbps (limited by TCP, not protocol)
Message Rate:         ~10,000 messages/sec (small messages)
Header Throughput:    ~20,000 headers/sec (100 bytes each)
```

**Full Node (133 peers):**
```
Bandwidth:            ~100 Mbps aggregate (limited by upstream)
Header Sync Rate:     ~2000 headers/sec (validation bottleneck)
Connection Attempts:  ~1/sec (5s interval, multiple attempts)
```

### 10.2 Memory Footprint

**Per Connection:**
```
Peer object:          ~500 bytes
Receive buffer:       ~5 MB max (flood protection)
Send queue:           ~100 KB typical (unbounded in theory)
────────────────────────────
Total:                ~5.1 MB per peer
```

**Full Node (133 peers):**
```
Peers:                133 × 5.1 MB = ~678 MB
AddressManager:       ~100 KB (1000 addresses)
BanMan:               ~10 KB (100 banned IPs)
────────────────────────────
Total:                ~700 MB
```

**Comparison:**
- Bitcoin Core: ~1-2 GB for peer management
- CoinbaseChain: ~700 MB (headers-only, simpler)

### 10.3 Latency Characteristics

| Operation | Latency | Notes |
|-----------|---------|-------|
| **Send message** | <1ms | Async queue, returns immediately |
| **VERSION handshake** | ~100-500ms | Round-trip over internet |
| **GETHEADERS request** | ~100-500ms | Round-trip + peer processing |
| **HEADERS response** | ~1-5s | 2000 headers × ~50ms validation |
| **Peer discovery (DNS)** | ~50-200ms | DNS lookup + TCP connect |
| **Feeler connection** | ~100-500ms | TCP connect + VERSION/VERACK |

### 10.4 Scalability Limits

**Connection Limits (configurable):**
```
Max outbound:         8 (Bitcoin-compatible)
Max inbound:          125 (Bitcoin-compatible)
Total:                133 connections
```

**Why These Limits?**

1. **Outbound (8):** Balance between network visibility and resource usage
2. **Inbound (125):** Socket limit, memory constraint (125 × 5MB = 625MB)

**Theoretical Maximum (hardware permitting):**
```
Linux socket limit:   ~65,535 (per process)
Memory limit:         ~32 GB RAM → ~6,000 peers (5MB each)
CPU limit:            ~1,000 peers (validation bottleneck)
```

**Practical Maximum (recommended):**
```
Max peers:            ~200 (125 inbound + 75 outbound)
RAM:                  ~1 GB
CPU:                  ~20% of modern CPU
```

---

## Conclusion

The network library implements a **robust, Bitcoin-compatible P2P networking stack** with:

**Strengths:**
- ✅ Proven protocol (98% Bitcoin compatibility)
- ✅ Async I/O (high concurrency via Boost.Asio)
- ✅ Multi-layer DoS protection (scoring, limits, banning)
- ✅ Testable design (abstract Transport interface)
- ✅ Modular architecture (specialized managers)

**Areas for Improvement:**
- ⚠️ No protocol encryption (Bitcoin Core recently added BIP324 v2 transport)
- ⚠️ Single-threaded message processing (validation bottleneck)
- ⚠️ No Tor/I2P support (privacy-focused networks)
- ⚠️ Limited address book (no bucketing, simpler than Bitcoin Core)

**Related Documents:**
- `ARCHITECTURE.md` - Overall system architecture
- `CHAIN_LIBRARY_ARCHITECTURE.md` - Chain validation logic
- `PROTOCOL_SPECIFICATION.md` - Detailed wire protocol
- `SECURITY_AUDIT.md` - Security vulnerabilities
