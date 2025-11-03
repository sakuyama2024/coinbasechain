# Network Architecture Refactoring Proposal

**Date:** 2025-11-03
**Goal:** Clean up NetworkManager architecture with clear responsibilities and dependencies
**Status:** Proposal for Review

---

## Executive Summary

**Current State:** NetworkManager is a god object doing coordinator + I/O + policy + persistence (500+ lines).
**Target State:** Clean 3-manager architecture using existing NetworkNotifications singleton, clear boundaries, zero circular dependencies.
**Migration:** 4 phases, each independently testable, ~16-24 hours total effort.

---

## Current Problems (Critical)

### Problem 1: NetworkManager Does Everything
```cpp
// NetworkManager.start() - lines 236-256
std::string anchors_path = config_.datadir + "/anchors.json";  // File I/O
if (std::filesystem::exists(anchors_path)) {                    // Filesystem
  auto anchor_addrs = discovery_manager_->LoadAnchors();        // Discovery
  for (const auto& addr : anchor_addrs) {
    auto ip_opt = network_address_to_string(addr);              // Conversion
    peer_manager_->AddToWhitelist(*ip_opt);                     // Policy
    connect_to_with_permissions(addr, NetPermissionFlags::NoBan); // Connection
  }
}
```

**Issues:**
- NetworkManager knows filesystem layout
- NetworkManager manipulates PeerLifecycleManager internals (AddToWhitelist)
- NetworkManager does PeerDiscoveryManager's job (anchor loading)
- NetworkManager does PeerLifecycleManager's job (connection policy)

### Problem 2: Circular Dependencies
```cpp
// PeerLifecycleManager needs PeerDiscoveryManager
peer_manager_ = std::make_unique<PeerLifecycleManager>(io_context_);

// PeerDiscoveryManager needs PeerLifecycleManager
discovery_manager_ = std::make_unique<PeerDiscoveryManager>(peer_manager_.get());

// Inject back (anti-pattern!)
peer_manager_->SetDiscoveryManager(discovery_manager_.get());
```

**Issues:**
- Two-phase initialization (fragile)
- Breaks RAII
- Testing requires injection setup
- No clear dependency direction

### Problem 3: 169-Line Constructor
- Message handler registration: 60 lines
- Component creation: 30 lines
- Dependency wiring: 20 lines
- Persistence setup: 15 lines
- I/O setup: 25 lines
- Mixed concerns throughout

---

## Target Architecture

### Principle: Event-Driven Decoupling via NetworkNotifications Singleton

```
NetworkManager (Coordinator ONLY)
│
├─ PeerLifecycleManager (Peer Lifecycle)
│  ├─ Owns: PeerRegistry
│  ├─ Owns: BanDatabase
│  ├─ Owns: MisbehaviorTracker
│  └─ Publishes events → NetworkNotifications::Get() (singleton)
│
├─ PeerDiscoveryManager (Address Discovery & Persistence)
│  ├─ Owns: AddressManager
│  ├─ Owns: AnchorManager
│  ├─ Subscribes to ← NetworkNotifications::Get() (singleton)
│  └─ NO dependencies on other managers! Pure discovery logic
│
├─ BlockchainSyncManager (Blockchain Sync)
│  ├─ Owns: HeaderSyncManager
│  └─ Owns: BlockRelayManager
│
└─ NetworkNotifications (Singleton - Already Exists!)
   ├─ Events: PeerConnected, PeerDisconnected, Misbehavior, etc.
   ├─ Thread-safe with RAII subscriptions
   └─ Pattern: Matches ChainNotifications
```

### Dependency Flow (Acyclic!)

```
PeerLifecycleManager
   └─ publishes events to → NetworkNotifications (singleton)
                                     ↓
                            PeerDiscoveryManager subscribes
                                     ↓
                               (no reverse deps!)
```

### Responsibilities Matrix

| Component | Owns | Does | Does NOT Do |
|-----------|------|------|-------------|
| **NetworkManager** | Managers, timers, transport | Coordinate, schedule, route | I/O, policy, persistence |
| **PeerLifecycleManager** | Peer registry, bans | Accept/reject, evict, track DoS, publish events | Address selection, file I/O |
| **PeerDiscoveryManager** | Addresses, anchors, subscriptions | Select addresses, persist, handle events | Connect peers, enforce limits |
| **BlockchainSyncManager** | Sync components | Route sync messages | Direct peer access |
| **NetworkNotifications** | Subscribers | Dispatch events | Business logic |

---

## Phase 1: Break Circular Dependency + Rename Components (CRITICAL)

