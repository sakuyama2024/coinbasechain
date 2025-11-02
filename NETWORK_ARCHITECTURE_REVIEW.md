# Network Library Architecture Review
**Date**: 2025-11-02
**Scope**: Network component design, code organization, and refactoring opportunities
**Reviewer**: Architectural Analysis

---

## Executive Summary

The network library implements a **functional but over-engineered** architecture with **9 manager classes** that have **unclear responsibility boundaries** and **extensive code duplication**. While the code works and follows Bitcoin Core patterns, it suffers from:

1. **God Object Pattern**: `NetworkManager` owns and coordinates everything
2. **Circular Dependencies**: Managers have bidirectional references to each other
3. **Scattered State**: Per-peer data spread across 4+ managers
4. **Code Duplication**: Similar patterns repeated in every manager
5. **Unclear Ownership**: Who is responsible for what is ambiguous

**Recommendation**: Refactor into 3-4 focused components instead of 9 managers.

---

## Current Architecture (9 Components)

```
┌──────────────────────────────────────────────────────────┐
│              NetworkManager (God Object)                  │
│  • Owns all 8 other managers                             │
│  • Coordinates timers (4 separate timers)                │
│  • Handles inbound/outbound connections                  │
│  • Bootstrap logic                                       │
│  • Lifecycle management                                  │
└──────────────────────────────────────────────────────────┘
              ▼         ▼         ▼         ▼
    ┌─────────────┬─────────────┬─────────────┬─────────────┐
    │             │             │             │             │
┌────────┐  ┌────────┐  ┌────────┐  ┌────────┐  ┌────────┐
│Peer    │  │Header  │  │Block   │  │Message │  │Address │
│Manager │  │Sync    │  │Relay   │  │Router  │  │Manager │
│        │  │Manager │  │Manager │  │        │  │        │
└────────┘  └────────┘  └────────┘  └────────┘  └────────┘
     ▲           ▲           ▲           ▲           ▲
     └───────────┴───────────┴───────────┴───────────┘
              (all reference each other)

          ┌────────┐  ┌────────┐  ┌────────┐
          │BanMan  │  │Anchor  │  │NAT     │
          │        │  │Manager │  │Manager │
          └────────┘  └────────┘  └────────┘
```

### Component Inventory

| Component | LOC | Responsibilities | Dependencies |
|-----------|-----|------------------|--------------|
| **NetworkManager** | ~1000 | Lifecycle, timers, bootstrap, connection orchestration | All 8 others |
| **PeerManager** | ~600 | Peer lifecycle, DoS scoring, connection limits | AddressManager |
| **MessageRouter** | ~700 | Message dispatch, ADDR handling, echo suppression | AddressManager, HeaderSyncManager, BlockRelayManager, PeerManager |
| **HeaderSyncManager** | ~500 | Header sync, IBD detection, locator generation | ChainstateManager, PeerManager, BanMan |
| **BlockRelayManager** | ~300 | Block announcements, INV flushing | ChainstateManager, PeerManager, HeaderSyncManager |
| **AddressManager** | ~400 | Address book, tried/new tables | None |
| **BanMan** | ~200 | IP banning, persistence | None |
| **AnchorManager** | ~150 | Anchor connection saving/loading | PeerManager |
| **NATManager** | ~200 | UPnP NAT traversal | None |
| **Total** | ~4050 | | |

---

## Problem 1: God Object (NetworkManager)

**Issue**: `NetworkManager` knows about EVERYTHING and does TOO MUCH.

