# Network Notifications Design
**Date**: 2025-11-02
**Status**: Proposed
**Based On**: Existing `ChainNotifications` pattern

---

## Overview

Instead of creating a new "EventBus", we should **reuse the existing observer pattern** from `ChainNotifications` for network events. This maintains consistency across the codebase and avoids reinventing the wheel.

---

## Existing Pattern: ChainNotifications

### Current Implementation

```cpp
// include/chain/notifications.hpp
class ChainNotifications {
public:
    // RAII subscription handle
    class Subscription {
        ~Subscription(); // Auto-unsubscribes
    };

    // Subscribe to events
    [[nodiscard]] Subscription SubscribeBlockConnected(BlockConnectedCallback callback);
    [[nodiscard]] Subscription SubscribeChainTip(ChainTipCallback callback);

    // Publish events
    void NotifyBlockConnected(const CBlockHeader& block, const CBlockIndex* pindex);
    void NotifyChainTip(const CBlockIndex* pindexNew, int height);

    // Singleton accessor
    static ChainNotifications& Get();

private:
    std::mutex mutex_;
    std::vector<CallbackEntry> callbacks_;
    size_t next_id_{1};
};
```

### Key Features ✅

1. **RAII Subscriptions**: Automatic cleanup when subscriber is destroyed
2. **Thread-Safe**: Uses `std::mutex` to protect callback list
3. **Type-Safe**: Each event type has its own callback signature
4. **Synchronous**: Callbacks executed immediately (no async queue)
5. **Singleton**: Global access via `Notifications()` helper
6. **Simple**: ~200 LOC, easy to understand

### Why This is Better Than Generic EventBus

```cpp
// Generic EventBus (what I proposed):
event_bus.Publish<InvalidHeaderEvent>({peer_id, hash, reason});
                  ^^^^^^^^^^^^^^^^^ Type parameter (verbose)

// ChainNotifications pattern (existing):
notifications.NotifyInvalidHeader(peer_id, hash, reason);
              ^^^^^^^^^^^^^^^^^^^ Named method (clear)
```

**Advantages**:
- Named methods are more discoverable
- No template magic, easier to debug
- Consistent with existing codebase
- Better IDE autocomplete support

---

## Proposed: NetworkNotifications

### Design (Mirror ChainNotifications)