**Goal:**
1. Remove `SetDiscoveryManager()` injection pattern using existing NetworkNotifications singleton
2. Rename components for clarity (ConnectionManager → PeerLifecycleManager, etc.)

**Effort:** 6-8 hours
**Risk:** Low (using existing infrastructure)

### Step 1.1: Update PeerDiscoveryManager to Subscribe to NetworkNotifications

**Rename:** `discovery_manager.hpp` → `peer_discovery_manager.hpp`

**File:** `include/network/peer_discovery_manager.hpp`

```cpp
#pragma once

#include <memory>
#include <optional>
#include <string>
#include "network/protocol.hpp"
#include "network/notifications.hpp"  // For NetworkNotifications

namespace coinbasechain {
namespace network {

class AddressManager;
class AnchorManager;

/**
 * PeerDiscoveryManager - Pure address discovery and persistence
 *
 * Responsibilities:
 * - Select addresses for outbound connections
 * - Track address quality (good/bad/attempted)
 * - Load/save anchors (persistent outbound peers)
 * - Handle ADDR/GETADDR protocol messages
 * - Subscribe to peer connection events via NetworkNotifications
 *
 * Key: NO compile-time dependencies on PeerLifecycleManager!
 * Updates come via NetworkNotifications singleton (runtime event subscription).
 */
class PeerDiscoveryManager {
public:
  explicit PeerDiscoveryManager(const std::string& datadir = "");
  ~PeerDiscoveryManager();

  // Address quality updates (called by event handlers)
  void MarkAttempted(const protocol::NetworkAddress& addr);
  void MarkGood(const protocol::NetworkAddress& addr);
  void MarkBad(const protocol::NetworkAddress& addr);

  // Address selection
  std::optional<protocol::NetworkAddress> SelectAddress();

  // Protocol message handlers
  void HandleAddr(PeerPtr peer, message::AddrMessage* msg);
  void HandleGetAddr(PeerPtr peer);

  // Anchor operations (self-contained persistence)
  void LoadAndConnectAnchors(
      std::function<void(const protocol::NetworkAddress&)> connect_callback);
  void SaveAnchors(const std::vector<protocol::NetworkAddress>& anchors);

private:
  std::string datadir_;  // Owns persistence path

  std::unique_ptr<AddressManager> addr_manager_;
  std::unique_ptr<AnchorManager> anchor_manager_;

  // Event subscriptions (RAII - automatically unsubscribe on destruction)
  NetworkNotifications::Subscription peer_connected_sub_;
  NetworkNotifications::Subscription peer_disconnected_sub_;
};

} // namespace network
} // namespace coinbasechain
```

**Implementation:** `src/network/peer_discovery_manager.cpp`

```cpp
#include "network/peer_discovery_manager.hpp"
#include "network/address_manager.hpp"
#include "network/anchor_manager.hpp"
#include "util/logging.hpp"

namespace coinbasechain {
namespace network {

PeerDiscoveryManager::PeerDiscoveryManager(const std::string& datadir)
    : datadir_(datadir),
      addr_manager_(std::make_unique<AddressManager>()),
      anchor_manager_(std::make_unique<AnchorManager>()) {

  // Subscribe to peer connection events from NetworkNotifications singleton
  peer_connected_sub_ = NetworkEvents().SubscribePeerConnected(
      [this](int peer_id, const std::string& address, const std::string& type) {
        // Convert address string to NetworkAddress
        auto addr = StringToNetworkAddress(address);
        if (addr) {
          MarkGood(*addr);
        }
      });

  peer_disconnected_sub_ = NetworkEvents().SubscribePeerDisconnected(
      [this](int peer_id, const std::string& address, const std::string& reason) {
        // Only mark as bad if disconnect was abnormal
        if (reason != "normal" && reason != "shutdown") {
          auto addr = StringToNetworkAddress(address);
          if (addr) {
            MarkBad(*addr);
          }
        }
      });

  LOG_NET_INFO("PeerDiscoveryManager initialized, subscribed to NetworkNotifications");
}

PeerDiscoveryManager::~PeerDiscoveryManager() {
  // Subscriptions automatically unsubscribe via RAII
  LOG_NET_INFO("PeerDiscoveryManager destroyed, unsubscribed from NetworkNotifications");
}

void PeerDiscoveryManager::MarkGood(const protocol::NetworkAddress& addr) {
  if (addr_manager_) {
    addr_manager_->Good(addr);
    LOG_NET_TRACE("Marked address as good: {}", NetworkAddressToString(addr));
  }
}

void PeerDiscoveryManager::MarkBad(const protocol::NetworkAddress& addr) {
  if (addr_manager_) {
    addr_manager_->Bad(addr);
    LOG_NET_TRACE("Marked address as bad: {}", NetworkAddressToString(addr));
  }
}

// ... rest of implementation
}
```