### Current Responsibilities (12+)
```cpp
class NetworkManager {
    // 1. Component ownership (8 managers)
    std::unique_ptr<PeerManager> peer_manager_;
    std::unique_ptr<AddressManager> addr_manager_;
    std::unique_ptr<HeaderSyncManager> header_sync_manager_;
    std::unique_ptr<BlockRelayManager> block_relay_manager_;
    std::unique_ptr<MessageRouter> message_router_;
    std::unique_ptr<BanMan> ban_man_;
    std::unique_ptr<NATManager> nat_manager_;
    std::unique_ptr<AnchorManager> anchor_manager_;

    // 2. I/O thread management
    boost::asio::io_context& io_context_;
    std::vector<std::thread> io_threads_;

    // 3. Four separate timers (could be one)
    std::unique_ptr<boost::asio::steady_timer> connect_timer_;
    std::unique_ptr<boost::asio::steady_timer> maintenance_timer_;
    std::unique_ptr<boost::asio::steady_timer> feeler_timer_;
    std::unique_ptr<boost::asio::steady_timer> sendmessages_timer_;

    // 4. Connection logic
    bool connect_to(const protocol::NetworkAddress &addr);
    void handle_inbound_connection(TransportConnectionPtr connection);

    // 5. Bootstrap logic
    void bootstrap_from_fixed_seeds(const chain::ChainParams &params);

    // 6. Address conversion
    std::optional<std::string> network_address_to_string(...);

    // 7. Self-connection detection
    bool check_incoming_nonce(uint64_t nonce);

    // 8. Message handling delegation
    bool handle_message(PeerPtr peer, std::unique_ptr<message::Message> msg);

    // 9. Anchor persistence
    bool SaveAnchors(const std::string &filepath);
    bool LoadAnchors(const std::string &filepath);

    // 10. Block relay coordination
    void announce_tip_to_peers();
    void relay_block(const uint256 &block_hash);

    // 11. Lifecycle
    bool start();
    void stop();

    // 12. Periodic maintenance
    void run_maintenance();
    void attempt_outbound_connections();
    void attempt_feeler_connection();
};
```

**Violation**: Single Responsibility Principle - this class has 12+ distinct responsibilities.

**Recommendation**: Split into:
- `NetworkService` (lifecycle, I/O threads)
- `ConnectionManager` (connect/disconnect logic)
- `PeriodicScheduler` (unified timer management)

---

## Problem 2: Circular Dependencies

**Issue**: Managers have bidirectional references, creating tight coupling.

### Dependency Graph
```
NetworkManager
  ├──> PeerManager
  ├──> AddressManager
  ├──> HeaderSyncManager ──┐
  │         ├──> PeerManager  ←──┘ CIRCULAR
  │         └──> BanMan
  ├──> BlockRelayManager ──┐
  │         ├──> PeerManager  ←──┘ CIRCULAR
  │         └──> HeaderSyncManager ←┘ CIRCULAR
  ├──> MessageRouter ──┐
  │         ├──> AddressManager  ←┘ OK
  │         ├──> HeaderSyncManager ←┘ CIRCULAR
  │         ├──> BlockRelayManager ←┘ CIRCULAR
  │         └──> PeerManager  ←──┘ CIRCULAR
  └──> AnchorManager ──┐
            └──> PeerManager  ←──┘ CIRCULAR
```

**Circular Dependencies Identified**:
1. `PeerManager` ← → `HeaderSyncManager`
2. `PeerManager` ← → `BlockRelayManager`
3. `PeerManager` ← → `AnchorManager`
4. `HeaderSyncManager` ← → `BlockRelayManager`
5. `MessageRouter` ← → (all other managers)

**Problems**:
- Hard to test in isolation
- Initialization order matters
- Refactoring one component breaks others
- Mental model is complex

**Solution**: Introduce event-based communication or inversion of control:
```cpp
// Instead of: HeaderSyncManager → PeerManager::Disconnect()
// Use: HeaderSyncManager → Event(DisconnectPeer) → NetworkService handles it
```

---

## Problem 3: Per-Peer State Scattered Across 4+ Managers

**Issue**: Data about a single peer is split across multiple managers.

### Current State Distribution
```cpp
// Peer's own state
class Peer {
    PeerState state_;
    int32_t version_;
    uint64_t services_;
    std::string user_agent_;
    std::vector<uint256> blocks_for_inv_relay_;  // ❌ Why here?
};

// PeerManager's state about the peer
struct PeerMisbehaviorData {
    int misbehavior_score;
    int num_unconnecting_headers_msgs;
    NetPermissionFlags permissions;
    std::string address;
    std::unordered_set<std::string> invalid_header_hashes;
};

// HeaderSyncManager's state
struct SyncState {
    uint64_t sync_peer_id;  // Which peer are we syncing from?
    int64_t sync_start_time_us;
    int64_t last_headers_received_us;
    size_t last_batch_size_;
};

// BlockRelayManager's state
std::unordered_map<int, uint256> last_announced_to_peer_;
std::unordered_map<int, int64_t> last_announce_time_s_;

// MessageRouter's state
std::unordered_set<int> getaddr_replied_;
std::unordered_map<int, LearnedMap> learned_addrs_by_peer_;
```

