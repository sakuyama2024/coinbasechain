# Peer Connection Deduplication Fix

## Issue
Network reports 10 peer connections instead of expected 5 in a 6-node mesh network. Each node has 2 connections to each peer (1 outbound + 1 inbound) instead of 1 connection per peer.

## Root Cause Analysis

### 1. Duplicate Detection Uses IP+Port Instead of IP-Only
**File**: `src/network/peer_manager.cpp:129`

Current implementation:
```cpp
if (peer->target_address() == address && peer->target_port() == port) {
```

Problem:
- Outbound connections store peer's listening port (e.g., 19590)
- Inbound connections store peer's ephemeral source port (e.g., 39546)
- Same peer has different ports, so duplicate check fails

Bitcoin Core comparison (`src/net.cpp:336`):
```cpp
if (static_cast<CNetAddr>(pnode->addr) == ip) {
```
- Bitcoin Core compares IP only, ignoring port
- Also checks IP:port combination with OR logic at `net.cpp:367`

### 2. Missing Bidirectional Connection Detection
**File**: `src/network/peer.cpp:352`

Current implementation:
```cpp
if (is_inbound_ && peer_nonce_ == local_nonce_) {
  // Only checks for self-connection
}
```

Missing:
- Check if incoming VERSION is from a peer we're already connecting to outbound
- Bitcoin Core has `CheckIncomingNonce()` at `net.cpp:370-378`
- Prevents simultaneous bidirectional connections in mesh networks

## Changes Required

### Change 1: Update find_peer_by_address to match IP-only
**File**: `src/network/peer_manager.cpp`
**Location**: Line 111-135

```cpp
int PeerManager::find_peer_by_address(const std::string &address,
                                      uint16_t port) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Bitcoin Core pattern: FindNode() searches by IP only
  // Inbound connections use ephemeral source ports, so we can't match by port
  for (const auto &[id, peer] : peers_) {
    if (!peer)
      continue;

    // Check address only (ignore port - Bitcoin Core pattern)
    if (peer->target_address() == address) {
      return id;
    }
  }

  return -1; // Not found
}
```

**Side effect**: `addnode remove IP:port` will now disconnect based on IP only. This matches Bitcoin Core behavior.

### Change 2: Add CheckIncomingNonce Equivalent
**File**: `src/network/peer.cpp`
**Location**: Line 350-358 (in handle_version_message)

Add before self-connection check:

```cpp
// SECURITY: Prevent duplicate bidirectional connections (Bitcoin Core pattern)
// If we're inbound and there's already an outbound connection to this peer
// that hasn't completed handshake, disconnect the inbound one
// This handles the race where both nodes try to connect simultaneously
if (is_inbound_ && message_handler_) {
  // Need to add PeerManager::CheckIncomingNonce() method
  // Check if any outbound peer (not successfully_connected) has this nonce
  if (!message_handler_->check_incoming_nonce(peer_nonce_)) {
    LOG_NET_WARN(
        "Duplicate bidirectional connection detected (outbound already exists), disconnecting inbound from {}",
        address());
    disconnect();
    return;
  }
}
```

**New method needed**: `PeerManager::CheckIncomingNonce(uint64_t nonce)`
```cpp
bool PeerManager::check_incoming_nonce(uint64_t nonce) const {
  std::lock_guard<std::mutex> lock(mutex_);

  // Check if we have an outbound connection (not yet fully connected) with this nonce
  for (const auto &[id, peer] : peers_) {
    if (!peer->is_inbound() &&
        !peer->is_successfully_connected() &&
        peer->peer_nonce() == nonce) {
      return false; // Duplicate found
    }
  }
  return true; // OK to proceed
}
```

## Impact Analysis

### Safe Changes
✅ Tests use unique IPs (127.0.0.1, 127.0.0.2, etc.) - won't break
✅ Production networks don't mix multiple networks on same IP
✅ Multi-network deployment (testnet/regtest/mainnet) uses same IPs but isolated networks

### Edge Case Broken
⚠️ Cannot run multiple nodes of SAME network on same IP with different ports
- This is purely a testing/development scenario
- Not a production use case
- Bitcoin Core has same limitation

### Evidence in Codebase
Test code already works around this issue (`test/network/simulated_node.cpp:169-172`):
```cpp
// We can't use find_peer_by_address() because:
// - For outbound peers: target_port = protocol::ports::REGTEST
// - For inbound peers: target_port = ephemeral source port (unknown)
// Since each node has a unique IP (127.0.0.X), search by address only
```

## Implementation Tasks

- [ ] 1. Add `PeerManager::check_incoming_nonce(uint64_t nonce)` method
  - File: `include/network/peer_manager.hpp`
  - File: `src/network/peer_manager.cpp`
  - Returns false if duplicate outbound connection exists

- [ ] 2. Add getter for peer_nonce in Peer class
  - File: `include/network/peer.hpp`
  - `uint64_t peer_nonce() const { return peer_nonce_; }`

- [ ] 3. Update `find_peer_by_address` to compare IP only
  - File: `src/network/peer_manager.cpp:129`
  - Remove port comparison

- [ ] 4. Add bidirectional connection check in VERSION handler
  - File: `src/network/peer.cpp:350`
  - Call check_incoming_nonce before self-connection check

- [ ] 5. Update comments explaining Bitcoin Core pattern
  - Files: peer_manager.cpp, peer.cpp
  - Reference Bitcoin Core net.cpp line numbers

- [ ] 6. Test on testnet deployment
  - Deploy changes to testnet
  - Verify connection count drops from 10 to 5
  - Verify network still functions correctly

- [ ] 7. Run full test suite
  - Ensure no regressions
  - Pay attention to network tests

## Expected Result
After fix, 6-node mesh network should show 5 connections per node instead of 10, matching Bitcoin Core behavior.

## References
- Bitcoin Core `FindNode()`: `src/net.cpp:332-363`
- Bitcoin Core `AlreadyConnectedToAddress()`: `src/net.cpp:365-368`
- Bitcoin Core `CheckIncomingNonce()`: `src/net.cpp:370-378`
- Bitcoin Core VERSION handling: `src/net_processing.cpp:3454-3459`
