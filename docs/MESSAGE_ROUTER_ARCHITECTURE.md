# MessageRouter Architecture - Elegant Design

**Date**: 2025-11-03
**Status**: Proposed
**Goal**: Clean protocol dispatch without hardcoded routing

---

## The Problem

Current MessageRouter has hardcoded dispatch:
```cpp
bool MessageRouter::route_message(Peer* peer, const Message& msg) {
    if (msg.command == "verack") return handle_verack(peer);
    if (msg.command == "addr") return handle_addr(peer, msg);
    if (msg.command == "getaddr") return handle_getaddr(peer);
    if (msg.command == "inv") return handle_inv(peer, msg);
    // ... 15 more if statements
}
```

**Issues**:
- Hardcoded dispatch logic
- Tight coupling to all managers
- Can't add new messages without modifying router
- Violates Open/Closed Principle

---

## Elegant Solution: Handler Registry Pattern

### Core Idea
**Managers register handlers for their message types**

```
┌─────────────────────────────────────────────┐
│           NetworkService                    │
│  ┌────────────────────────────────┐        │
│  │     MessageDispatcher          │        │
│  │  (Handler Registry)            │        │
│  │                                 │        │
│  │  map<string, Handler> handlers_ │        │
│  └────────────────────────────────┘        │
└─────────────────────────────────────────────┘
         ▲            ▲            ▲
         │            │            │
    RegisterHandler  RegisterHandler  RegisterHandler
         │            │            │
┌────────┴────┐  ┌───┴─────┐  ┌───┴──────┐
│ Connection  │  │  Sync   │  │Discovery │
│ Manager     │  │ Manager │  │ Manager  │
└─────────────┘  └─────────┘  └──────────┘
```

---

## Design: MessageDispatcher

### Interface

```cpp
// include/network/message_dispatcher.hpp

namespace coinbasechain {
namespace network {

// Forward declarations
class Peer;
namespace message { class Message; }

/**
 * MessageDispatcher - Protocol message routing via handler registry
 *
 * Design:
 * - Zero hardcoded dispatch logic
 * - Managers register handlers for their message types
 * - Thread-safe registration and dispatch
 * - Extensible: new messages = new registration, no code changes
 */
class MessageDispatcher {
public:
  // Handler signature: takes peer + message, returns success
  using MessageHandler = std::function<bool(PeerPtr, message::Message*)>;

  MessageDispatcher() = default;

  /**
   * Register handler for a message command
   * Thread-safe, can be called during initialization
   *
   * Example:
   *   dispatcher.RegisterHandler("verack",
   *     [this](PeerPtr p, message::Message* m) {
   *       return connection_mgr_.HandleVerack(p);
   *     });
   */
  void RegisterHandler(const std::string& command, MessageHandler handler);

  /**
   * Unregister handler (for testing/cleanup)
   */
  void UnregisterHandler(const std::string& command);

  /**
   * Dispatch message to registered handler
   * Returns false if no handler found or handler returns false
   */
  bool Dispatch(PeerPtr peer, const std::string& command, message::Message* msg);

  /**
   * Check if handler exists for command
   */
  bool HasHandler(const std::string& command) const;

  /**
   * Get list of registered commands (for diagnostics)
   */
  std::vector<std::string> GetRegisteredCommands() const;

private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, MessageHandler> handlers_;
};

} // namespace network
} // namespace coinbasechain
```

### Implementation

```cpp
// src/network/message_dispatcher.cpp

#include "network/message_dispatcher.hpp"
#include "network/peer.hpp"
#include "network/message.hpp"
#include "util/logging.hpp"

namespace coinbasechain {
namespace network {

void MessageDispatcher::RegisterHandler(const std::string& command,
                                         MessageHandler handler) {
  std::lock_guard<std::mutex> lock(mutex_);
  handlers_[command] = std::move(handler);
  LOG_NET_DEBUG("Registered handler for command: {}", command);
}

void MessageDispatcher::UnregisterHandler(const std::string& command) {
  std::lock_guard<std::mutex> lock(mutex_);
  handlers_.erase(command);
}

bool MessageDispatcher::Dispatch(PeerPtr peer,
                                  const std::string& command,
                                  message::Message* msg) {
  // Get handler (lock scope minimized)
  MessageHandler handler;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = handlers_.find(command);
    if (it == handlers_.end()) {
      LOG_NET_DEBUG("No handler for command: {} from peer {}",
                    command, peer->id());
      return false;
    }
    handler = it->second;
  }

  // Execute handler (outside lock - handlers may take time)
  try {
    return handler(peer, msg);
  } catch (const std::exception& e) {
    LOG_NET_ERROR("Handler exception for command {}: {}", command, e.what());
    return false;
  }
}

bool MessageDispatcher::HasHandler(const std::string& command) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return handlers_.count(command) > 0;
}

std::vector<std::string> MessageDispatcher::GetRegisteredCommands() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> result;
  result.reserve(handlers_.size());
  for (const auto& [cmd, _] : handlers_) {
    result.push_back(cmd);
  }
  std::sort(result.begin(), result.end());
  return result;
}

} // namespace network
} // namespace coinbasechain
```

