# NetworkManager Refactoring Plan

**Current State**: `network_manager.cpp` = **1508 lines** (too large, violates Single Responsibility Principle)

**Goal**: Break down into focused, maintainable components (~300-400 lines each)

---

## Problem Analysis

### Largest Methods
1. `handle_headers_message()` - **253 lines**
2. `handle_message()` - **101 lines**
3. `connect_to()` - **94 lines**
4. `bootstrap_from_fixed_seeds()` - **85 lines**
5. `LoadAnchors()` - **82 lines**
6. `handle_getheaders_message()` - **75 lines**

**Top 2 methods alone = 354 lines (23% of file!)**

### Responsibilities Currently Mixed Together
NetworkManager currently handles:
1. **Header Synchronization** (~400 lines)
2. **Message Routing** (~150 lines)
3. **Connection Management** (~250 lines)
4. **Block Relay** (~100 lines)
5. **Anchor Management** (~150 lines)
6. **Lifecycle/Maintenance** (~100 lines)
7. **Component Coordination** (~rest)

---

## Proposed Architecture

### Current (Monolithic)
```
NetworkManager (1508 lines)
├── Header sync logic
├── Message routing
├── Connection management
├── Block relay
├── Anchor management
└── Everything else
```

### Proposed (Modular)
```
NetworkManager (coordinator, ~350 lines)
├── HeaderSyncManager (~400 lines)
├── MessageRouter (~200 lines)
├── ConnectionManager (~300 lines)
├── BlockRelayManager (~150 lines)
└── AnchorManager (~100 lines)
```

---

## Refactoring Plan

### Phase 1: Extract HeaderSyncManager ⭐ **HIGHEST IMPACT**

**Rationale**: Largest chunk of code (400+ lines), well-defined responsibility

**New file**: `include/network/header_sync_manager.hpp`, `src/network/header_sync_manager.cpp`

**Responsibilities**:
- Handle incoming headers messages
- Request headers from peers
- Track sync state (is_synced, should_request_more)
- Generate block locators
- Initial sync coordination

**Methods to move**:
```cpp
class HeaderSyncManager {
public:
  HeaderSyncManager(validation::ChainstateManager& chainstate,
                    PeerManager& peer_mgr,
                    network::BanMan& ban_man);

  // Message handlers (from NetworkManager)
  bool HandleHeadersMessage(PeerPtr peer, message::HeadersMessage* msg);
  bool HandleGetHeadersMessage(PeerPtr peer, message::GetHeadersMessage* msg);

  // Sync coordination (from NetworkManager)
  void RequestHeadersFromPeer(PeerPtr peer);
  void CheckInitialSync();

  // State queries (from NetworkManager)
  bool IsSynced(int64_t max_age_seconds = 3600) const;
  bool ShouldRequestMore() const;

  // Block locator generation (from NetworkManager)
  CBlockLocator GetLocator() const;
  CBlockLocator GetLocatorFromPrev() const;

  // Sync tracking
  uint64_t GetSyncPeerId() const { return sync_peer_id_.load(); }
  void SetSyncPeer(uint64_t peer_id);
  void ClearSyncPeer();

private:
  validation::ChainstateManager& chainstate_manager_;
  PeerManager& peer_manager_;
  network::BanMan& ban_man_;

  // Sync state
  std::atomic<uint64_t> sync_peer_id_{0};
  std::atomic<int64_t> sync_start_time_{0};
  std::atomic<int64_t> last_headers_received_{0};

  mutable std::mutex sync_mutex_;
  size_t last_batch_size_{0};
};
```

**Lines saved in NetworkManager**: ~400 lines

**Before**:
```cpp
// NetworkManager.cpp line 805-1058
bool NetworkManager::handle_headers_message(PeerPtr peer, ...) {
    // 253 lines of header processing
}
```

**After**:
```cpp
// NetworkManager.cpp
bool NetworkManager::handle_message(PeerPtr peer, ...) {
    if (msg_type == message::Type::HEADERS) {
        return header_sync_->HandleHeadersMessage(peer, ...);
    }
    // ...
}
```

---

### Phase 2: Extract MessageRouter

**Rationale**: Message routing is a separate concern from coordination

**New file**: `include/network/message_router.hpp`, `src/network/message_router.cpp`

**Responsibilities**:
- Route incoming messages to appropriate handlers
- Setup peer message handlers
- Message type dispatch