```cpp
// include/network/notifications.hpp
#pragma once

#include "network/peer.hpp"
#include "util/uint.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace coinbasechain {
namespace network {

/**
 * Notification system for network events
 *
 * Design philosophy (same as ChainNotifications):
 * - Simple observer pattern with std::function
 * - Thread-safe using std::mutex
 * - No background queue (synchronous callbacks)
 * - Type-safe callbacks
 * - RAII-based subscription management
 *
 * Events:
 * - PeerConnected: New peer connection established
 * - PeerDisconnected: Peer disconnected or removed
 * - InvalidHeader: Invalid header received from peer
 * - LowWorkHeaders: Headers with insufficient work
 * - MisbehaviorDetected: Generic misbehavior event
 * - BlockAnnouncement: New block announced via INV
 */
class NetworkNotifications {
public:
    /**
     * Subscription handle - RAII wrapper
     * Automatically unsubscribes when destroyed
     */
    class Subscription {
    public:
        Subscription() = default;
        ~Subscription();

        // Movable but not copyable
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        // Unsubscribe explicitly
        void Unsubscribe();

    private:
        friend class NetworkNotifications;
        Subscription(NetworkNotifications* owner, size_t id);

        NetworkNotifications* owner_{nullptr};
        size_t id_{0};
        bool active_{false};
    };

    // Callback types
    using PeerConnectedCallback = std::function<void(
        int peer_id,
        const std::string& address,
        uint16_t port,
        bool is_inbound
    )>;

    using PeerDisconnectedCallback = std::function<void(
        int peer_id,
        const std::string& address,
        const std::string& reason
    )>;

    using InvalidHeaderCallback = std::function<void(
        int peer_id,
        const uint256& header_hash,
        const std::string& reason
    )>;

    using LowWorkHeadersCallback = std::function<void(
        int peer_id,
        size_t header_count,
        const std::string& reason
    )>;

    using MisbehaviorCallback = std::function<void(
        int peer_id,
        const std::string& violation_type,
        int penalty
    )>;

    using BlockAnnouncementCallback = std::function<void(
        const uint256& block_hash,
        int announcing_peer_id
    )>;

    // === Subscription Methods ===

    /**
     * Subscribe to peer connected events
     * Returns RAII subscription handle
     */
    [[nodiscard]] Subscription
    SubscribePeerConnected(PeerConnectedCallback callback);

    /**
     * Subscribe to peer disconnected events
     * Returns RAII subscription handle
     */
    [[nodiscard]] Subscription
    SubscribePeerDisconnected(PeerDisconnectedCallback callback);

    /**
     * Subscribe to invalid header events
     * Returns RAII subscription handle
     */
    [[nodiscard]] Subscription
    SubscribeInvalidHeader(InvalidHeaderCallback callback);

    /**
     * Subscribe to low-work headers events
     * Returns RAII subscription handle
     */
    [[nodiscard]] Subscription
    SubscribeLowWorkHeaders(LowWorkHeadersCallback callback);

    /**
     * Subscribe to misbehavior events
     * Returns RAII subscription handle
     */
    [[nodiscard]] Subscription
    SubscribeMisbehavior(MisbehaviorCallback callback);

    /**
     * Subscribe to block announcement events
     * Returns RAII subscription handle
     */
    [[nodiscard]] Subscription
    SubscribeBlockAnnouncement(BlockAnnouncementCallback callback);

    // === Notification Methods ===

    /**
     * Notify all subscribers of peer connected
     * Called by PeerManager when new peer is added
     */
    void NotifyPeerConnected(int peer_id, const std::string& address,
                            uint16_t port, bool is_inbound);

    /**
     * Notify all subscribers of peer disconnected
     * Called by PeerManager when peer is removed
     */
    void NotifyPeerDisconnected(int peer_id, const std::string& address,
                               const std::string& reason);

    /**
     * Notify all subscribers of invalid header
     * Called by HeaderSyncManager when header validation fails
     */
    void NotifyInvalidHeader(int peer_id, const uint256& header_hash,
                            const std::string& reason);

    /**
     * Notify all subscribers of low-work headers
     * Called by HeaderSyncManager during anti-DoS work check
     */
    void NotifyLowWorkHeaders(int peer_id, size_t header_count,
                             const std::string& reason);

    /**
     * Notify all subscribers of misbehavior
     * Called by various managers when protocol violations detected
     */
    void NotifyMisbehavior(int peer_id, const std::string& violation_type,
                          int penalty);

    /**
     * Notify all subscribers of block announcement
     * Called by BlockRelayManager when INV received
     */
    void NotifyBlockAnnouncement(const uint256& block_hash, int announcing_peer_id);

    /**
     * Get singleton instance
     */
    static NetworkNotifications& Get();

private:
    NetworkNotifications() = default;

    // Unsubscribe by ID (called by Subscription destructor)
    void Unsubscribe(size_t id);

    struct CallbackEntry {
        size_t id;
        PeerConnectedCallback peer_connected;
        PeerDisconnectedCallback peer_disconnected;
        InvalidHeaderCallback invalid_header;
        LowWorkHeadersCallback low_work_headers;
        MisbehaviorCallback misbehavior;
        BlockAnnouncementCallback block_announcement;
    };

    std::mutex mutex_;
    std::vector<CallbackEntry> callbacks_;
    size_t next_id_{1}; // 0 reserved for invalid
};

/**
 * Global accessor for network notifications
 * Consistent with ChainNotifications pattern
 */
inline NetworkNotifications& NetworkEvents() {
    return NetworkNotifications::Get();
}

} // namespace network
} // namespace coinbasechain
```

---

## Usage Examples

### Example 1: PeerManager Publishes Disconnect Event

