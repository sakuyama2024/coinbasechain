# Missing Features from Unicity's CConnman

Unicity's CConnman has **78 methods** and **4,655 lines of code** (net.h + net.cpp).
Our NetworkManager has **~15 methods** and **~400 lines**.

## Major Missing Features:

### 1. **Connection Type Management**
Unicity has multiple connection types:
- `OUTBOUND_FULL_RELAY` - Full transaction/block relay
- `BLOCK_RELAY_ONLY` - Headers-only, no tx relay
- `INBOUND` - Connections from others
- `MANUAL` - User-specified (addnode)
- `FEELER` - Short-lived test connections
- `ADDR_FETCH` - Just get addresses and disconnect

**We have:** Basic inbound/outbound distinction only

**Missing:**
- `ConnectionType` enum
- Block-relay-only connections for bandwidth efficiency
- FEELER connections to test addresses
- ADDR_FETCH for rapid address discovery
- Manual connection management (addnode/removenode)

### 2. **Bandwidth Management**
- `nMaxOutboundLimit` - Max outbound bandwidth per timeframe
- `OutboundTargetReached()` - Check if limit hit
- `GetOutboundTargetBytesLeft()` - Bytes remaining
- `GetMaxOutboundTimeLeftInCycle()` - Time until reset
- `GetTotalBytesRecv()` / `GetTotalBytesSent()` - Stats
- Per-connection send/receive buffer limits
- Historical block serving limits

**We have:** Nothing - unlimited bandwidth

**Impact:** Could be abused, waste bandwidth

### 3. **Thread Management**
Unicity has **6 dedicated threads:**
- `threadDNSAddressSeed` - DNS seed lookups
- `threadSocketHandler` - Socket I/O (select/poll)
- `threadOpenAddedConnections` - Manual connections
- `threadOpenConnections` - Automatic outbound
- `threadMessageHandler` - Message processing
- `threadI2PAcceptIncoming` - I2P listener

**We have:** Generic IO thread pool (4 threads)

**Missing:**
- Dedicated thread roles
- DNS seeding thread
- Separate socket handling thread
- Message processing thread separation

### 4. **Network Configuration**
- `SetNetworkActive()` / `GetNetworkActive()` - Enable/disable networking
- `SetTryNewOutboundPeer()` - Force extra connection attempts
- `StartExtraBlockRelayPeers()` - Add block-relay-only peers
- `GetExtraFullOutboundCount()` / `GetExtraBlockRelayCount()` - Connection counts
- Network type filtering (IPv4/IPv6/Tor/I2P)
- Per-network connection limits
- Whitelisted IP ranges with permissions
- Bind addresses configuration

**We have:** Basic config (port, thread count)

**Missing:**
- Network enable/disable
- Extra peer management
- Network type support (Tor, I2P)
- Whitelist/permissions system

### 5. **Node Management**
- `ForNode()` - Execute function on specific node
- `ForEachNode()` - Iterate all connected nodes
- `DisconnectNode()` - By string/subnet/address/ID
- `GetNodeStats()` - Detailed per-node statistics
- `ShouldRunInactivityChecks()` - Timeout logic
- `InactivityCheck()` - Disconnect inactive peers
- `MultipleManualOrFullOutboundConns()` - Check connection diversity
- Node permission flags

**We have:** Basic add/remove/get peers

**Missing:**
- Node iteration utilities
- Flexible disconnect options
- Detailed node statistics
- Inactivity checking
- Permission system

### 6. **Address Management Integration**
- `GetAddresses()` - With caching, network filtering, max percentage
- Cached address responses per peer (prevent topology leaks)
- `GetMappedAS()` - ASN mapping for diversity
- Anchor connections (peers from previous session)
- DNS seeding
- `AddAddrFetch()` / `ProcessAddrFetch()` - Address discovery

**We have:** Basic AddressManager integration

**Missing:**
- Address response caching
- Per-peer address tracking
- ASN-based selection
- Anchor peer persistence
- DNS seed support
- Dedicated address fetching

### 7. **Advanced Connection Features**
- `AddedNodesContain()` - Check if address is manual
- `GetAddedNodeInfo()` - Info about manual connections
- Semaphores for connection limiting
- Reconnection queue for failed connections
- Connection grants for resource management
- I2P session pooling
- V2 transport protocol support (BIP 324)

**We have:** Simple connection attempts

**Missing:**
- Manual node management
- Connection semaphores
- Reconnection logic
- I2P support
- Encrypted transport

### 8. **Message Handling**
- `PushMessage()` - Send with bandwidth accounting
- `WakeMessageHandler()` - Trigger message processing
- Message proc wake condition variable
- Interrupt message processing
- Per-peer message queuing with limits

**We have:** Basic message routing (ADDR/GETADDR only)

**Missing:**
- Bandwidth-aware sending
- Wake-up signaling
- Interruptible processing
- Message queue limits

### 9. **Socket Management**
- `SocketHandler()` - Select-based socket polling
- `GenerateWaitSockets()` - Build fd_set for select
- `ListenSocket` abstraction with permissions
- Per-socket permission flags
- Accept queue management
- Socket error handling

**We have:** Boost.Asio handles this

**Difference:** We use Boost.Asio's proactor pattern, Unicity uses select() reactor pattern

### 10. **Statistics & Monitoring**
- Per-network connection counts
- Total bytes sent/received tracking
- Outbound target monitoring
- Node count by direction
- Deterministic randomizer for testing
- Connection metrics