**Methods to move**:
```cpp
class MessageRouter {
public:
  MessageRouter(HeaderSyncManager& header_sync,
                BlockRelayManager& block_relay,
                PeerManager& peer_mgr);

  bool RouteMessage(PeerPtr peer, std::unique_ptr<message::Message> msg);
  void SetupPeerMessageHandler(Peer* peer);

private:
  HeaderSyncManager& header_sync_;
  BlockRelayManager& block_relay_;
  PeerManager& peer_manager_;

  bool HandleVersionMessage(PeerPtr peer, message::VersionMessage* msg);
  bool HandleVerackMessage(PeerPtr peer, message::VerackMessage* msg);
  bool HandlePingMessage(PeerPtr peer, message::PingMessage* msg);
  bool HandlePongMessage(PeerPtr peer, message::PongMessage* msg);
  bool HandleAddrMessage(PeerPtr peer, message::AddrMessage* msg);
  // ...
};
```

**Lines saved in NetworkManager**: ~150 lines

---

### Phase 3: Extract ConnectionManager

**Rationale**: Connection lifecycle is complex and deserves its own component

**New file**: `include/network/connection_manager.hpp`, `src/network/connection_manager.cpp`

**Responsibilities**:
- Manage outbound connection attempts
- Handle inbound connections
- Bootstrap from fixed seeds
- Track connection state
- Periodic connection maintenance

**Methods to move**:
```cpp
class ConnectionManager {
public:
  ConnectionManager(std::shared_ptr<Transport> transport,
                    AddressManager& addr_mgr,
                    PeerManager& peer_mgr,
                    network::BanMan& ban_man,
                    uint64_t local_nonce);

  // Connection lifecycle
  bool ConnectTo(const std::string& address, uint16_t port);
  void DisconnectFrom(int peer_id);
  void HandleInboundConnection(TransportConnectionPtr connection);

  // Periodic tasks
  void AttemptOutboundConnections();
  void BootstrapFromFixedSeeds(const chain::ChainParams& params);

  // Connection checks
  bool CheckIncomingNonce(uint64_t nonce) const;

private:
  std::shared_ptr<Transport> transport_;
  AddressManager& addr_manager_;
  PeerManager& peer_manager_;
  network::BanMan& ban_man_;
  uint64_t local_nonce_;

  std::optional<std::string> NetworkAddressToString(const protocol::NetworkAddress& addr);
};
```

**Lines saved in NetworkManager**: ~300 lines

---

### Phase 4: Extract BlockRelayManager

**Rationale**: Block announcement/relay is distinct from header sync

**New file**: `include/network/block_relay_manager.hpp`, `src/network/block_relay_manager.cpp`

**Responsibilities**:
- Announce new blocks to peers
- Handle INV messages
- Track block propagation

**Methods to move**:
```cpp
class BlockRelayManager {
public:
  BlockRelayManager(PeerManager& peer_mgr,
                    validation::ChainstateManager& chainstate);

  void RelayBlock(const uint256& block_hash);
  void AnnounceTipToPeers();
  bool HandleInvMessage(PeerPtr peer, message::InvMessage* msg);

private:
  PeerManager& peer_manager_;
  validation::ChainstateManager& chainstate_manager_;

  uint256 last_announced_tip_;
  int64_t last_tip_announcement_time_{0};
};
```

**Lines saved in NetworkManager**: ~150 lines

---

### Phase 5: Extract AnchorManager

**Rationale**: Anchor persistence is self-contained

**New file**: `include/network/anchor_manager.hpp`, `src/network/anchor_manager.cpp`

**Responsibilities**:
- Save/load anchor peers for eclipse attack resistance
- Select high-quality anchor connections

**Methods to move**:
```cpp
class AnchorManager {
public:
  AnchorManager(PeerManager& peer_mgr);

  bool SaveAnchors(const std::string& filepath);
  bool LoadAnchors(const std::string& filepath);
  std::vector<protocol::NetworkAddress> GetAnchors() const;

private:
  PeerManager& peer_manager_;
};
```

**Lines saved in NetworkManager**: ~150 lines

---

## Final Architecture

### NetworkManager (Coordinator, ~350 lines)

**Remaining responsibilities**:
- Component lifecycle (start/stop)
- Component coordination
- IO context management
- Periodic timer scheduling
- Component accessor methods