---

## Usage: Manager Registration

### NetworkService Setup

```cpp
// In network_manager.cpp (renamed NetworkService)

void NetworkService::Initialize() {
  // Create dispatcher
  message_dispatcher_ = std::make_unique<MessageDispatcher>();

  // Create managers
  connection_mgr_ = std::make_unique<ConnectionManager>(/*...*/);
  sync_mgr_ = std::make_unique<SyncManager>(/*...*/);
  discovery_mgr_ = std::make_unique<DiscoveryManager>(/*...*/);

  // === Register Message Handlers ===

  // ConnectionManager handles connection lifecycle messages
  message_dispatcher_->RegisterHandler("verack",
    [this](PeerPtr p, message::Message* m) {
      return connection_mgr_->HandleVerack(p);
    });

  message_dispatcher_->RegisterHandler("ping",
    [this](PeerPtr p, message::Message* m) {
      return connection_mgr_->HandlePing(p, static_cast<message::PingMessage*>(m));
    });

  message_dispatcher_->RegisterHandler("pong",
    [this](PeerPtr p, message::Message* m) {
      return connection_mgr_->HandlePong(p, static_cast<message::PongMessage*>(m));
    });

  // SyncManager handles blockchain sync messages
  message_dispatcher_->RegisterHandler("inv",
    [this](PeerPtr p, message::Message* m) {
      return sync_mgr_->HandleInv(p, static_cast<message::InvMessage*>(m));
    });

  message_dispatcher_->RegisterHandler("headers",
    [this](PeerPtr p, message::Message* m) {
      return sync_mgr_->HandleHeaders(p, static_cast<message::HeadersMessage*>(m));
    });

  message_dispatcher_->RegisterHandler("getheaders",
    [this](PeerPtr p, message::Message* m) {
      return sync_mgr_->HandleGetHeaders(p, static_cast<message::GetHeadersMessage*>(m));
    });

  // DiscoveryManager handles address discovery messages
  message_dispatcher_->RegisterHandler("addr",
    [this](PeerPtr p, message::Message* m) {
      return discovery_mgr_->HandleAddr(p, static_cast<message::AddrMessage*>(m));
    });

  message_dispatcher_->RegisterHandler("getaddr",
    [this](PeerPtr p, message::Message* m) {
      return discovery_mgr_->HandleGetAddr(p);
    });

  LOG_NET_INFO("Registered {} message handlers",
               message_dispatcher_->GetRegisteredCommands().size());
}
```

### Message Reception (Transport Layer)

```cpp
// In transport.cpp or wherever messages arrive

void Transport::OnMessageReceived(const std::string& command,
                                   std::vector<uint8_t> payload) {
  // Deserialize message
  auto msg = message::Deserialize(command, payload);
  if (!msg) {
    LOG_NET_WARN("Failed to deserialize message: {}", command);
    return;
  }

  // Dispatch via NetworkService
  bool handled = network_service_->DispatchMessage(peer_, command, msg.get());

  if (!handled) {
    // Unknown message or handler returned false
    // Could penalize peer for unknown messages here
    LOG_NET_TRACE("Message not handled: {} from peer {}", command, peer_->id());
  }
}
```

---

## Benefits

### 1. **Zero Hardcoded Dispatch**
No switch statements, no if-else chains. New messages = new registration.

### 2. **Separation of Concerns**
```
MessageDispatcher:  Routes messages (infrastructure)
Managers:           Handle business logic (domain)
```

### 3. **Testability**
```cpp
// Test just the dispatcher
MessageDispatcher dispatcher;
bool called = false;
dispatcher.RegisterHandler("test", [&](auto p, auto m) {
  called = true;
  return true;
});
dispatcher.Dispatch(peer, "test", msg);
REQUIRE(called);

// Test managers in isolation
ConnectionManager mgr;
bool result = mgr.HandleVerack(peer);
REQUIRE(result);
```

