# Network Manager Responsibilities

**Last Updated**: 2025-11-03 (Phase 4 Complete)
**Network Architecture**: 7 Managers
**Total Network LOC**: 7,779

---

## Architecture Overview

The Coinbase Chain network layer is organized into 7 specialized managers, each with a single, well-defined responsibility. This design intentionally decomposes Bitcoin Core's monolithic `PeerManagerImpl` into focused, testable components while maintaining clean interfaces and minimal coupling.

```
NetworkManager (Coordinator)
  ├─> PeerManager (Connection lifecycle + bans)
  ├─> AddrManager (Peer discovery)
  ├─> AnchorManager (Connection reliability)
  ├─> HeaderSyncManager (Initial sync)
  ├─> BlockRelayManager (Block propagation)
  └─> TransactionManager (Mempool + tx relay)
```

---

## 1. NetworkManager

**Purpose**: Top-level coordinator and event dispatcher

**LOC**: ~1,200
**File**: `src/network/network_manager.cpp`

### Responsibilities
- Initialize and shutdown all network subsystems
- Coordinate message routing between managers
- Handle P2P socket I/O (asio integration)
- Dispatch protocol messages to appropriate managers
- Maintain global network state (e.g., network time)
- Provide RPC API for network commands

### Key APIs
```cpp
void Start();
void Stop();
void SendMessage(int peer_id, const Message& msg);
void ProcessIncomingMessage(int peer_id, const Message& msg);
```

### Does NOT Handle
- Peer connection lifecycle (delegated to PeerManager)
- Address book management (delegated to AddrManager)
- Ban/discouragement logic (delegated to PeerManager)
- Block/header synchronization (delegated to HeaderSync/BlockRelay)

### Dependencies
- All other managers (as coordinator)
- boost::asio for network I/O

---

## 2. PeerManager

**Purpose**: Connection lifecycle, permissions, and ban management

**LOC**: ~1,510 (includes former BanMan)
**File**: `src/network/peer_manager.cpp`

### Responsibilities
- Track active peer connections (inbound/outbound/feeler)
- Manage connection slots (max inbound/outbound limits)
- Enforce connection policies (whitelisting, local peer preferences)
- **Ban management** (persistent bans + temporary discouragement)
- Misbehavior tracking and peer eviction
- Permission flags (NoBan, BloomFilter, Relay, etc.)
- Ping/pong keepalive

### Key APIs
```cpp
// Connection lifecycle
int add_peer(PeerPtr peer, bool is_inbound, bool is_feeler);
void remove_peer(int peer_id);
bool can_accept_inbound_from(const std::string& address);

// Ban management (merged from BanMan in Phase 4)
void Ban(const std::string& address, int64_t ban_time_offset = 0);
void Unban(const std::string& address);
bool IsBanned(const std::string& address) const;

void Discourage(const std::string& address);
bool IsDiscouraged(const std::string& address) const;

bool LoadBans(const std::string& datadir);
bool SaveBans();

// Misbehavior
void penalize_peer(int peer_id, int penalty, const std::string& reason);
```

### Ban System Design
**Two-tier system**:
1. **Persistent bans**: Stored on disk (`banlist.json`), survive restarts
2. **Temporary discouragement**: In-memory only, 24-hour duration

**Automatic discouragement**: When misbehavior threshold reached, `remove_peer()` automatically calls `Discourage()` internally.

**Bitcoin Core Compatibility**: Ban and whitelist are **independent states**. Whitelisted addresses CAN be banned; whitelist only affects connection acceptance, not ban queries or operations.

### Does NOT Handle
- Address book/discovery (delegated to AddrManager)
- Message content validation (delegated to protocol-specific managers)
- Block/transaction relay decisions (delegated to HeaderSync/BlockRelay/TxManager)

### Phase 4 Changes
- **Merged with BanMan** (391 LOC eliminated)
- Ban persistence logic moved from separate BanMan class
- Fixed whitelist behavior to match Bitcoin Core (ban/whitelist independence)

---

## 3. AddrManager

**Purpose**: Peer discovery and address book management

**LOC**: ~800
**File**: `src/network/addr_manager.cpp`

### Responsibilities
- Maintain address book (~2000 known peer addresses)
- Implement tried/new bucket algorithm (Bitcoin Core compatible)
- Select peers for outbound connections based on address quality
- Handle ADDR/GETADDR messages
- Persist address book to disk
- Implement address selection bias (prefer diverse subnets)

### Key APIs
```cpp
void Add(const std::vector<CAddress>& addresses, const std::string& source);
CAddress Select(bool new_only = false);
void Good(const CAddress& addr);
void Attempt(const CAddress& addr);
std::vector<CAddress> GetAddr(size_t max_addresses, size_t max_pct);
```