**Total**: 5 different places tracking state for ONE peer!

**Problems**:
- Hard to reason about peer state holistically
- When peer disconnects, must clean up in 5 places
- Easy to introduce bugs (forgot to clean up state in one manager)
- No single source of truth

**Solution**: Consolidate into `PeerState` struct:
```cpp
struct PeerState {
    // Connection info
    PeerConnectionInfo connection;

    // DoS tracking
    MisbehaviorData misbehavior;

    // Sync state
    HeaderSyncState header_sync;

    // Block relay state
    BlockRelayState block_relay;

    // Address state
    AddressState addr_state;
};

// Managers access via:
class PeerStateManager {
    PeerState& GetState(int peer_id);
};
```

---

## Problem 4: Message Handling Split Across 3 Layers

**Issue**: Message processing logic is fragmented.

### Current Flow (Too Complex)
```
Peer receives raw bytes from transport
  ↓
Peer::on_transport_receive() → Appends to buffer
  ↓
Peer::process_received_data() → Parses 24-byte header
  ↓
Peer::process_message() → Deserializes payload
  ↓
Calls message_handler_ callback
  ↓
NetworkManager::handle_message() → Self-connection detection
  ↓
MessageRouter::RouteMessage() → Dispatch logic
  ↓
├─> handle_verack()
├─> handle_addr() → AddressManager
├─> handle_getaddr() → AddressManager + echo suppression
├─> handle_inv() → BlockRelayManager
├─> handle_headers() → HeaderSyncManager
└─> handle_getheaders() → HeaderSyncManager
```

**3 Layers of Indirection**:
1. `Peer::process_message()` - Deserializes
2. `NetworkManager::handle_message()` - Self-connection check
3. `MessageRouter::RouteMessage()` - Actual dispatch

**Redundancy**: Each layer does minimal work, mostly just forwarding.

**Solution**: Collapse into 2 layers:
```cpp
// Layer 1: Peer (framing only)
Peer::process_message() → Deserializes → Calls handler

// Layer 2: MessageDispatcher (single dispatch point)
MessageDispatcher::Dispatch(peer, msg)
  ├─> If VERACK → ...
  ├─> If ADDR → ...
  ├─> If HEADERS → HeaderSyncManager::Handle()
  └─> ...
```

---

## Problem 5: Code Duplication (Same Patterns Everywhere)

### Duplication 1: Mutex + Map Pattern (Repeated 5 Times)

**In PeerManager:**
```cpp
mutable std::mutex mutex_;
std::map<int, PeerPtr> peers_;
std::map<int, PeerMisbehaviorData> peer_misbehavior_;

PeerPtr get_peer(int peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
        return it->second;
    }
    return nullptr;
}
```

**In HeaderSyncManager:**
```cpp
mutable std::mutex sync_mutex_;
SyncState sync_state_;

uint64_t GetSyncPeerId() const {
    std::lock_guard<std::mutex> lock(sync_mutex_);
    return sync_state_.sync_peer_id;
}
```

**In BlockRelayManager:**
```cpp
mutable std::mutex announce_mutex_;
std::unordered_map<int, uint256> last_announced_to_peer_;
std::unordered_map<int, int64_t> last_announce_time_s_;
```

**In MessageRouter:**
```cpp
mutable std::mutex addr_mutex_;
std::unordered_map<int, LearnedMap> learned_addrs_by_peer_;
std::unordered_set<int> getaddr_replied_;
```

**In AddressManager:**
```cpp
mutable std::mutex mutex_;
std::map<std::string, AddrInfo> tried_;
std::map<std::string, AddrInfo> new_;
```

**Duplication**: Same "mutex + map + accessor" pattern repeated 5 times.