```cpp
// src/network/peer_manager.cpp
void PeerManager::remove_peer(int peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = peers_.find(peer_id);
    if (it == peers_.end()) {
        return;
    }

    PeerPtr peer = it->second;
    std::string address = peer->address();

    // Disconnect peer
    peer->disconnect();
    peers_.erase(it);

    // Publish notification (replaces callback)
    NetworkEvents().NotifyPeerDisconnected(peer_id, address, "user_initiated");
}
```

### Example 2: HeaderSyncManager Publishes Invalid Header

```cpp
// src/network/header_sync_manager.cpp
bool HeaderSyncManager::HandleHeaders(PeerPtr peer, HeadersMessage* msg) {
    // ... validation logic ...

    if (!validation::CheckHeaderPoW(header, params)) {
        // OLD WAY (circular dependency):
        // peer_manager_.ReportInvalidPoW(peer->id());

        // NEW WAY (notification):
        NetworkEvents().NotifyInvalidHeader(
            peer->id(),
            header.GetHash(),
            "PoW check failed"
        );

        return false;
    }

    // ... rest of logic ...
}
```

### Example 3: PeerManager Subscribes to Invalid Header

```cpp
// src/network/peer_manager.cpp
PeerManager::PeerManager(boost::asio::io_context& io_context,
                        AddressManager& addr_manager,
                        const Config& config)
    : io_context_(io_context),
      addr_manager_(addr_manager),
      config_(config) {

    // Subscribe to invalid header events
    invalid_header_subscription_ = NetworkEvents().SubscribeInvalidHeader(
        [this](int peer_id, const uint256& hash, const std::string& reason) {
            // Apply misbehavior penalty
            Misbehaving(peer_id, MisbehaviorPenalty::INVALID_POW, reason);
        }
    );

    // Subscribe to low-work headers
    low_work_subscription_ = NetworkEvents().SubscribeLowWorkHeaders(
        [this](int peer_id, size_t count, const std::string& reason) {
            Misbehaving(peer_id, MisbehaviorPenalty::LOW_WORK_HEADERS, reason);
        }
    );
}

// Subscriptions automatically cleaned up in destructor
PeerManager::~PeerManager() {
    // RAII: subscriptions unsubscribe automatically
}
```

### Example 4: Application Layer Logs All Events

```cpp
// src/application.cpp
class Application {
public:
    Application() {
        // Subscribe to all network events for logging
        peer_connected_sub_ = NetworkEvents().SubscribePeerConnected(
            [](int peer_id, const std::string& addr, uint16_t port, bool inbound) {
                LOG_NET_INFO("Peer {} connected: {}:{} ({})",
                           peer_id, addr, port, inbound ? "inbound" : "outbound");
            }
        );

        peer_disconnected_sub_ = NetworkEvents().SubscribePeerDisconnected(
            [](int peer_id, const std::string& addr, const std::string& reason) {
                LOG_NET_INFO("Peer {} disconnected: {} (reason: {})",
                           peer_id, addr, reason);
            }
        );

        misbehavior_sub_ = NetworkEvents().SubscribeMisbehavior(
            [](int peer_id, const std::string& type, int penalty) {
                LOG_NET_WARN("Peer {} misbehaved: {} (penalty: {})",
                           peer_id, type, penalty);
            }
        );
    }

private:
    NetworkNotifications::Subscription peer_connected_sub_;
    NetworkNotifications::Subscription peer_disconnected_sub_;
    NetworkNotifications::Subscription misbehavior_sub_;
};
```

---

## Migration Strategy

### Phase 1: Implement NetworkNotifications (1 week)

```cpp
// 1. Create include/network/notifications.hpp (interface)
// 2. Create src/network/notifications.cpp (implementation)
// 3. Add unit tests for subscription/notification
// 4. No changes to existing code yet
```

**Deliverable**: `NetworkNotifications` class ready to use

### Phase 2: Dual-Run (1 week)

```cpp
// Run both old callbacks AND new notifications
void PeerManager::remove_peer(int peer_id) {
    // ... removal logic ...

    // OLD: Call callback
    if (peer_disconnect_callback_) {
        peer_disconnect_callback_(peer_id);
    }

    // NEW: Publish notification
    NetworkEvents().NotifyPeerDisconnected(peer_id, address, reason);
}

// Verify both systems produce same results
```