### Does NOT Handle
- Anchor peer persistence (delegated to AnchorManager)
- Connection establishment (delegated to PeerManager)
- Ban/whitelist checks (delegated to PeerManager)

### Phase 4 Investigation
Evaluated for merge with AnchorManager. **Decision: KEEP SEPARATE**
- 0 cross-calls (completely independent)
- 13% code duplication (below 20% threshold)
- Different lifecycles: continuous vs transient
- Different purposes: security vs reliability

---

## 4. AnchorManager

**Purpose**: Connection reliability through anchor peer persistence

**LOC**: ~300
**File**: `src/network/anchor_manager.cpp`

### Responsibilities
- Save ~2 best anchor addresses on shutdown (oldest READY outbound peers)
- Load anchor addresses on startup for initial connections
- Delete anchor file after loading (one-time use)
- Whitelist loaded anchor addresses (NoBan protection)

### Key APIs
```cpp
bool SaveAnchors(const std::string& path);
bool LoadAnchors(const std::string& path);
```

### Lifecycle
**Transient**: Only active during startup and shutdown
- **Startup**: `LoadAnchors()` → attempt connections → delete file
- **Runtime**: Inactive (no continuous operation)
- **Shutdown**: `SaveAnchors()` → persist 2 oldest outbound peers

### Does NOT Handle
- Address quality assessment (uses addresses from AddrManager indirectly)
- Connection establishment (delegated to PeerManager)
- Address book management (delegated to AddrManager)

### Phase 4 Investigation
Evaluated for merge with AddrManager. **Decision: KEEP SEPARATE**
- 0 cross-calls (orthogonal concerns)
- Different lifecycles: transient vs continuous
- Small, focused, easy to understand

---

## 5. HeaderSyncManager

**Purpose**: Header synchronization during Initial Block Download (IBD)

**LOC**: ~769
**File**: `src/network/header_sync_manager.cpp`

### Responsibilities
- Manage header-only synchronization from sync peer
- Request headers via GETHEADERS messages
- Validate header chain (PoW, timestamps, difficulty)
- Track sync state (syncing, synced, stale)
- Detect and handle sync peer misbehavior
- Switch sync peers on timeout or failure

### Key APIs
```cpp
void StartHeaderSync(int peer_id);
void ProcessHeaders(int peer_id, const std::vector<BlockHeader>& headers);
bool IsSynced() const;
int GetSyncPeer() const;
```

### Lifecycle
**Stateful synchronization orchestration**:
- Active during IBD (Initial Block Download)
- Manages sync peer selection and tracking
- Coordinates multi-round header fetching

### Does NOT Handle
- Full block download (delegated to BlockRelayManager)
- Block relay after sync complete (delegated to BlockRelayManager)
- Ban decisions (delegated to PeerManager via automatic discouragement)

### Phase 4 Changes
- **Removed redundant BanMan discouragement calls**: HeaderSyncManager no longer directly calls `ban_man_.Discourage()` after misbehavior detection. Instead, `remove_peer()` handles discouragement automatically.

### Phase 4 Investigation
Evaluated for merge with BlockRelayManager. **Decision: KEEP SEPARATE**
- 4 cross-calls (below threshold of 5)
- 5-8% code duplication (below 20% threshold)
- Different lifecycles: stateful sync vs stateless relay
- Intentional decomposition of Bitcoin Core monolith

---

## 6. BlockRelayManager

**Purpose**: Block announcement and relay

**LOC**: ~367
**File**: `src/network/block_relay_manager.cpp`

### Responsibilities
- Handle INV/GETDATA for block announcements
- Relay blocks to peers
- Validate blocks before relay
- Implement compact block relay (future)
- Coordinate with HeaderSyncManager for block requests

### Key APIs
```cpp
void ProcessInv(int peer_id, const std::vector<CInv>& inv);
void ProcessBlock(int peer_id, const Block& block);
void RelayBlock(const Block& block);
```

### Lifecycle
**Stateless relay/validation**:
- No sync state tracking
- Pure relay logic (announce, request, validate, forward)
- Active throughout node lifetime

### Does NOT Handle
- Header synchronization (delegated to HeaderSyncManager)
- Sync peer selection (delegated to HeaderSyncManager)
- Block validation consensus rules (delegated to chain layer)

### Phase 4 Investigation
Evaluated for merge with HeaderSyncManager. **Decision: KEEP SEPARATE**
- Minimal coupling (4 cross-calls)
- Different responsibilities: sync orchestration vs relay
- Current separation improves testability

---

## 7. TransactionManager

**Purpose**: Mempool management and transaction relay

**LOC**: ~833
**File**: `src/network/transaction_manager.cpp`

### Responsibilities
- Maintain mempool (unconfirmed transaction pool)
- Handle INV/GETDATA for transaction announcements
- Relay transactions to peers
- Validate transactions before relay
- Fee estimation
- Transaction eviction policies