**Solution**: Extract common pattern:
```cpp
template<typename Key, typename Value>
class ThreadSafeMap {
    mutable std::mutex mutex_;
    std::map<Key, Value> data_;

public:
    std::optional<Value> Get(const Key& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = data_.find(key);
        return (it != data_.end()) ? std::make_optional(it->second) : std::nullopt;
    }

    void Set(const Key& key, const Value& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_[key] = value;
    }

    // ... other operations
};
```

### Duplication 2: Timer Management (Repeated 4 Times)

**In NetworkManager:**
```cpp
// Timer 1: Connection attempts
void schedule_next_connection_attempt() {
    if (!running_.load()) return;
    connect_timer_->expires_after(config_.connect_interval);
    connect_timer_->async_wait([this](const boost::system::error_code &ec) {
        if (!ec && running_.load()) {
            attempt_outbound_connections();
            schedule_next_connection_attempt();
        }
    });
}

// Timer 2: Maintenance
void schedule_next_maintenance() {
    if (!running_.load()) return;
    maintenance_timer_->expires_after(config_.maintenance_interval);
    maintenance_timer_->async_wait([this](const boost::system::error_code &ec) {
        if (!ec && running_.load()) {
            run_maintenance();
            schedule_next_maintenance();
        }
    });
}

// Timer 3: Feeler
void schedule_next_feeler() { /* Same pattern */ }

// Timer 4: SendMessages
void schedule_next_sendmessages() { /* Same pattern */ }
```

**Duplication**: Identical pattern for 4 timers.

**Solution**: Unified timer scheduler:
```cpp
class PeriodicScheduler {
    void SchedulePeriodic(std::string name,
                          std::chrono::milliseconds interval,
                          std::function<void()> task) {
        // Manages all periodic tasks
    }
};

// Usage:
scheduler.SchedulePeriodic("connection_attempts", 5s, [this]() {
    attempt_outbound_connections();
});
scheduler.SchedulePeriodic("maintenance", 30s, [this]() {
    run_maintenance();
});
// etc.
```

### Duplication 3: Cleanup on Peer Disconnect (Repeated 5 Times)

**In PeerManager:**
```cpp
void remove_peer(int peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    peers_.erase(peer_id);
    peer_misbehavior_.erase(peer_id);
    peer_created_at_.erase(peer_id);
}
```

**In MessageRouter:**
```cpp
void OnPeerDisconnected(int peer_id) {
    std::lock_guard<std::mutex> lock(addr_mutex_);
    getaddr_replied_.erase(peer_id);
    learned_addrs_by_peer_.erase(peer_id);
}
```

**In BlockRelayManager:**
```cpp
void OnPeerDisconnected(int peer_id) {
    std::lock_guard<std::mutex> lock(announce_mutex_);
    last_announced_to_peer_.erase(peer_id);
    last_announce_time_s_.erase(peer_id);
}
```

**In HeaderSyncManager:**
```cpp
void OnPeerDisconnected(uint64_t peer_id) {
    std::lock_guard<std::mutex> lock(sync_mutex_);
    if (sync_state_.sync_peer_id == peer_id) {
        ClearSyncPeerUnlocked();
    }
}
```

**Problem**: Each manager must manually clean up when peer disconnects.

**Solution**: Unified event system:
```cpp
// Peer disconnected → Event → All managers notified
event_bus.Publish(PeerDisconnectedEvent{peer_id});

// Each manager subscribes:
event_bus.Subscribe([this](PeerDisconnectedEvent e) {
    CleanupPeerState(e.peer_id);
});
```

---

## Problem 6: Unclear Responsibility Boundaries

**Question**: Who is responsible for what?

### Example 1: Who Manages Peer Connections?

**Answer**: Three classes!

```cpp
// NetworkManager handles:
- Creating outbound connections (connect_to)
- Accepting inbound connections (handle_inbound_connection)
- Self-connection detection (check_incoming_nonce)

// PeerManager handles:
- Adding peers to the list (add_peer)
- Removing peers (remove_peer)
- Connection limits (needs_more_outbound, can_accept_inbound)
- Eviction (evict_inbound_peer)

// Peer handles:
- Connection state machine (CONNECTING → READY)
- Handshake (VERSION/VERACK)
- Disconnect (disconnect)
```