### Step 1.2: Rename and Update PeerLifecycleManager to Publish Events

**Rename:** `connection_manager.hpp` → `peer_lifecycle_manager.hpp`

**File:** `include/network/peer_lifecycle_manager.hpp`

```cpp
#pragma once

#include <memory>
#include <boost/asio/io_context.hpp>
#include "network/peer.hpp"
#include "network/notifications.hpp"  // For NetworkNotifications

namespace coinbasechain {
namespace network {

class BanDatabase;

/**
 * PeerLifecycleManager - Manages peer connection lifecycle
 *
 * Responsibilities:
 * - Add/remove peers
 * - Ban/unban addresses
 * - Track connection limits (inbound/outbound)
 * - Misbehavior tracking and eviction
 * - Publish connection events to NetworkNotifications singleton
 *
 * Renamed from: ConnectionManager (clearer responsibility)
 */
class PeerLifecycleManager {
public:
  explicit PeerLifecycleManager(
      boost::asio::io_context& io_context,
      const std::string& datadir = "");

  // Remove SetDiscoveryManager() method entirely!

  // Peer operations
  int add_peer(TransportConnectionPtr connection,
               const protocol::NetworkAddress& addr,
               bool inbound);
  void remove_peer(int peer_id);

  // Ban operations (delegates to BanDatabase)
  void Ban(const std::string& address, int64_t duration_seconds);
  bool IsBanned(const std::string& address) const;

  // Query methods
  size_t GetOutboundCount() const;
  std::vector<PeerPtr> GetOutboundPeers() const;

private:
  boost::asio::io_context& io_context_;
  std::unique_ptr<BanDatabase> ban_database_;
  // ... peer registry, etc.
};
```

**Implementation:** `src/network/peer_lifecycle_manager.cpp`

```cpp
int PeerLifecycleManager::add_peer(
    TransportConnectionPtr connection,
    const protocol::NetworkAddress& addr,
    bool inbound) {

  // ... create peer ...

  // Publish PeerConnected event to NetworkNotifications singleton
  if (peer->successfully_connected()) {
    std::string addr_str = NetworkAddressToString(addr);
    std::string type = inbound ? "inbound" : "outbound";
    NetworkEvents().NotifyPeerConnected(peer_id, addr_str, type);
  }

  return peer_id;
}

void PeerLifecycleManager::remove_peer(int peer_id) {
  // ... find peer ...

  std::string peer_address = peer->address();
  std::string reason = peer->was_punished() ? "misbehavior" : "normal";

  // ... remove peer ...

  // Publish PeerDisconnected event to NetworkNotifications singleton
  NetworkEvents().NotifyPeerDisconnected(peer_id, peer_address, reason);
}
```

### Step 1.3: Update NetworkManager Construction (Simpler!)

**File:** `src/network/network_manager.cpp`

```cpp
NetworkManager::NetworkManager(
    validation::ChainstateManager& chainstate_manager,
    const Config& config,
    std::shared_ptr<Transport> transport,
    boost::asio::io_context* external_io_context)
    : config_(config),
      chainstate_manager_(chainstate_manager),
      // ... transport setup ...
{
  // 1. Create PeerDiscoveryManager (subscribes to NetworkNotifications in constructor)
  peer_discovery_ = std::make_unique<PeerDiscoveryManager>(config_.datadir);

  // 2. Create PeerLifecycleManager (publishes to NetworkNotifications)
  peer_lifecycle_ = std::make_unique<PeerLifecycleManager>(
      io_context_,
      config_.datadir);

  // NO circular dependency!
  // NO SetDiscoveryManager() call!
  // NO mediator class needed!
  // Events flow through NetworkNotifications singleton

  // Rest of initialization...
}
```

**Benefits:**
- ✅ No circular dependency (acyclic graph!)
- ✅ PeerDiscoveryManager has ZERO compile-time dependencies on PeerLifecycleManager
- ✅ Uses existing NetworkNotifications infrastructure (no new classes!)
- ✅ Thread-safe via NetworkNotifications mutex
- ✅ RAII subscriptions prevent leaks
- ✅ Consistent with ChainNotifications pattern
- ✅ Easy to add more subscribers (metrics, logging, etc.)

### Step 1.4: Rename Files (Preserve Git History)