### Key APIs
```cpp
void ProcessTx(int peer_id, const Transaction& tx);
void RelayTx(const Transaction& tx);
void AddToMempool(const Transaction& tx);
void RemoveFromMempool(const TxHash& hash);
```

### Does NOT Handle
- Transaction consensus validation (delegated to chain layer)
- Peer banning (delegated to PeerManager)

---

## Manager Interaction Patterns

### Initialization Flow
```
NetworkManager::Start()
  ├─> PeerManager::Initialize()
  │     └─> LoadBans(datadir)
  ├─> AddrManager::Load(datadir)
  ├─> AnchorManager::LoadAnchors(datadir)
  │     └─> PeerManager::AddToWhitelist(anchor_addr)  # Whitelist anchors
  └─> HeaderSyncManager, BlockRelayManager, TransactionManager ready
```

### Connection Establishment Flow
```
NetworkManager receives inbound connection
  └─> PeerManager::can_accept_inbound_from(address)
        ├─> Check IsBanned(address)           # Ban check
        ├─> Check IsDiscouraged(address)      # Discourage check
        ├─> Check IsWhitelisted(address)      # Whitelist override
        ├─> Check connection slots            # Max connections
        └─> Return accept/reject decision
```

### Misbehavior Flow
```
HeaderSyncManager detects bad headers
  └─> PeerManager::penalize_peer(peer_id, 100, "bad-headers")
        ├─> Accumulate misbehavior score
        ├─> If threshold exceeded:
        │     ├─> Check NetPermissionFlags::NoBan
        │     └─> If not whitelisted: Discourage(address)
        └─> PeerManager::remove_peer(peer_id)
              └─> Calls Discourage() internally if threshold reached
```

### Address Discovery Flow
```
NetworkManager receives ADDR message
  └─> AddrManager::Add(addresses, source_peer)
        └─> Update tried/new buckets

NetworkManager needs outbound connection
  └─> AddrManager::Select()
        └─> Return best address from tried/new buckets
```

---

## Phase 4 Evolution

### Before Phase 4 (8 Managers)
1. NetworkManager
2. PeerManager
3. **BanMan** ← DELETED
4. AddrManager
5. AnchorManager
6. HeaderSyncManager
7. BlockRelayManager
8. TransactionManager

### After Phase 4 (7 Managers)
1. NetworkManager
2. **PeerManager** (now includes ban management)
3. AddrManager
4. AnchorManager
5. HeaderSyncManager
6. BlockRelayManager
7. TransactionManager

### Key Changes
- **BanMan → PeerManager**: Eliminated 391 LOC
- **Investigated HeaderSync + BlockRelay merge**: Kept separate (intentional decomposition)
- **Investigated AddrManager + AnchorManager merge**: Kept separate (orthogonal concerns)
- **Fixed Bitcoin Core compatibility**: Ban/whitelist independence

---

## Design Principles

### 1. Single Responsibility
Each manager has one clear purpose. No manager should handle multiple unrelated concerns.

### 2. Minimal Coupling
Managers interact through well-defined APIs. Cross-manager calls are minimized and documented.

### 3. Testability
Each manager can be unit tested independently with minimal mocking.

### 4. Bitcoin Core Compatibility
Design patterns match Bitcoin Core where appropriate, while improving modularity.

### 5. Clear Lifecycle Boundaries
Managers have distinct lifecycles:
- **Continuous**: PeerManager, AddrManager, HeaderSync, BlockRelay, TxManager
- **Transient**: AnchorManager (startup/shutdown only)
- **Coordinator**: NetworkManager (entire node lifetime)

---

## Future Considerations

### Potential Evolution
1. **Compact Block Relay**: May require BlockRelayManager expansion
2. **Erlay**: May require new TxRelayManager split from TransactionManager
3. **UTREEXO**: May require new AccumulatorManager
4. **P2P Encryption**: May require new EncryptionManager or integration into PeerManager

### Merge Criteria for Future Changes
Only merge managers if:
- **Cross-calls > 5**: High coupling suggests merge
- **Code duplication > 20%**: Significant shared logic suggests merge
- **Race conditions**: Shared state across managers suggests merge
- **Same lifecycle**: Similar initialization/operation patterns

---

## Summary

The 7-manager architecture represents an optimal balance:
- **vs Bitcoin Core**: More modular (decomposed monolithic PeerManagerImpl)
- **vs Original Plan**: More managers than target (4-5), but each with clear purpose
- **Result**: Better testability, clarity, and maintainability

**Total Network Layer**: 7,779 LOC across 7 focused managers
**Phase 4 Reduction**: 391 LOC eliminated (BanMan merge)
**Test Coverage**: 599 tests, 16,439 assertions, 100% passing