**Problem**: No single owner. Logic scattered.

### Example 2: Who Manages Block Announcements?

**Answer**: Two classes!

```cpp
// Peer stores:
std::vector<uint256> blocks_for_inv_relay_;
std::mutex block_inv_mutex_;

// BlockRelayManager handles:
- AnnounceTipToAllPeers() → Queues blocks in Peer
- FlushBlockAnnouncements() → Sends INV from Peer's queue
```

**Problem**: `Peer` holds data but `BlockRelayManager` manages it. Split-brain.

### Example 3: Who Handles ADDR Messages?

**Answer**: Two classes!

```cpp
// MessageRouter handles:
- handle_addr() → Processes incoming ADDR
- handle_getaddr() → Sends ADDR response
- Echo suppression logic

// AddressManager handles:
- add() → Stores addresses
- select() → Retrieves addresses
```

**Problem**: ADDR *message* logic in `MessageRouter`, but *address* logic in `AddressManager`.

---

## Problem 7: Over-Abstraction (9 Managers for ~4000 LOC)

**Metric**: Lines of Code per Component

| Component | LOC | Comment |
|-----------|-----|---------|
| NetworkManager | 1000 | God object |
| PeerManager | 600 | Peer lifecycle + DoS (2 responsibilities) |
| MessageRouter | 700 | Message dispatch + ADDR handling (2 responsibilities) |
| HeaderSyncManager | 500 | Just header sync |
| BlockRelayManager | 300 | Just block announcements |
| AddressManager | 400 | Address book |
| BanMan | 200 | IP banning |
| AnchorManager | 150 | Anchor persistence |
| NATManager | 200 | UPnP |

**Average**: 450 LOC per component

**Analysis**:
- `BlockRelayManager` (300 LOC) could be merged with `HeaderSyncManager`
- `AnchorManager` (150 LOC) is just persistence - could be part of `AddressManager`
- `BanMan` (200 LOC) could be part of `PeerManager`
- `MessageRouter` (700 LOC) is just a dispatch table - could be a map of lambdas

**Over-engineering**: 9 classes when 3-4 would suffice.

---

## Proposed Refactoring

### Goal: Reduce from 9 components to 4 components

```
┌──────────────────────────────────────────────────────────┐
│              NetworkService (Lifecycle)                   │
│  • I/O thread management                                 │
│  • Startup/shutdown                                      │
│  • Configuration                                         │
└──────────────────────────────────────────────────────────┘
                          │
        ┌─────────────────┼─────────────────┐
        ▼                 ▼                 ▼
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ Connection   │  │ Sync         │  │ Discovery    │
│ Manager      │  │ Manager      │  │ Manager      │
│              │  │              │  │              │
│ • Peer       │  │ • Header     │  │ • Address    │
│   lifecycle  │  │   sync       │  │   book       │
│ • DoS        │  │ • Block      │  │ • Anchors    │
│   scoring    │  │   relay      │  │ • ADDR/      │
│ • Banning    │  │ • IBD        │  │   GETADDR    │
│ • Eviction   │  │              │  │              │
└──────────────┘  └──────────────┘  └──────────────┘
```

### Component 1: NetworkService (Lifecycle Only)

**Responsibilities**:
- Start/stop I/O threads
- Load/save persistent data
- Configuration management
- Event bus (for component communication)

**Eliminated**:
- ❌ Connection logic (moved to ConnectionManager)
- ❌ Timer management (each manager owns its timers)
- ❌ Message routing (moved to MessageDispatcher)

**Lines of Code**: ~300 (down from 1000)

```cpp
class NetworkService {
public:
    struct Config {
        size_t io_threads;
        std::string datadir;
        // ... other config
    };

    NetworkService(Config config);

    bool Start();
    void Stop();

    // Provide services to components
    boost::asio::io_context& GetIOContext() { return io_context_; }
    EventBus& GetEventBus() { return event_bus_; }

private:
    Config config_;
    boost::asio::io_context io_context_;
    std::vector<std::thread> io_threads_;
    EventBus event_bus_;  // For decoupled communication
};
```