```bash
# Rename header files
git mv include/network/connection_manager.hpp \
       include/network/peer_lifecycle_manager.hpp

git mv include/network/discovery_manager.hpp \
       include/network/peer_discovery_manager.hpp

git mv include/network/sync_manager.hpp \
       include/network/blockchain_sync_manager.hpp

# Rename implementation files
git mv src/network/connection_manager.cpp \
       src/network/peer_lifecycle_manager.cpp

git mv src/network/discovery_manager.cpp \
       src/network/peer_discovery_manager.cpp

git mv src/network/sync_manager.cpp \
       src/network/blockchain_sync_manager.cpp

# Update all references (29 files)
find include src -type f \( -name "*.hpp" -o -name "*.cpp" \) \
  -exec sed -i '' \
    -e 's/ConnectionManager/PeerLifecycleManager/g' \
    -e 's/DiscoveryManager/PeerDiscoveryManager/g' \
    -e 's/SyncManager/BlockchainSyncManager/g' \
    -e 's/connection_manager\.hpp/peer_lifecycle_manager.hpp/g' \
    -e 's/discovery_manager\.hpp/peer_discovery_manager.hpp/g' \
    -e 's/sync_manager\.hpp/blockchain_sync_manager.hpp/g' \
    {} +
```

**Phase 1 Complete - Benefits:**
- ✅ No circular dependency (acyclic graph!)
- ✅ RAII construction (fully initialized in constructor)
- ✅ PeerDiscoveryManager has ZERO compile-time dependencies
- ✅ Uses existing NetworkNotifications infrastructure (no new classes!)
- ✅ Thread-safe event system with RAII subscriptions
- ✅ Easy to add more subscribers (metrics, logging, etc.)
- ✅ Consistent with ChainNotifications pattern (same codebase style)
- ✅ Clear, descriptive component names

---

## Phase 2: Extract PeerDiscoveryManager Persistence Logic

**Goal:** Move file I/O and policy out of NetworkManager
**Effort:** 4-6 hours
**Risk:** Low

### Step 2.1: Move Anchor Logic to PeerDiscoveryManager

**Before (NetworkManager):**
```cpp
// NetworkManager.start() - WRONG
std::string anchors_path = config_.datadir + "/anchors.json";
if (std::filesystem::exists(anchors_path)) {
  auto anchor_addrs = peer_discovery_->LoadAnchors(anchors_path);
  for (const auto& addr : anchor_addrs) {
    auto ip_opt = network_address_to_string(addr);
    peer_lifecycle_->AddToWhitelist(*ip_opt);
    connect_to_with_permissions(addr, NetPermissionFlags::NoBan);
  }
}
```

**After (NetworkManager):**
```cpp
// NetworkManager.start() - CORRECT
peer_discovery_->LoadAndConnectAnchors(
    [this](const protocol::NetworkAddress& addr) {
      // NetworkManager only provides connection primitive
      connect_to_with_permissions(addr, NetPermissionFlags::NoBan);
    });
```

**Implementation (PeerDiscoveryManager):**
```cpp
void PeerDiscoveryManager::LoadAndConnectAnchors(
    std::function<void(const protocol::NetworkAddress&)> connect_callback) {

  if (datadir_.empty()) return;

  std::string anchors_path = datadir_ + "/anchors.json";
  if (!std::filesystem::exists(anchors_path)) {
    LOG_NET_DEBUG("No anchors file at {}", anchors_path);
    return;
  }

  auto anchor_addrs = anchor_manager_->LoadAnchors(anchors_path);

  if (anchor_addrs.empty()) {
    LOG_NET_DEBUG("No anchors loaded from {}", anchors_path);
    return;
  }

  LOG_NET_TRACE("Loaded {} anchors, connecting...", anchor_addrs.size());

  for (const auto& addr : anchor_addrs) {
    // Discovery knows about NoBan policy for anchors
    connect_callback(addr);
  }
}

void PeerDiscoveryManager::SaveAnchors(
    const std::vector<protocol::NetworkAddress>& anchors) {
  if (datadir_.empty()) return;

  std::string anchors_path = datadir_ + "/anchors.json";

  if (anchor_manager_->SaveAnchors(anchors_path, anchors)) {
    LOG_NET_TRACE("Saved {} anchors to {}", anchors.size(), anchors_path);
  } else {
    LOG_NET_ERROR("Failed to save anchors to {}", anchors_path);
  }
}
```

**Benefits:**
- ✅ NetworkManager doesn't know about `anchors.json`
- ✅ NetworkManager doesn't manipulate whitelist
- ✅ PeerDiscoveryManager encapsulates all persistence
- ✅ Clear boundary: PeerDiscoveryManager = policy, NetworkManager = execution

