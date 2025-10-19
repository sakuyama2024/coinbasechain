# Code Review: `attempt_outbound_connections()`

**Date:** 2025-10-19
**Function:** `NetworkManager::attempt_outbound_connections()`
**File:** `src/network/network_manager.cpp:383-409`
**Severity:** 🔴 **CRITICAL** - Node cannot bootstrap via peer discovery

---

## Summary

**This function is BROKEN and has never worked.** It's a stub with incomplete IP address conversion, preventing the node from making automatic outbound connections via the address manager. The node can only connect via:

1. ✅ Manual RPC commands (`addnode`)
2. ✅ Hardcoded seed nodes (DNS/IP seeds)
3. ✅ Anchor reconnections (on restart)
4. ❌ **Peer discovery via ADDR messages** ← BROKEN

This is a **deployment blocker** for any production network.

---

## Current Implementation

```cpp
void NetworkManager::attempt_outbound_connections() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  // Check if we need more outbound connections
  while (peer_manager_->needs_more_outbound()) {
    // Select an address from the address manager
    auto maybe_addr = addr_manager_->select();
    if (!maybe_addr) {
      break; // No addresses available
    }

    auto &addr = *maybe_addr;

    // Convert IP to string
    std::string ip_str;
    // TODO: Proper IP conversion from addr.ip array
    // For now, just skip - this will be implemented when we have real addresses

    // Mark as attempt
    addr_manager_->attempt(addr);

    // Try to connect (this will be properly implemented when IP conversion is
    // done) connect_to(ip_str, addr.port);
  }
}
```

---

## Issues Identified

### 🔴 **CRITICAL: Empty IP String**

```cpp
std::string ip_str;
// TODO: Proper IP conversion from addr.ip array
// For now, just skip - this will be implemented when we have real addresses
```

**Problem:**
- `ip_str` is declared but **never assigned**
- Remains empty string `""`
- `connect_to(ip_str, addr.port)` is called with empty IP

**Impact:**
- `connect_to("", 9590)` will fail immediately
- No outbound connections via peer discovery
- Node cannot bootstrap without manual intervention

---

### 🔴 **CRITICAL: Incorrect Resource Accounting**

```cpp
// Mark as attempt
addr_manager_->attempt(addr);

// Try to connect (this will be properly implemented when IP conversion is
// done) connect_to(ip_str, addr.port);
```

**Problem:**
- `attempt(addr)` marks address as "tried" **before** connection succeeds
- Connection never happens (empty IP)
- Address gets permanently marked as "attempted" with no actual connection
- Wastes addresses in the address manager

**Correct Order:**
1. Convert IP to string
2. Call `connect_to()` - it internally handles success/failure
3. Let `connect_to()` mark the address as attempted (if it does)

Actually, looking at `connect_to()` implementation (lines 172-266), it doesn't call `addr_manager_->attempt()` or `addr_manager_->good()`. This is missing!

---

### 🔴 **CRITICAL: Commented-Out connect_to() Call**

```cpp
// Try to connect (this will be properly implemented when IP conversion is
// done) connect_to(ip_str, addr.port);
```

**Problem:**
This is NOT commented out in the actual code - the comment just says "TODO", but the call happens with empty string. Even worse!

---

### 🟡 **MODERATE: IP Conversion Already Implemented**

**The IP conversion logic ALREADY EXISTS in the same file!**

Lines 1253-1266 (anchor loading):

```cpp
// Convert 16-byte array to boost::asio IP address
boost::asio::ip::address_v6::bytes_type bytes;
std::copy(addr.ip.begin(), addr.ip.end(), bytes.begin());
auto v6_addr = boost::asio::ip::make_address_v6(bytes);

// Check if it's IPv4-mapped and convert if needed
std::string ip_str;
if (v6_addr.is_v4_mapped()) {
  ip_str = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped,
                                            v6_addr)
               .to_string();
} else {
  ip_str = v6_addr.to_string();
}
```

**Why wasn't this copied here?**

This exact same code could be used in `attempt_outbound_connections()`.

---

### 🟡 **MODERATE: Missing Error Handling**

No try/catch block around IP conversion (when implemented):

```cpp
try {
  // IP conversion
} catch (const std::exception &e) {
  LOG_NET_WARN("Failed to convert address: {}", e.what());
  addr_manager_->failed(addr);
  continue;
}
```

The anchor code (lines 1252-1276) has proper exception handling.

---

### 🟡 **MODERATE: Infinite Loop Potential**

If `addr_manager_->select()` keeps returning the same address (with empty IP):

```cpp
while (peer_manager_->needs_more_outbound()) {
  auto maybe_addr = addr_manager_->select();
  if (!maybe_addr) {
    break; // ✅ Good - exits if no addresses
  }

  // But if select() keeps returning same address...
  // and connect_to() fails (empty IP)...
  // loop continues forever
}
```

**Mitigation:**
- The `if (!maybe_addr)` check prevents truly infinite loops
- But wastes CPU if address manager has addresses but they're all invalid

---

### 🟢 **LOW: Missing Feedback to AddressManager**