**Deliverable**: Both systems running in parallel, behavior verified

### Phase 3: Migrate Subscribers (2 weeks)

```cpp
// Week 1: Migrate HeaderSyncManager
HeaderSyncManager::HeaderSyncManager(...) {
    // OLD: Store reference to PeerManager (circular dep)
    // peer_manager_(peer_manager)

    // NEW: Subscribe to notifications
    peer_disconnected_sub_ = NetworkEvents().SubscribePeerDisconnected(
        [this](int peer_id, ...) { OnPeerDisconnected(peer_id); }
    );
}

// Week 2: Migrate BlockRelayManager, MessageRouter
```

**Deliverable**: All subscribers migrated to notifications

### Phase 4: Remove Old Callbacks (1 week)

```cpp
// Delete old callback infrastructure
class PeerManager {
    // ❌ DELETE
    std::function<void(int)> peer_disconnect_callback_;
    void SetPeerDisconnectCallback(...);
};

// HeaderSyncManager no longer has PeerManager reference
class HeaderSyncManager {
    // ❌ DELETE
    PeerManager& peer_manager_;
};
```

**Deliverable**: Circular dependencies eliminated

---

## Benefits Over Generic EventBus

### 1. Consistency with Existing Code ✅

```cpp
// Chain notifications
ChainNotifications::Get().NotifyBlockConnected(block, pindex);
ChainNotifications::Get().SubscribeBlockConnected(callback);

// Network notifications (same pattern!)
NetworkNotifications::Get().NotifyPeerDisconnected(peer_id, addr, reason);
NetworkNotifications::Get().SubscribePeerDisconnected(callback);
```

**Same API style** across the entire codebase.

### 2. Better Type Safety ✅

```cpp
// Generic EventBus: Easy to get event data wrong
event_bus.Publish<InvalidHeader>({
    peer_id,
    hash,
    reason,
    penalty  // ← WRONG ORDER! Compiles but broken
});

// NetworkNotifications: Named parameters, compiler-checked
notifications.NotifyInvalidHeader(
    peer_id,
    hash,
    reason
    // penalty missing → COMPILER ERROR ✅
);
```

### 3. Better IDE Support ✅

```
User types: NetworkEvents().
                           ^
                           |
IDE autocomplete shows:
  - NotifyPeerConnected()
  - NotifyPeerDisconnected()
  - NotifyInvalidHeader()
  - SubscribePeerConnected()
  - SubscribePeerDisconnected()
  - SubscribeInvalidHeader()
```

**Discoverability** - users can see all available events.

### 4. Easier to Debug ✅

```cpp
// Generic EventBus call stack:
EventBus::Publish<InvalidHeader>()
  → EventBus::DispatchToHandlers()
    → std::function::operator()()
      → PeerManager::lambda

// NetworkNotifications call stack:
NetworkNotifications::NotifyInvalidHeader()  ← Named function!
  → PeerManager::lambda

// Clearer what's happening in debugger
```

### 5. No Template Metaprogramming ✅

```cpp
// Generic EventBus: Template magic
template<typename EventType>
void Publish(const EventType& event) {
    auto type_id = std::type_index(typeid(EventType));
    // ... template machinery ...
}

// NetworkNotifications: Plain C++
void NotifyInvalidHeader(int peer_id, const uint256& hash, ...) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : callbacks_) {
        if (entry.invalid_header) {
            entry.invalid_header(peer_id, hash, ...);
        }
    }
}

// Simpler = easier to maintain
```

---

## Comparison: EventBus vs NetworkNotifications

| Aspect | Generic EventBus | NetworkNotifications |
|--------|------------------|---------------------|
| **Consistency** | New pattern | Matches ChainNotifications ✅ |
| **Type safety** | Template parameters | Named parameters ✅ |
| **Discoverability** | Poor (must know event types) | Excellent (IDE autocomplete) ✅ |
| **Debugging** | Template stack traces | Clear call stack ✅ |
| **Code size** | ~300 LOC (generic) | ~250 LOC (specific) ✅ |
| **Learning curve** | Higher (templates) | Lower (simple pattern) ✅ |
| **Flexibility** | Can add events easily | Can add events easily ✅ |