### 4. **Extensibility**
Adding a new message type:
```cpp
// Before: Modify MessageRouter (tight coupling)
// After: Just register a handler (zero coupling)

message_dispatcher_->RegisterHandler("newmessage",
  [this](PeerPtr p, message::Message* m) {
    return new_feature_mgr_->HandleNewMessage(p, m);
  });
```

### 5. **Diagnostics**
```cpp
// List all supported messages
auto commands = dispatcher.GetRegisteredCommands();
for (const auto& cmd : commands) {
  LOG_INFO("Supported: {}", cmd);
}
```

### 6. **Bitcoin Core Alignment**
Core's `PeerManager::ProcessMessage()` is similar - single entry point, handler dispatch.

---

## Alternative: Event-Driven (NetworkNotifications)

If you want to go even further with decoupling:

```cpp
// Managers subscribe to message events instead of explicit registration

class NetworkService {
  void OnMessageReceived(PeerPtr peer, const std::string& command, Message* msg) {
    // Publish message event via notifications
    NetworkNotifications::Get().NotifyMessageReceived(peer, command, msg);
  }
};

class SyncManager {
  NetworkNotifications::Subscription inv_sub_;

  SyncManager() {
    // Subscribe to INV messages
    inv_sub_ = NetworkNotifications::Get().SubscribeMessage("inv",
      [this](PeerPtr p, const std::string& cmd, Message* m) {
        return HandleInv(p, static_cast<InvMessage*>(m));
      });
  }
};
```

**Pros**: Even more decoupled, managers self-register
**Cons**: Less explicit, harder to see full routing at a glance

---

## Recommendation: Handler Registry (Not Events)

**Use the Handler Registry pattern** (first design) because:
1. ✅ **Explicit**: All routing visible in NetworkService::Initialize()
2. ✅ **Simple**: Just a map<string, function>, easy to understand
3. ✅ **Performant**: Direct dispatch, no notification overhead
4. ✅ **Debuggable**: Can list all handlers, set breakpoints easily
5. ✅ **Familiar**: Similar to HTTP routers, event dispatchers in other frameworks

Event-driven would be elegant for *cross-cutting concerns* (logging, metrics), but for core protocol dispatch, explicit is better.

---

## Migration Path

### Phase 4.X: Introduce MessageDispatcher

1. **Create MessageDispatcher** (1-2 hours)
   - Write header + implementation
   - Add tests

2. **Add to NetworkService** (1 hour)
   - Create `message_dispatcher_` member
   - Add `DispatchMessage()` wrapper

3. **Register handlers incrementally** (2-3 hours)
   - Start with VERACK (simplest)
   - Then PING/PONG
   - Then sync messages (INV, HEADERS, GETHEADERS)
   - Then discovery messages (ADDR, GETADDR)
   - Delete MessageRouter when done

4. **Update Transport** (1 hour)
   - Call `network_service_->DispatchMessage()` instead of `message_router_->route()`

5. **Delete MessageRouter** (5 min)
   - `git rm include/network/message_router.hpp src/network/message_router.cpp`

**Total**: 1 day of work, clean architecture gain

---

## File Structure (After)

```
include/network/
  message_dispatcher.hpp      ← New (registry)
  network_service.hpp         ← Renamed from network_manager.hpp
  connection_manager.hpp      ← Renamed from peer_manager.hpp
  sync_manager.hpp           ← Merged header_sync + block_relay
  discovery_manager.hpp      ← Merged addr_manager + anchor_manager

src/network/
  message_dispatcher.cpp     ← New
  network_service.cpp
  connection_manager.cpp
  sync_manager.cpp
  discovery_manager.cpp
```

**MessageRouter.hpp/.cpp**: ❌ **DELETED**

---

## Summary

**Elegant Architecture for MessageRouter**:
- Replace with **MessageDispatcher** (handler registry pattern)
- Managers register handlers for their message types
- NetworkService owns dispatcher, wires it up
- Zero hardcoded routing, fully extensible
- Clean separation: dispatch (infra) vs business logic (managers)

**Result**:
```
8 components → 4 components
MessageRouter → MessageDispatcher (simpler, 50 LOC instead of 600)
Tight coupling → Loose coupling via registry
```

This is the missing piece for your elegant 3-manager architecture! 🎯