### Component 2: ConnectionManager (Peers + DoS + Banning)

**Responsibilities**:
- Peer connection lifecycle
- DoS tracking & misbehavior scoring
- IP banning (absorbs BanMan)
- Connection limits & eviction
- Message dispatch (absorbs MessageRouter)

**Merges**:
- ✅ PeerManager
- ✅ BanMan
- ✅ MessageRouter (dispatch only)

**Lines of Code**: ~1200 (consolidates 1500 LOC)

```cpp
class ConnectionManager {
public:
    ConnectionManager(EventBus& event_bus, Config config);

    // Connection lifecycle
    int AddPeer(PeerPtr peer, NetPermissionFlags perms);
    void RemovePeer(int peer_id);
    PeerPtr GetPeer(int peer_id);

    // DoS protection
    void ReportMisbehavior(int peer_id, MisbehaviorType type);
    void Ban(const std::string& ip, int64_t duration);
    bool IsBanned(const std::string& ip) const;

    // Connection limits
    bool NeedsMoreOutbound() const;
    bool CanAcceptInbound() const;

    // Message dispatch
    void DispatchMessage(PeerPtr peer, std::unique_ptr<Message> msg);

private:
    struct PeerState {
        PeerPtr peer;
        MisbehaviorData misbehavior;
        NetPermissionFlags permissions;
        std::chrono::steady_clock::time_point created_at;
    };

    // Single state map (instead of 4 separate maps)
    ThreadSafeMap<int, PeerState> peers_;

    // Banning
    ThreadSafeMap<std::string, int64_t> banned_ips_;

    // Message handlers (map instead of MessageRouter class)
    std::unordered_map<std::string, MessageHandler> handlers_;
};
```

### Component 3: SyncManager (Headers + Blocks)

**Responsibilities**:
- Header synchronization
- Block announcements (absorbs BlockRelayManager)
- IBD detection
- Sync peer selection

**Merges**:
- ✅ HeaderSyncManager
- ✅ BlockRelayManager

**Lines of Code**: ~700 (consolidates 800 LOC)

```cpp
class SyncManager {
public:
    SyncManager(ChainstateManager& chainstate, EventBus& event_bus);

    // Header sync
    void RequestHeaders(PeerPtr peer);
    bool HandleHeaders(PeerPtr peer, HeadersMessage* msg);
    bool HandleGetHeaders(PeerPtr peer, GetHeadersMessage* msg);

    // Block relay
    void AnnounceBlock(const uint256& hash);
    bool HandleInv(PeerPtr peer, InvMessage* msg);
    void FlushAnnouncements();

    // Sync state
    bool IsSynced() const;
    std::optional<int> GetSyncPeerId() const;

private:
    struct SyncState {
        std::optional<int> sync_peer_id;
        int64_t sync_start_time;
        int64_t last_headers_received;
    };

    ChainstateManager& chainstate_;
    EventBus& event_bus_;
    ThreadSafeState<SyncState> sync_state_;

    // Per-peer block announcements
    ThreadSafeMap<int, std::vector<uint256>> pending_announcements_;
};
```

### Component 4: DiscoveryManager (Addresses + Anchors)

**Responsibilities**:
- Address book management (absorbs AddressManager)
- Anchor connections (absorbs AnchorManager)
- ADDR/GETADDR handling
- Peer discovery

**Merges**:
- ✅ AddressManager
- ✅ AnchorManager
- ✅ MessageRouter (ADDR handling only)

**Lines of Code**: ~600 (consolidates 850 LOC)

```cpp
class DiscoveryManager {
public:
    DiscoveryManager(EventBus& event_bus);

    // Address book
    bool Add(const protocol::NetworkAddress& addr);
    std::optional<protocol::NetworkAddress> Select();
    void MarkGood(const protocol::NetworkAddress& addr);
    void MarkFailed(const protocol::NetworkAddress& addr);

    // Anchors
    std::vector<protocol::NetworkAddress> GetAnchors() const;
    void SaveAnchors(const std::string& path);
    void LoadAnchors(const std::string& path);

    // ADDR messages
    bool HandleAddr(PeerPtr peer, AddrMessage* msg);
    bool HandleGetAddr(PeerPtr peer);

private:
    // Tried/new tables
    ThreadSafeMap<std::string, AddrInfo> tried_;
    ThreadSafeMap<std::string, AddrInfo> new_;

    // Anchor addresses
    std::vector<protocol::NetworkAddress> anchors_;

    // GETADDR tracking
    ThreadSafeSet<int> getaddr_replied_;
};
```