`connect_to()` doesn't call back to `addr_manager_->good()` or `addr_manager_->failed()`:

```cpp
// In connect_to() callback (lines 224-234):
if (success) {
  peer_ptr->start();
  // ❌ MISSING: addr_manager_->good(addr);
} else {
  peer_manager_->remove_peer(peer_id);
  // ❌ MISSING: addr_manager_->failed(addr);
}
```

**Impact:**
- AddressManager never learns which addresses work
- Can't prioritize good addresses
- Can't deprioritize bad addresses

**Problem:** `connect_to()` takes `std::string address`, not `NetworkAddress`, so it can't call back.

---

## Security Implications

### 1. Network Partitioning Risk

**Without working peer discovery:**
- Node relies on seed nodes and manual connections
- Seed nodes are single point of failure
- Attacker can eclipse attack by:
  1. Taking down seed nodes
  2. Controlling manual connections
  3. No fallback via ADDR messages

**Attack Cost:** Very cheap - just DDoS the seed nodes

---

### 2. Sybil Attack Amplification

**Without address quality tracking:**
- `addr_manager_->good()` never called
- All addresses treated equally
- Attacker can flood ADDR messages with malicious IPs
- Node wastes time connecting to Sybil nodes

---

### 3. Anchor-Only Dependency

**Current bootstrap paths:**
1. ✅ Anchors (restart only, limited to last 2 peers)
2. ✅ Seeds (hardcoded, centralized)
3. ✅ Manual RPC (requires operator intervention)
4. ❌ **Peer discovery** ← Should be primary method!

**This inverts Bitcoin's trust model:**
- Bitcoin: Distributed peer discovery (ADDR propagation)
- This node: Centralized seeds/anchors

---

## Fix Implementation

### Option A: Simple Fix (Copy from Anchor Code)

```cpp
void NetworkManager::attempt_outbound_connections() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  while (peer_manager_->needs_more_outbound()) {
    auto maybe_addr = addr_manager_->select();
    if (!maybe_addr) {
      break;
    }

    auto &addr = *maybe_addr;

    try {
      // Convert 16-byte array to boost::asio IP address
      boost::asio::ip::address_v6::bytes_type bytes;
      std::copy(addr.ip.begin(), addr.ip.end(), bytes.begin());
      auto v6_addr = boost::asio::ip::make_address_v6(bytes);

      // Check if it's IPv4-mapped and convert if needed
      std::string ip_str;
      if (v6_addr.is_v4_mapped()) {
        ip_str = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped,
                                                  v6_addr)
                     .to_string();
      } else {
        ip_str = v6_addr.to_string();
      }

      LOG_NET_DEBUG("Attempting outbound connection to {}:{}", ip_str, addr.port);

      // Attempt connection
      if (!connect_to(ip_str, addr.port)) {
        LOG_NET_DEBUG("Failed to initiate connection to {}:{}", ip_str, addr.port);
        addr_manager_->failed(addr);
      }
      // Note: addr_manager_->good() should be called in connect_to() callback

    } catch (const std::exception &e) {
      LOG_NET_WARN("Failed to convert address: {}", e.what());
      addr_manager_->failed(addr);
    }
  }
}
```

---

### Option B: Better Fix (Extract Helper Function)

Create a reusable IP conversion helper:

```cpp
// In network_manager.hpp (private section):
std::optional<std::string> network_address_to_string(const protocol::NetworkAddress& addr);

// In network_manager.cpp:
std::optional<std::string> NetworkManager::network_address_to_string(
    const protocol::NetworkAddress& addr) {
  try {
    boost::asio::ip::address_v6::bytes_type bytes;
    std::copy(addr.ip.begin(), addr.ip.end(), bytes.begin());
    auto v6_addr = boost::asio::ip::make_address_v6(bytes);

    if (v6_addr.is_v4_mapped()) {
      return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6_addr)
                 .to_string();
    } else {
      return v6_addr.to_string();
    }
  } catch (const std::exception &e) {
    LOG_NET_WARN("Failed to convert NetworkAddress to string: {}", e.what());
    return std::nullopt;
  }
}

void NetworkManager::attempt_outbound_connections() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  while (peer_manager_->needs_more_outbound()) {
    auto maybe_addr = addr_manager_->select();
    if (!maybe_addr) {
      break;
    }

    auto &addr = *maybe_addr;
    auto maybe_ip_str = network_address_to_string(addr);

    if (!maybe_ip_str) {
      LOG_NET_WARN("Failed to convert address, skipping");
      addr_manager_->failed(addr);
      continue;
    }

    const std::string &ip_str = *maybe_ip_str;
    LOG_NET_DEBUG("Attempting outbound connection to {}:{}", ip_str, addr.port);

    if (!connect_to(ip_str, addr.port)) {
      LOG_NET_DEBUG("Failed to initiate connection to {}:{}", ip_str, addr.port);
      addr_manager_->failed(addr);
    }
  }
}
```

**Benefits:**
- Reusable for anchor loading (DRY principle)
- Centralized error handling
- Type-safe with `std::optional`