```cpp
class NetworkManager {
public:
  NetworkManager(validation::ChainstateManager& chainstate_manager,
                 const Config& config = Config{},
                 std::shared_ptr<Transport> transport = nullptr,
                 boost::asio::io_context* external_io_context = nullptr);
  ~NetworkManager();

  // Lifecycle
  bool start();
  void stop();
  bool is_running() const { return running_; }

  // Component access
  PeerManager& peer_manager() { return *peer_manager_; }
  AddressManager& address_manager() { return *addr_manager_; }
  HeaderSyncManager& header_sync() { return *header_sync_; }
  BlockRelayManager& block_relay() { return *block_relay_; }
  network::BanMan& ban_man() { return *ban_man_; }

  // Delegated operations (thin wrappers)
  bool connect_to(const std::string& address, uint16_t port) {
    return connection_mgr_->ConnectTo(address, port);
  }

  void relay_block(const uint256& block_hash) {
    block_relay_->RelayBlock(block_hash);
  }

private:
  Config config_;
  std::atomic<bool> running_{false};
  uint64_t local_nonce_;

  // IO infrastructure
  std::shared_ptr<Transport> transport_;
  std::unique_ptr<boost::asio::io_context> owned_io_context_;
  boost::asio::io_context& io_context_;
  std::unique_ptr<boost::asio::executor_work_guard<...>> work_guard_;
  std::vector<std::thread> io_threads_;

  // Core components
  std::unique_ptr<AddressManager> addr_manager_;
  std::unique_ptr<PeerManager> peer_manager_;
  std::unique_ptr<network::BanMan> ban_man_;
  std::unique_ptr<NATManager> nat_manager_;
  validation::ChainstateManager& chainstate_manager_;

  // NEW: Extracted components
  std::unique_ptr<HeaderSyncManager> header_sync_;
  std::unique_ptr<MessageRouter> message_router_;
  std::unique_ptr<ConnectionManager> connection_mgr_;
  std::unique_ptr<BlockRelayManager> block_relay_;
  std::unique_ptr<AnchorManager> anchor_mgr_;

  // Periodic tasks
  std::unique_ptr<boost::asio::steady_timer> connect_timer_;
  std::unique_ptr<boost::asio::steady_timer> maintenance_timer_;

  void run_maintenance();
  void schedule_next_maintenance();
  void schedule_next_connection_attempt();
};
```

---

## Implementation Strategy

### Order of Execution
1. **Phase 1 (HeaderSyncManager)** - Biggest win, lowest risk
2. **Phase 4 (BlockRelayManager)** - Small, independent
3. **Phase 5 (AnchorManager)** - Small, independent
4. **Phase 3 (ConnectionManager)** - Medium complexity
5. **Phase 2 (MessageRouter)** - Ties everything together

### Per-Phase Steps
1. Create new header/source files
2. Copy methods to new class
3. Update NetworkManager to delegate
4. Update tests
5. Verify build + tests pass
6. Commit

### Risk Mitigation
- **One phase at a time** - don't mix refactorings
- **Keep tests passing** - run full test suite after each phase
- **No behavior changes** - pure code movement
- **Git commits per phase** - easy to revert if needed

---

## Benefits

### Code Quality
- **Single Responsibility**: Each class has one clear purpose
- **Testability**: Can test header sync without full NetworkManager
- **Readability**: 400-line files vs 1500-line monolith
- **Maintainability**: Changes localized to relevant component

### Performance
- **No overhead**: All calls are direct (no virtual dispatch needed)
- **Same object graph**: Just reorganized, not restructured

### Developer Experience
- **Easier navigation**: Find header sync code in HeaderSyncManager
- **Parallel development**: Multiple devs can work on different components
- **Clearer intent**: Class names document responsibilities

---

## Metrics

### Before Refactoring
- NetworkManager: **1508 lines**
- Largest method: **253 lines**
- Responsibilities: **7+ mixed concerns**

### After Refactoring (estimated)
- NetworkManager: **~350 lines** (-77%)
- HeaderSyncManager: **~400 lines**
- MessageRouter: **~200 lines**
- ConnectionManager: **~300 lines**
- BlockRelayManager: **~150 lines**
- AnchorManager: **~100 lines**

**Total lines**: ~1500 (same code, better organized)
**Largest file**: 400 lines (vs 1508)
**Largest method**: <100 lines (vs 253)

---

## Alternatives Considered

### Alternative 1: Keep as-is
- **Pros**: No refactoring risk
- **Cons**: Code continues to rot, harder to maintain

### Alternative 2: Full rewrite
- **Pros**: Clean slate
- **Cons**: High risk, breaks existing tests

### Alternative 3: Incremental + Inheritance
- **Pros**: Gradual migration
- **Cons**: Virtual dispatch overhead, complex class hierarchy

**Chosen**: Composition-based extraction (best balance of safety + benefit)

---

## Next Steps

1. Review this plan with team
2. Start with Phase 1 (HeaderSyncManager) as proof-of-concept
3. If successful, proceed with remaining phases
4. Update architecture documentation

---

**Document Version**: 1.0
**Created**: 2025-10-20
**Status**: PROPOSED