---

## Refactoring Benefits

### Before (9 Components, 4050 LOC)
```
NetworkManager:     1000 LOC
PeerManager:         600 LOC
MessageRouter:       700 LOC
HeaderSyncManager:   500 LOC
BlockRelayManager:   300 LOC
AddressManager:      400 LOC
BanMan:              200 LOC
AnchorManager:       150 LOC
NATManager:          200 LOC
────────────────────────────
Total:              4050 LOC
```

### After (4 Components, ~2800 LOC)
```
NetworkService:      300 LOC  (lifecycle only)
ConnectionManager:  1200 LOC  (peers + DoS + banning + dispatch)
SyncManager:         700 LOC  (headers + blocks)
DiscoveryManager:    600 LOC  (addresses + anchors + ADDR)
────────────────────────────
Total:              2800 LOC  (30% reduction)
```

**Savings**:
- **1250 LOC eliminated** (mostly boilerplate and duplication)
- **30% code reduction**
- **5 fewer classes to understand**
- **Clearer responsibilities**

### Before/After Comparison

| Aspect | Before (9 components) | After (4 components) |
|--------|----------------------|---------------------|
| **Circular deps** | 5+ circular dependencies | 0 (event bus breaks cycles) |
| **Per-peer state** | Scattered across 5 classes | Consolidated in ConnectionManager |
| **Message dispatch** | 3 layers of indirection | 1 layer (handler map) |
| **Mutex patterns** | Repeated 5 times | Extracted to ThreadSafeMap |
| **Timer patterns** | Repeated 4 times | Each component owns its timers |
| **Cleanup on disconnect** | 5 separate functions | Single event handler |
| **Lines of code** | 4050 LOC | 2800 LOC |
| **Cognitive load** | High (9 components) | Medium (4 components) |

---

## Specific Recommendations

### Recommendation 1: Eliminate NetworkManager God Object

**Current**:
```cpp
class NetworkManager {
    // Owns 8 managers + does coordination + lifecycle + timers + connections
};
```

**Proposed**:
```cpp
class NetworkService {
    // ONLY: Lifecycle + I/O threads + event bus
};

// Components are peers, not owned:
NetworkService service;
ConnectionManager connection_mgr(service.GetEventBus());
SyncManager sync_mgr(chainstate, service.GetEventBus());
DiscoveryManager discovery_mgr(service.GetEventBus());
```

### Recommendation 2: Consolidate Per-Peer State

**Current**: 5 separate places
```cpp
Peer::blocks_for_inv_relay_
PeerManager::peer_misbehavior_[id]
HeaderSyncManager::sync_state_.sync_peer_id
BlockRelayManager::last_announced_to_peer_[id]
MessageRouter::learned_addrs_by_peer_[id]
```

**Proposed**: Single source of truth
```cpp
struct PeerState {
    PeerPtr peer;
    MisbehaviorData misbehavior;
    SyncData sync;
    BlockRelayData block_relay;
    AddressData addr_data;
};

ConnectionManager::peers_[id]  // Single map
```

### Recommendation 3: Use Event Bus for Decoupling

**Current**: Direct calls create circular dependencies
```cpp
HeaderSyncManager → PeerManager::ReportInvalidHeader()
BlockRelayManager → PeerManager::GetPeer()
MessageRouter → HeaderSyncManager::HandleHeaders()
```

**Proposed**: Event-driven architecture
```cpp
// HeaderSyncManager publishes event
event_bus.Publish(InvalidHeaderEvent{peer_id, reason});

// ConnectionManager subscribes
event_bus.Subscribe([this](InvalidHeaderEvent e) {
    ReportMisbehavior(e.peer_id, MisbehaviorType::InvalidHeader);
});
```