### Step 2.2: Move Ban Persistence to PeerLifecycleManager

**Extract BanDatabase class:**

**New file:** `include/network/ban_database.hpp`

```cpp
#pragma once

#include <map>
#include <unordered_set>
#include <string>

namespace coinbasechain {
namespace network {

struct CBanEntry {
  int64_t ban_until_time;
  int64_t ban_created_time;
  std::string reason;
};

/**
 * BanDatabase - Pure storage and persistence for peer bans
 *
 * Renamed from: BanManager (it's storage, not a manager)
 */
class BanDatabase {
public:
  explicit BanDatabase(const std::string& datadir = "");

  // Ban operations
  void Ban(const std::string& address, int64_t duration_seconds, const std::string& reason = "");
  void Unban(const std::string& address);
  bool IsBanned(const std::string& address) const;

  // Discourage operations
  void Discourage(const std::string& address);
  bool IsDiscouraged(const std::string& address) const;

  // Whitelist operations
  void AddToWhitelist(const std::string& address);
  bool IsWhitelisted(const std::string& address) const;

  // Persistence
  bool LoadBans();
  bool SaveBans();

private:
  std::string datadir_;
  std::map<std::string, CBanEntry> banned_;
  std::map<std::string, int64_t> discouraged_;
  std::unordered_set<std::string> whitelist_;

  mutable std::mutex mutex_;
};

} // namespace network
} // namespace coinbasechain
```

**Update PeerLifecycleManager:**

```cpp
class PeerLifecycleManager {
public:
  explicit PeerLifecycleManager(
      boost::asio::io_context& io_context,
      PeerConnectionNotifications& notifications,
      const std::string& datadir = "");  // For ban persistence

  // Ban operations delegate to BanDatabase
  void Ban(const std::string& address, int64_t duration, const std::string& reason = "");
  bool IsBanned(const std::string& address) const;
  // ...

  BanDatabase& GetBanDatabase() { return ban_database_; }

private:
  PeerConnectionNotifications& notifications_;
  std::unique_ptr<BanDatabase> ban_database_;
  // ...
};
```

**Update NetworkManager:**

```cpp
// NetworkManager.start() - OLD (WRONG)
if (!config_.datadir.empty()) {
  peer_lifecycle_->LoadBans(config_.datadir);
}

// NetworkManager.start() - NEW (CORRECT)
peer_lifecycle_->GetBanDatabase().LoadBans();

// NetworkManager.stop() - OLD (WRONG)
if (!config_.datadir.empty()) {
  peer_lifecycle_->SaveBans(config_.datadir);
}

// NetworkManager.stop() - NEW (CORRECT)
peer_lifecycle_->GetBanDatabase().SaveBans();
```

**Benefits:**
- ✅ NetworkManager doesn't know about `banlist.json`
- ✅ Clear ownership: PeerLifecycleManager owns BanDatabase
- ✅ BanDatabase independently testable
- ✅ Reduces PeerLifecycleManager API by 10 methods
- ✅ Clear naming: "Database" for storage, "Manager" for coordination

---

## Phase 3: Extract Message Handler Registry

**Goal:** Remove 60 lines from NetworkManager constructor
**Effort:** 2-4 hours
**Risk:** Low (pure extraction)

### Step 3.1: Create NetworkMessageRegistry

**New file:** `include/network/network_message_registry.hpp`

```cpp
#pragma once

#include "network/message_dispatcher.hpp"
#include "network/peer_discovery_manager.hpp"
#include "network/peer_lifecycle_manager.hpp"
#include "network/blockchain_sync_manager.hpp"

namespace coinbasechain {
namespace network {

/**
 * NetworkMessageRegistry - Centralized protocol message routing
 *
 * Extracts 60+ lines of handler registration from NetworkManager constructor.
 * Makes message routing logic testable and maintainable.
 */
class NetworkMessageRegistry {
public:
  /**
   * Register all protocol message handlers with the dispatcher.
   *
   * This centralizes message routing logic that was previously
   * scattered in NetworkManager constructor.
   */
  static void RegisterHandlers(
      MessageDispatcher& dispatcher,
      PeerDiscoveryManager& discovery,
      PeerLifecycleManager& lifecycle,
      BlockchainSyncManager& sync);
};

} // namespace network
} // namespace coinbasechain
```

**Implementation:** `src/network/network_message_registry.cpp`