**Winner: NetworkNotifications** (reuses existing pattern, simpler, more consistent)

---

## Implementation Checklist

### Files to Create
- [ ] `include/network/notifications.hpp` (interface)
- [ ] `src/network/notifications.cpp` (implementation)
- [ ] `test/network/notifications_tests.cpp` (unit tests)

### Files to Modify
- [ ] `src/network/peer_manager.cpp` (publish PeerDisconnected, subscribe to violations)
- [ ] `src/network/header_sync_manager.cpp` (publish InvalidHeader, subscribe to PeerDisconnected)
- [ ] `src/network/block_relay_manager.cpp` (publish BlockAnnouncement, subscribe to PeerDisconnected)
- [ ] `src/network/message_router.cpp` (subscribe to PeerDisconnected)
- [ ] `include/network/peer_manager.hpp` (remove callback infrastructure)
- [ ] `include/network/header_sync_manager.hpp` (remove PeerManager reference)

### Tests to Add
- [ ] Subscription RAII cleanup
- [ ] Multiple subscribers to same event
- [ ] Thread safety (concurrent publish/subscribe)
- [ ] Unsubscribe during notification
- [ ] Verify all event types

---

## Updated Refactoring Timeline

### Original Plan (with EventBus)
- Phase 4: Implement EventBus - **10 days**

### Revised Plan (with NetworkNotifications)
- Phase 4: Implement NetworkNotifications - **5 days** ✅

**Savings**: 5 days (because we're reusing existing pattern, not inventing new one)

---

## Example: Full Migration for PeerDisconnected

### Before (Callback Hell)

```cpp
// network_manager.cpp
peer_manager_->SetPeerDisconnectCallback([this](int peer_id) {
    if (header_sync_manager_) {
        header_sync_manager_->OnPeerDisconnected(peer_id);
    }
    if (block_relay_manager_) {
        block_relay_manager_->OnPeerDisconnected(peer_id);
    }
    if (message_router_) {
        message_router_->OnPeerDisconnected(peer_id);
    }
});
```

**Problems**:
- NetworkManager must know about all managers
- Adding new manager requires code change
- Circular dependencies everywhere

### After (NetworkNotifications)

```cpp
// peer_manager.cpp - Publisher
void PeerManager::remove_peer(int peer_id) {
    // ... removal logic ...
    NetworkEvents().NotifyPeerDisconnected(peer_id, address, reason);
}

// header_sync_manager.cpp - Subscriber
HeaderSyncManager::HeaderSyncManager(...) {
    peer_disconnected_sub_ = NetworkEvents().SubscribePeerDisconnected(
        [this](int peer_id, ...) { OnPeerDisconnected(peer_id); }
    );
}

// block_relay_manager.cpp - Subscriber
BlockRelayManager::BlockRelayManager(...) {
    peer_disconnected_sub_ = NetworkEvents().SubscribePeerDisconnected(
        [this](int peer_id, ...) { OnPeerDisconnected(peer_id); }
    );
}

// message_router.cpp - Subscriber
MessageRouter::MessageRouter(...) {
    peer_disconnected_sub_ = NetworkEvents().SubscribePeerDisconnected(
        [this](int peer_id, ...) { OnPeerDisconnected(peer_id); }
    );
}
```

**Benefits**:
- NetworkManager doesn't wire anything ✅
- Adding new subscriber is trivial ✅
- No circular dependencies ✅
- Each component only knows about NetworkNotifications ✅

---

## Conclusion

**Recommendation**: Use `NetworkNotifications` (mirroring `ChainNotifications`) instead of generic `EventBus`.

**Why**:
1. ✅ Consistency with existing codebase
2. ✅ Simpler implementation (less code)
3. ✅ Better type safety and IDE support
4. ✅ Easier to debug (no templates)
5. ✅ Familiar pattern for team
6. ✅ Faster to implement (5 days vs 10 days)

**Updated Action Plan**: Replace Phase 4 "Event Bus" with Phase 4 "Network Notifications" (same benefits, less complexity).