### Recommendation 4: Extract ThreadSafeMap Utility

**Instead of repeating**:
```cpp
mutable std::mutex mutex_;
std::map<int, T> data_;
```

**Use**:
```cpp
template<typename K, typename V>
class ThreadSafeMap {
    mutable std::mutex mutex_;
    std::map<K, V> data_;
public:
    std::optional<V> Get(const K& key) const;
    void Set(const K& key, const V& value);
    void Erase(const K& key);
    size_t Size() const;
    void ForEach(std::function<void(const K&, const V&)> fn) const;
};
```

### Recommendation 5: Simplify Message Dispatch

**Current**: 3 layers
```cpp
Peer → NetworkManager::handle_message → MessageRouter::RouteMessage → handle_X()
```

**Proposed**: 1 layer with handler map
```cpp
class ConnectionManager {
    std::unordered_map<std::string, MessageHandler> handlers_;

    void RegisterHandler(std::string cmd, MessageHandler handler) {
        handlers_[cmd] = handler;
    }

    void DispatchMessage(PeerPtr peer, std::unique_ptr<Message> msg) {
        auto it = handlers_.find(msg->command());
        if (it != handlers_.end()) {
            it->second(peer, std::move(msg));
        }
    }
};

// Registration:
connection_mgr.RegisterHandler("headers", [&](PeerPtr p, auto msg) {
    sync_mgr.HandleHeaders(p, static_cast<HeadersMessage*>(msg.get()));
});
```

---

## Migration Path

### Phase 1: Extract ThreadSafeMap (Low Risk)
1. Create `ThreadSafeMap<K,V>` template
2. Replace all `mutex + map` patterns
3. Run tests

**Effort**: 2 days
**Risk**: Low

### Phase 2: Merge BanMan into PeerManager (Low Risk)
1. Move banning logic into `PeerManager`
2. Update `NetworkManager` references
3. Run tests

**Effort**: 1 day
**Risk**: Low

### Phase 3: Consolidate Per-Peer State (Medium Risk)
1. Create `PeerState` struct
2. Migrate state from 4 managers into `PeerState`
3. Update accessors
4. Run tests

**Effort**: 5 days
**Risk**: Medium

### Phase 4: Introduce Event Bus (High Risk)
1. Implement simple event bus
2. Convert one circular dependency to events
3. Run tests
4. Repeat for other dependencies

**Effort**: 10 days
**Risk**: High (changes architecture fundamentally)

### Phase 5: Merge Managers (High Risk)
1. Merge `HeaderSyncManager` + `BlockRelayManager` → `SyncManager`
2. Merge `AddressManager` + `AnchorManager` → `DiscoveryManager`
3. Simplify `NetworkManager` → `NetworkService`
4. Consolidate message dispatch

**Effort**: 15 days
**Risk**: High

**Total Effort**: ~33 days (6-7 weeks)

---

## Conclusion

The network library is **functionally correct but over-engineered**. The current 9-component architecture with circular dependencies and scattered state creates unnecessary complexity.

### Key Metrics

| Metric | Current | Proposed | Improvement |
|--------|---------|----------|-------------|
| Components | 9 | 4 | 56% fewer |
| LOC | 4050 | 2800 | 30% reduction |
| Circular deps | 5+ | 0 | 100% eliminated |
| Per-peer state locations | 5 | 1 | 80% consolidation |
| Message dispatch layers | 3 | 1 | 66% simplification |

### Recommendations (Priority Order)

1. **HIGH**: Extract `ThreadSafeMap` utility (quick win, low risk)
2. **HIGH**: Consolidate per-peer state (major simplification)
3. **MEDIUM**: Merge BanMan into PeerManager (reduce component count)
4. **MEDIUM**: Merge HeaderSync + BlockRelay managers (related responsibilities)
5. **LOW**: Introduce event bus (long-term decoupling)
6. **LOW**: Simplify NetworkManager (can be done incrementally)

**Immediate Action**: Start with Phase 1 (ThreadSafeMap) - low risk, immediate benefit, enables future refactoring.

The code works, but it's harder to maintain than it needs to be. A focused refactoring effort would make the codebase significantly more maintainable without changing functionality.