---

### Option C: Best Fix (Also Fix connect_to Feedback)

The above fixes still don't address `addr_manager_->good()` never being called.

**Problem:** `connect_to()` takes `std::string address`, not `protocol::NetworkAddress`.

**Solutions:**

#### C1: Add NetworkAddress Parameter

```cpp
bool connect_to(const std::string &address, uint16_t port,
                std::optional<protocol::NetworkAddress> net_addr = std::nullopt);
```

Then in callback:
```cpp
if (success) {
  peer_ptr->start();
  if (net_addr) {
    addr_manager_->good(*net_addr);
  }
} else {
  peer_manager_->remove_peer(peer_id);
  if (net_addr) {
    addr_manager_->failed(*net_addr);
  }
}
```

#### C2: Store NetworkAddress in Peer

```cpp
class Peer {
  std::optional<protocol::NetworkAddress> network_address_;
  // ...
};
```

Then `connect_to()` can store it when creating the peer.

---

## Testing Requirements

After fixing, verify:

### Unit Tests

```cpp
TEST_CASE("attempt_outbound_connections converts IPv4 addresses", "[network]") {
  // 1. Add IPv4 address to addr_manager
  // 2. Call attempt_outbound_connections()
  // 3. Verify connect_to() was called with correct IP string
}

TEST_CASE("attempt_outbound_connections converts IPv6 addresses", "[network]") {
  // Same but with IPv6
}

TEST_CASE("attempt_outbound_connections handles invalid addresses", "[network]") {
  // Add malformed address
  // Verify addr_manager_->failed() is called
}
```

### Integration Tests

```cpp
TEST_CASE("Node bootstraps via peer discovery", "[integration]") {
  // 1. Start node with empty addr_manager
  // 2. Manually connect to seed node
  // 3. Receive ADDR message with peer addresses
  // 4. Verify node automatically connects to discovered peers
  // 5. Verify addr_manager learns which addresses are good
}
```

### Functional Tests

1. **Clean slate bootstrap:**
   - Delete anchors.dat, peers.dat
   - Start node
   - Connect to seed
   - Verify automatic peer discovery

2. **Address quality learning:**
   - Add 100 addresses (mix of good/bad)
   - Verify good addresses get prioritized
   - Verify bad addresses get deprioritized

---

## Comparison to Bitcoin Core

### Bitcoin Core (`net.cpp:ThreadOpenConnections`)

```cpp
void CConnman::ThreadOpenConnections(const std::vector<std::string> &connect) {
    // ...
    while (!interruptNet) {
        // Get address from addrman
        CAddress addr = addrman.Select();

        // Convert to string and connect
        OpenNetworkConnection(addr, false, &grant,
                             addr.ToStringIPPort().c_str(), false);

        // Feedback handled in connection result
    }
}
```

**Key differences:**

| Feature | Bitcoin Core | This Codebase |
|---------|-------------|---------------|
| IP Conversion | `addr.ToStringIPPort()` | ❌ Not implemented |
| Success Feedback | `addrman.Good(addr)` | ❌ Missing |
| Failure Feedback | `addrman.Attempt(addr, fCountFailure)` | ⚠️ Partial (no failure) |
| Error Handling | ✅ Exception handling | ❌ Missing |
| Logging | ✅ Detailed | ⚠️ TODO only |

---

## Recommended Action

### Immediate (1 hour):

1. ✅ Implement Option A (simple copy from anchor code)
2. ✅ Add error handling and logging
3. ✅ Test with manual ADDR injection

### Short-term (4 hours):

1. ✅ Implement Option B (extract helper function)
2. ✅ Refactor anchor loading to use helper
3. ✅ Add unit tests for IP conversion

### Medium-term (8 hours):

1. ✅ Implement Option C (fix addr_manager feedback)
2. ✅ Add integration tests
3. ✅ Verify bootstrap on clean slate

---

## Related Issues

This connects to earlier security analysis in `SECURITY_ADVERSARIAL_ANALYSIS.md`:

- **V1: Sync Stalling** - Made worse by broken peer discovery
- **V2: Orphan Memory** - Can't escape bad peer without alternatives
- **V4: Connection Slot Exhaustion** - Can't discover good peers

**Root Cause:** All assume working peer discovery, which doesn't exist.

---

## Conclusion

This is a **CRITICAL DEPLOYMENT BLOCKER**:

- ❌ Node cannot bootstrap via peer discovery
- ❌ Relies entirely on centralized seeds/manual connections
- ❌ Cannot recover from eclipse attacks
- ❌ Wastes addresses in addr_manager

**Priority:** 🔴 **P0 - Fix immediately before any production deployment**

**Estimated Fix Time:** 1-2 hours (Option A), properly tested in 4-8 hours (Option B+C)

**Impact of Fix:**
- ✅ Enables distributed peer discovery
- ✅ Reduces reliance on seed nodes
- ✅ Improves network resilience
- ✅ Better address quality learning

---

**Status:** 🔴 BROKEN - Never worked, production blocker
**Next Step:** Implement Option A as hot fix, then Option B+C properly