```cpp
#include "network/network_message_registry.hpp"
#include "network/message.hpp"
#include "network/protocol.hpp"
#include "util/logging.hpp"

namespace coinbasechain {
namespace network {

void NetworkMessageRegistry::RegisterHandlers(
    MessageDispatcher& dispatcher,
    PeerDiscoveryManager& discovery,
    PeerLifecycleManager& lifecycle,
    BlockchainSyncManager& sync) {

  // ADDR - Address discovery
  dispatcher.RegisterHandler(protocol::commands::ADDR,
    [&discovery](PeerPtr peer, message::Message* msg) {
      auto* addr_msg = dynamic_cast<message::AddrMessage*>(msg);
      if (!addr_msg) {
        LOG_NET_ERROR("Bad ADDR payload from peer {}", peer ? peer->id() : -1);
        return false;
      }
      return discovery.HandleAddr(peer, addr_msg);
    });

  // GETADDR - Address request
  dispatcher.RegisterHandler(protocol::commands::GETADDR,
    [&discovery](PeerPtr peer, message::Message* msg) {
      discovery.HandleGetAddr(peer);
      return true;
    });

  // VERACK - Version acknowledgment
  dispatcher.RegisterHandler(protocol::commands::VERACK,
    [&lifecycle](PeerPtr peer, message::Message* msg) {
      if (!peer || !peer->successfully_connected()) {
        LOG_NET_TRACE("Ignoring VERACK from pre-VERACK peer");
        return true;
      }
      return lifecycle.HandleVerack(peer);
    });

  // HEADERS - Block headers
  dispatcher.RegisterHandler(protocol::commands::HEADERS,
    [&sync](PeerPtr peer, message::Message* msg) {
      auto* headers_msg = dynamic_cast<message::HeadersMessage*>(msg);
      if (!headers_msg) {
        LOG_NET_ERROR("Bad HEADERS payload from peer {}", peer ? peer->id() : -1);
        return false;
      }
      if (!peer || !peer->successfully_connected()) {
        LOG_NET_TRACE("Ignoring HEADERS from pre-VERACK peer");
        return true;
      }
      return sync.HandleHeaders(peer, headers_msg);
    });

  // GETHEADERS - Header request
  dispatcher.RegisterHandler(protocol::commands::GETHEADERS,
    [&sync](PeerPtr peer, message::Message* msg) {
      auto* getheaders_msg = dynamic_cast<message::GetHeadersMessage*>(msg);
      if (!getheaders_msg) {
        LOG_NET_ERROR("Bad GETHEADERS payload from peer {}", peer ? peer->id() : -1);
        return false;
      }
      if (!peer || !peer->successfully_connected()) {
        LOG_NET_TRACE("Ignoring GETHEADERS from pre-VERACK peer");
        return true;
      }
      return sync.HandleGetHeaders(peer, getheaders_msg);
    });

  // INV - Inventory announcement
  dispatcher.RegisterHandler(protocol::commands::INV,
    [&sync](PeerPtr peer, message::Message* msg) {
      auto* inv_msg = dynamic_cast<message::InvMessage*>(msg);
      if (!inv_msg) {
        LOG_NET_ERROR("Bad INV payload from peer {}", peer ? peer->id() : -1);
        return false;
      }
      return sync.HandleInv(peer, inv_msg);
    });

  LOG_NET_INFO("Registered {} message handlers with MessageDispatcher",
               dispatcher.GetRegisteredCommands().size());
}

} // namespace network
} // namespace coinbasechain
```

### Step 3.2: Update NetworkManager Constructor

**Before (169 lines):**
```cpp
NetworkManager::NetworkManager(...) {
  // ... 30 lines of component creation ...

  // 60 lines of handler registration
  message_dispatcher_->RegisterHandler(protocol::commands::ADDR,
    [this](PeerPtr peer, message::Message* msg) {
      auto* addr_msg = dynamic_cast<message::AddrMessage*>(msg);
      // ... etc
    });
  // ... 5 more handlers ...

  // ... rest of init ...
}
```

**After (~40 lines):**
```cpp
NetworkManager::NetworkManager(...) {
  InitializeTransport();
  CreateManagers();
  RegisterMessageHandlers();
  // Note: LoadPersistentData() called in start(), not constructor
}

void NetworkManager::RegisterMessageHandlers() {
  NetworkMessageRegistry::RegisterHandlers(
      *message_dispatcher_,
      *peer_discovery_,
      *peer_lifecycle_,
      *blockchain_sync_);
}
```

**Benefits:**
- ✅ Constructor: 169 → ~40 lines
- ✅ Handler logic testable independently
- ✅ Easy to add new handlers
- ✅ Clear separation: construction vs configuration

---

## Phase 4: Refactor NetworkManager Constructor