**We have:** Basic peer counts

**Missing:**
- Bandwidth statistics
- Per-network metrics
- Deterministic testing support

### 11. **Security Features**
- Nonce checking for self-connections
- Whitelist permissions
- Subnet banning integration
- Rate limiting
- Anti-fingerprinting (cached responses)
- Connection limits per network/subnet

**We have:**
- ✅ Connection limits
- ✅ Self-connection prevention (nLocalHostNonce - implemented 2025-10-13)

**Missing:**
- ❌ Permissions system
- ❌ Rate limiting
- ❌ Anti-fingerprinting
- ❌ Subnet-based limiting

### 12. **Lifecycle Management**
- `Start()` with CScheduler integration
- `Stop()` - Clean shutdown
- `StopThreads()` - Thread cleanup
- `StopNodes()` - Disconnect all
- `Interrupt()` - Graceful interruption
- Proper shutdown ordering

**We have:** Basic start/stop

**Missing:**
- Scheduler integration
- Graceful interruption
- Ordered shutdown

## Summary

**Core functionality we have:**
- ✅ Basic outbound connections
- ✅ Inbound connection acceptance
- ✅ Peer lifecycle management
- ✅ Periodic maintenance
- ✅ Address manager integration
- ✅ Basic message routing

**Critical missing for production:**
- ❌ Bandwidth limits (DoS risk - LOW priority for headers-only chain)
- ⚠️  Connection type diversity (NOT NEEDED - no transactions to relay)
- ✅ Self-connection prevention (IMPLEMENTED - 2025-10-13)
- ❌ Proper inactivity handling (HIGH priority)
- ❌ DNS seeding (MEDIUM priority)
- ❌ Statistics/monitoring (LOW priority - nice to have)

**Nice to have later:**
- ❌ Tor/I2P support
- ❌ V2 encrypted transport
- ❌ Advanced address caching
- ❌ Whitelisting/permissions
- ❌ Manual node management

**Architectural differences:**
- Unicity: 6 dedicated threads + select() pattern
- Us: Thread pool + Boost.Asio proactor pattern (actually cleaner!)

**Verdict:** Our NetworkManager has the essential features for a minimal blockchain.
The missing features are mostly for production hardening, advanced privacy, and enterprise use cases.

---

## Implementation Status

### ✅ Implemented Features (2025-10-13)

#### Self-Connection Prevention
**Status:** Complete
**Priority:** HIGH
**Implementation:**
- Added `local_nonce_` (64-bit random) to NetworkManager, generated on startup
- Each Peer uses the NetworkManager's `local_nonce_` in VERSION messages
- Inbound connections check if peer's nonce matches our local nonce
- Automatic disconnect on nonce match (self-connection detected)
- Prevents connection loops where node connects to itself

**Files Modified:**
- `include/network/network_manager.hpp` - Added local_nonce_, get_local_nonce(), check_incoming_nonce()
- `src/network/network_manager.cpp` - Generate nonce, pass to Peer creation
- `include/network/peer.hpp` - Added local_nonce_ and peer_nonce_ members
- `src/network/peer.cpp` - Use local_nonce_ in VERSION, check in handle_version()

**Code Location:**
- NetworkManager nonce generation: `src/network/network_manager.cpp:11-17`
- Peer nonce check: `src/network/peer.cpp:223-229`

**Testing:** Build successful, ready for integration testing

---

### 🚧 Next Priority Features

#### 1. Inactivity Timeout (HIGH Priority)
**Status:** Not Started
**Unicity Reference:** `TIMEOUT_INTERVAL = 20 minutes` (net.h:62)
**Why Critical:** Without this, zombie connections accumulate forever, wasting resources
**Implementation Plan:**
- Add 20-minute inactivity timer in Peer class (already has placeholder at protocol.hpp)
- Check time since last send/recv in periodic maintenance
- Disconnect peers that exceed timeout
- ~50 lines of code

#### 2. DNS Seeding (MEDIUM Priority)
**Status:** Not Started
**Unicity Reference:** `ThreadDNSAddressSeed()` (net.cpp:1115)
**Why Important:** Hard to discover initial peers without DNS seeds
**Implementation Plan:**
- Add DNS resolver using Boost.Asio
- Query seed hostnames (e.g., seed.coinbasechain.com)
- Parse A/AAAA records, add to AddressManager
- Run on startup if address manager is empty
- ~150 lines of code

#### 3. Bandwidth Tracking (LOW Priority - Headers-Only)
**Status:** Not Started
**Unicity Reference:** `GetTotalBytesRecv()`, `GetTotalBytesSent()` (net.h:1026-1027)
**Why Less Critical:** Headers are tiny (~100 bytes), not a DoS concern
**Implementation Plan:**
- Add atomic counters to NetworkManager
- Increment in Peer send/recv operations
- Add getter methods for stats
- Optional: Add per-peer tracking
- ~75 lines of code

---

### ⚠️ Features NOT Needed (Headers-Only Chain)

#### Connection Type Diversity
**Why Skip:** Bitcoin uses BLOCK_RELAY connections to avoid transaction relay overhead.
Since we have NO transactions, all connections are effectively "block-relay-only" already.
No bandwidth benefit from implementing multiple connection types.

---

**Last Updated:** 2025-10-13
**Next Review:** After implementing inactivity timeout