**Goal:** Break constructor into clear phases
**Effort:** 4-6 hours
**Risk:** Medium

### Step 4.1: Extract Phase Methods

**File:** `include/network/network_manager.hpp`

```cpp
class NetworkManager {
private:
  // Construction phases (called from constructor)
  void InitializeTransport();
  void CreateManagers();
  void RegisterMessageHandlers();
  void LoadPersistentData();

  // ... rest ...
};
```

**File:** `src/network/network_manager.cpp`

```cpp
NetworkManager::NetworkManager(
    validation::ChainstateManager& chainstate_manager,
    const Config& config,
    std::shared_ptr<Transport> transport,
    boost::asio::io_context* external_io_context)
    : config_(config),
      local_nonce_(protocol::GenerateNonce()),
      chainstate_manager_(chainstate_manager),
      owned_io_context_(external_io_context ? nullptr : std::make_unique<boost::asio::io_context>()),
      io_context_(external_io_context ? *external_io_context : *owned_io_context_) {

  InitializeTransport();
  CreateManagers();
  RegisterMessageHandlers();
  // Note: LoadPersistentData() called in start(), not constructor
}

void NetworkManager::InitializeTransport() {
  if (!transport_) {
    transport_ = std::make_shared<RealTransport>(io_context_);
  }

  // Setup inbound connection handler
  transport_->set_connection_callback(
      [this](TransportConnectionPtr connection) {
        handle_inbound_connection(connection);
      });
}

void NetworkManager::CreateManagers() {
  // 1. PeerDiscoveryManager (subscribes to NetworkNotifications in constructor)
  peer_discovery_ = std::make_unique<PeerDiscoveryManager>(
      config_.datadir);

  // 2. PeerLifecycleManager (publishes to NetworkNotifications)
  peer_lifecycle_ = std::make_unique<PeerLifecycleManager>(
      io_context_,
      config_.datadir);

  // 3. BlockchainSyncManager components
  auto header_sync = std::make_unique<HeaderSyncManager>(
      chainstate_manager_, *peer_lifecycle_);
  auto block_relay = std::make_unique<BlockRelayManager>(
      chainstate_manager_, *peer_lifecycle_, header_sync.get());
  blockchain_sync_ = std::make_unique<BlockchainSyncManager>(
      std::move(header_sync), std::move(block_relay));

  // 4. MessageDispatcher
  message_dispatcher_ = std::make_unique<MessageDispatcher>();

  // 5. NATManager (optional)
  if (config_.enable_nat) {
    nat_manager_ = std::make_unique<NATManager>();
  }
}

void NetworkManager::RegisterMessageHandlers() {
  NetworkMessageRegistry::RegisterHandlers(
      *message_dispatcher_,
      *discovery_manager_,
      *peer_manager_,
      *sync_manager_);
}

void NetworkManager::LoadPersistentData() {
  // Called from start(), not constructor
  peer_lifecycle_->GetBanDatabase().LoadBans();
  // Anchors loaded via peer_discovery_->LoadAndConnectAnchors() in start()
}
```

**Benefits:**
- ✅ Constructor: clear, ~20 lines
- ✅ Each phase independently testable
- ✅ Easy to understand initialization order
- ✅ Persistence separated from construction

---

## Migration Plan Summary

### Phase 1: Break Circular Dependency + Rename (6-8 hours)
1. Update PeerDiscoveryManager to subscribe to NetworkNotifications (singleton)
2. Update PeerLifecycleManager to publish to NetworkNotifications (singleton)
3. Rename `ConnectionManager` → `PeerLifecycleManager`
4. Rename `DiscoveryManager` → `PeerDiscoveryManager`
5. Rename `SyncManager` → `BlockchainSyncManager`
6. Remove `SetDiscoveryManager()` anti-pattern
7. Test event subscriptions work correctly

### Phase 2: Extract Persistence (4-6 hours)
1. Move anchor loading to PeerDiscoveryManager
2. Extract BanDatabase class from PeerLifecycleManager
3. Update NetworkManager to delegate to managers
4. Test persistence roundtrip

### Phase 3: Extract Message Registry (2-4 hours)
1. Create `NetworkMessageRegistry` class
2. Move 60 lines of handler registration
3. Update NetworkManager constructor
4. Test message routing

### Phase 4: Refactor Constructor (4-6 hours)
1. Extract phase methods (InitializeTransport, CreateManagers, etc.)
2. Test each phase independently
3. Measure constructor reduction (169 → ~30 lines)

**Total Effort:** 16-24 hours
**Result:** Clean architecture, current 5-7/10 → target 9/10

---

## Validation Checklist

### Before Starting
- [ ] All 569 tests passing
- [ ] Git branch created: `refactor/clean-network-architecture`
- [ ] Architecture review document read

### After Each Phase
- [ ] All 569 tests still passing
- [ ] No new compiler warnings
- [ ] Constructor line count measured
- [ ] Circular dependency check (include-what-you-use)
- [ ] Git commit created with clear message

### After Completion
- [ ] Constructor < 50 lines (target: ~30)
- [ ] No circular dependencies (acyclic graph)
- [ ] No `SetDiscoveryManager()` anti-pattern
- [ ] NetworkNotifications event system in use (already exists!)
- [ ] PeerDiscoveryManager subscribes to events
- [ ] PeerLifecycleManager publishes events
- [ ] NetworkManager doesn't know about file formats
- [ ] All persistence encapsulated in managers
- [ ] Clear, descriptive component names (PeerLifecycleManager, PeerDiscoveryManager, etc.)

---

## Decision Log

### Why NetworkNotifications Singleton Instead of Custom Mediator or Interface?
**Decision:** Use existing NetworkNotifications singleton, NOT create new PeerConnectionNotifications class
**Reasoning:**
- ✅ **Already exists** - NetworkNotifications is already implemented for this exact purpose
- ✅ **Explicit design goal** - Comments say "Breaks circular dependencies between network managers"
- ✅ **Consistent pattern** - Matches ChainNotifications (same codebase style)
- ✅ **Thread-safe** - Already has mutex protection
- ✅ **RAII subscriptions** - Prevents memory leaks automatically
- ✅ **Zero compile-time deps** - PeerDiscoveryManager has no dependency on PeerLifecycleManager
- ✅ **Extensible** - Easy to add more subscribers (metrics, logging) without changes
- ✅ **No new code** - Reuse existing infrastructure
- ✅ **Bitcoin-like** - Similar to Bitcoin Core's ValidationInterface pattern

### Why Rename to PeerLifecycleManager vs Keep ConnectionManager?
**Decision:** Rename to PeerLifecycleManager
**Reasoning:**
- "ConnectionManager" too generic (connections to what?)
- "PeerLifecycleManager" explicitly describes responsibility (peer lifecycle: add, remove, ban, evict)
- Consistent naming with PeerDiscoveryManager (both manage peer-related concerns)
- Educational codebase benefits from explicit names

### Why Extract BanDatabase vs Keep in PeerLifecycleManager?
**Decision:** Extract separate BanDatabase class
**Reasoning:**
- Clear single responsibility (pure storage)
- Independently testable
- Reduces PeerLifecycleManager complexity (40+ → 30 methods)
- Natural boundary (persistence + policy)
- Consistent naming: "Database" for storage, "Manager" for coordination

### Why NetworkMessageRegistry Static Method vs Instance?
**Decision:** Static method
**Reasoning:**
- No state needed
- Simple dependency passing
- Easy to test
- Clear one-time setup

---

## Risk Mitigation

### Risk: Breaking Existing Tests
**Mitigation:**
- Run full test suite after each phase
- Commit after each phase
- Can rollback individual phases if needed

### Risk: Performance Regression
**Mitigation:**
- Interface calls are zero-cost abstraction
- No event dispatch overhead
- Measure connection setup latency before/after

### Risk: Missing Edge Cases
**Mitigation:**
- Keep existing behavior for Phase 1-3
- Only refactor structure, not logic
- Extensive comments on moved code

---

## Success Metrics

| Metric | Current | Target | How to Measure |
|--------|---------|--------|----------------|
| Constructor lines | 169 | < 50 (target: ~30) | `wc -l` on constructor |
| Circular deps | 1 (ConnectionManager ↔ DiscoveryManager) | 0 (acyclic graph) | include-what-you-use |
| PeerLifecycleManager methods | 40+ | < 30 | Count public methods |
| NetworkManager file I/O | Yes (anchors, bans) | No (delegated) | Grep for filesystem |
| PeerDiscoveryManager deps | 1 (PeerLifecycleManager) | 0 (none!) | Check constructor |
| Event system usage | No | Yes (NetworkNotifications) | Check subscriptions |
| Test pass rate | 100% (569/569) | 100% (569/569) | Run test suite |

---

## Next Steps

1. **Review this proposal** with team/stakeholders
2. **Create git branch** `refactor/clean-network-architecture`
3. **Start with Phase 1** (highest impact, breaks circular dependency)
4. **Commit after each phase** for incremental progress
5. **Measure improvement** against success metrics

**Questions? Concerns? Feedback?**
