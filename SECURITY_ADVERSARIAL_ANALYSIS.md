# Adversarial Security Analysis: How to Attack the Network Infrastructure

**Date:** 2025-10-19
**Threat Model:** Resourceful adversary with network access, multiple IPs, ability to run modified nodes
**Goal:** Identify exploitable vulnerabilities before attackers do

---

## Executive Summary

I've identified **12 critical attack vectors** ranging from resource exhaustion to consensus manipulation. While the infrastructure has solid DoS protection inspired by Bitcoin Core, there are **architectural weaknesses** and **logic gaps** that can be exploited.

**Risk Level: MEDIUM-HIGH** - Some attacks are immediately exploitable, others require coordination.

---

## Attack Vector 1: Inbound Connection Slot Exhaustion ⚠️ HIGH RISK

### The Vulnerability

**File:** `src/network/peer_manager.cpp:212-291` (evict_inbound_peer)

```cpp
// Protect recently connected peers (within 10 seconds)
auto connection_age = std::chrono::duration_cast<std::chrono::seconds>(
    now - peer->stats().connected_time);
if (connection_age.count() < 10) {
  continue;  // CANNOT be evicted
}
```

### The Attack

**Goal:** Deny legitimate peers from connecting by filling all 125 inbound slots

**Steps:**
1. Attacker controls 125+ IP addresses (cheap with cloud/VPN/Tor)
2. Connect from all 125 IPs simultaneously
3. Complete VERSION/VERACK handshake (required to count as "connected")
4. Every 9 seconds, **rotate one connection:**
   - Disconnect oldest connection
   - Immediately reconnect from same IP
   - New connection gets 10-second eviction protection
5. Repeat rotation to maintain perpetual protection

**Impact:**
- ✅ All 125 slots filled with attacker nodes
- ✅ Legitimate peers CANNOT connect (eviction fails)
- ✅ Victim node is isolated from honest network
- ✅ **Eclipse attack successful**

### Why It Works

```cpp
if (candidates.empty()) {
  return false;  // Can't evict - all peers protected
}
```

If all 125 peers have connected within last 10 seconds (via rotation), **NO candidates for eviction exist**.

### Exploitation Cost

- **125 IPs:** $5-20/month (cloud, VPN pools, or Tor)
- **Bandwidth:** Minimal (just handshakes + ping/pong)
- **Difficulty:** EASY

### Fix Required

```cpp
// Bitcoin Core approach: Protect SOME peers, not ALL
// Only protect the MOST RECENT 4-8 connections
sort(candidates.by_connection_time());
candidates = candidates.skip_newest(8);  // Protect newest 8 only

// Also add: Never protect ALL slots simultaneously
if (candidates.empty() && total_inbound > 100) {
  // Force evict oldest connection regardless
  return evict_oldest_unprotected();
}
```

---

## Attack Vector 2: Orphan Header Memory Exhaustion 🔴 CRITICAL

### The Vulnerability

**File:** `src/network/network_manager.cpp:889-906`

```cpp
if (reason == "orphaned") {
  LOG_INFO("Header from peer {} cached as orphan: {}",
           peer_id, header.GetHash().ToString().substr(0, 16));
  peer_manager_->IncrementUnconnectingHeaders(peer_id);
  continue;  // Process next header
}

if (reason == "orphan-limit") {
  LOG_WARN("Peer {} exceeded orphan limit", peer_id);
  peer_manager_->ReportTooManyOrphans(peer_id);  // Penalty only AFTER limit
  // ... disconnect peer
}
```

### The Attack

**Goal:** Consume all available RAM by forcing orphan header accumulation

**Steps:**
1. Attacker sends 2,000 **fabricated orphan headers** in a single HEADERS message
2. Each orphan header has **valid PoW** but unknown `hashPrevBlock`
3. Node caches ALL 2,000 orphans (limit not hit yet)
4. Repeat from 125 different inbound connections
5. **Total orphans:** 125 peers × 2,000 headers = **250,000 orphan headers**

**Memory Consumption:**
```
CBlockIndex size: ~200 bytes (estimated)
250,000 orphans × 200 bytes = 50 MB

With additional tracking:
- Orphan map entries
- Index pointers
- Hash maps
Total: ~100-200 MB per attack round
```

**Attack Amplification:**
- Disconnect and reconnect every 5 minutes
- Send another 2,000 orphans each time
- **Orphan count grows unbounded** (if cleanup is slow)

### Why It Works

1. **Orphan limit is PER-PEER**, not global
2. **Penalty applied AFTER limit** (attacker gets free 2,000 orphans first)
3. **No global memory bound** on total orphan count
4. **Cleanup might be slower than injection rate**

### Exploitation Difficulty

- **PoW Required:** YES (must mine valid PoW for 2,000 headers)
- **Cost:** Moderate (depends on difficulty)
- **Coordination:** 125 malicious peers

### Fix Required

```cpp
// Global orphan limit (Bitcoin Core approach)
static constexpr size_t MAX_ORPHAN_HEADERS_GLOBAL = 10000;

if (total_orphans_cached() >= MAX_ORPHAN_HEADERS_GLOBAL) {
  // Evict oldest orphans BEFORE accepting new ones
  evict_oldest_orphans(1000);
}

// Also: Penalize BEFORE caching
if (reason == "orphaned") {
  if (peer_orphan_count[peer_id]++ > 100) {  // Lower per-peer limit
    ReportTooManyOrphans(peer_id);
    return false;  // Reject message
  }
}
```

---

## Attack Vector 3: Unconnecting Headers Counter Reset Bypass 🔴 CRITICAL

### The Vulnerability

**File:** `src/network/network_manager.cpp:840-842`

```cpp
// Headers connect - reset unconnecting counter
peer_manager_->ResetUnconnectingHeaders(peer_id);
```

**File:** `src/network/peer_manager.cpp:459-486`

```cpp
static constexpr int MAX_UNCONNECTING_HEADERS = 10;

if (data.num_unconnecting_headers_msgs >= MAX_UNCONNECTING_HEADERS) {
  Misbehaving(peer_id, MisbehaviorPenalty::TOO_MANY_UNCONNECTING,
              "too many unconnecting headers");
}
```

### The Attack

**Goal:** Send unlimited orphan headers without getting banned

**Steps:**
1. Attacker sends **9 orphan headers** (just below threshold of 10)
2. Counter = 9 (close to limit)
3. Attacker sends **1 connecting header** (extends active chain)
4. **Counter RESET to 0** ✅
5. Repeat: 9 orphans → 1 connect → RESET
6. **Infinite orphan injection** without penalty

**Impact:**
- Attacker can send 9× more orphans than intended
- Combined with Attack Vector 2: **Massive memory exhaustion**
- Node never bans the peer

### Why It Works

The reset logic is too permissive:
```cpp
// ANY successful header resets the counter
// Even if peer sent 9 garbage orphans first
peer_manager_->ResetUnconnectingHeaders(peer_id);
```

### Exploitation Difficulty

- **Easy:** Just needs to mine 1 valid header per round
- **Cost:** Low (1 header every 10 orphans)

### Fix Required

```cpp
// Bitcoin Core approach: Don't reset counter on EVERY connect
// Only reset if peer has sent ONLY connecting headers recently

// Option 1: Decay instead of reset
data.num_unconnecting_headers_msgs = std::max(0, data.num_unconnecting_headers_msgs - 2);

// Option 2: Only reset if counter is low
if (data.num_unconnecting_headers_msgs < 5) {
  data.num_unconnecting_headers_msgs = 0;
}

// Option 3: Track ratio of good:bad headers
data.good_headers++;
if (data.good_headers / (data.bad_headers + 1) < 0.1) {  // Less than 10% good
  Misbehaving(peer_id, penalty, "poor header quality ratio");
}
```

---

## Attack Vector 4: Ban Evasion via IP Rotation ⚠️ MEDIUM RISK

### The Vulnerability

**File:** `src/network/banman.cpp:183-192`

```cpp
void BanMan::Discourage(const std::string &address) {
  std::lock_guard<std::mutex> lock(m_discouraged_mutex);
  int64_t now = GetTime();
  int64_t expiry = now + DISCOURAGEMENT_DURATION;  // 24 hours
  m_discouraged[address] = expiry;
  LOG_INFO("BanMan: Discouraged {} until {} (~24h)", address, expiry);
}
```

Discouragement is **IP-based** and temporary (24 hours).

### The Attack

**Goal:** Evade bans by rotating IPs

**Steps:**
1. Attacker uses IP pool (cloud, VPN, Tor exit nodes)
2. Attack from IP #1 until discouraged/banned
3. Immediately switch to IP #2, IP #3, etc.
4. **With 100 IPs:** 100 × 24 hours = **2,400 hours of attack time**
5. Or cycle through IPs faster than ban duration

**Impact:**
- Bans are ineffective against resourced attackers
- Attacker can maintain persistent DoS
- No defense against distributed attacks

### Why It Works

- **No account-based identity** (Bitcoin is also vulnerable)
- **IP rotation is cheap** (cloud providers, Tor, VPNs)
- **Temporary bans** (24h is not enough for persistent threats)

### Exploitation Difficulty

- **Easy:** Just needs IP pool
- **Cost:** $20-50/month for 100+ IPs

### Mitigations (Partial)

```cpp
// 1. Ban entire subnet ranges for repeat offenders
if (repeat_offender_from_subnet(subnet)) {
  ban_subnet(subnet, 7 * 24 * 3600);  // 7 days
}

// 2. Increase ban duration for patterns
if (ban_count_last_24h > 10) {
  ban_duration = 7 * 24 * 3600;  // Escalate to 7 days
}

// 3. Proof-of-Work connection challenge (drastic)
if (under_attack()) {
  require_pow_for_new_connections();
}
```

**Note:** This is hard to fully solve without centralized identity (which Bitcoin doesn't have).

---

## Attack Vector 5: GETHEADERS Amplification Attack ⚠️ MEDIUM RISK

### The Vulnerability

**File:** `src/network/network_manager.cpp:1010-1084`

```cpp
bool NetworkManager::handle_getheaders_message(
    PeerPtr peer, message::GetHeadersMessage *msg) {

  // No rate limiting on GETHEADERS requests
  // No check if peer is spamming requests

  auto response = std::make_unique<message::HeadersMessage>();

  while (pindex && response->headers.size() < protocol::MAX_HEADERS_SIZE) {
    CBlockHeader hdr = pindex->GetBlockHeader();
    response->headers.push_back(hdr);  // Up to 2,000 headers
  }

  peer->send_message(std::move(response));  // Send immediately
  return true;
}
```

### The Attack

**Goal:** Amplify bandwidth consumption and CPU usage

**Steps:**
1. Attacker sends GETHEADERS with locator pointing to genesis
2. Node responds with 2,000 headers (first batch)
3. Attacker sends GETHEADERS again (different locator)
4. Node responds with another 2,000 headers
5. Repeat **continuously** from 125 inbound peers

**Amplification:**
```
GETHEADERS request: ~500 bytes (small)
HEADERS response: 2,000 headers × 80 bytes = 160,000 bytes (large)

Amplification factor: 160,000 / 500 = 320×
```

**Attack Rate:**
```
125 peers × 10 requests/sec × 160 KB = 200 MB/sec outbound
```

**Impact:**
- ✅ Saturate victim's **upload bandwidth**
- ✅ **CPU exhaustion** (serializing 2,000 headers repeatedly)
- ✅ **Legitimate peers starved** of bandwidth
- ✅ Node becomes unusable

### Why It Works

1. **No rate limiting** on GETHEADERS
2. **No request deduplication** (same request answered multiple times)
3. **Large response size** (2,000 headers)
4. **No cost to attacker** (tiny request, huge response)

### Exploitation Difficulty

- **Very Easy:** Just spam GETHEADERS
- **Cost:** Minimal (125 connections)

### Fix Required

```cpp
// Rate limit GETHEADERS per peer
struct PeerRequestTracking {
  int64_t last_getheaders_time;
  int getheaders_count_per_minute;
};

if (peer_tracking[peer_id].getheaders_count_per_minute > 10) {
  LOG_WARN("Peer {} is spamming GETHEADERS, ignoring", peer_id);
  Misbehaving(peer_id, 10, "GETHEADERS spam");
  return false;
}

// Deduplicate identical requests
if (is_duplicate_request(msg, peer_id)) {
  LOG_DEBUG("Ignoring duplicate GETHEADERS from peer {}", peer_id);
  return true;  // Ignore silently
}
```

---

## Attack Vector 6: Sync Stalling Attack 🔴 CRITICAL

### The Vulnerability

**File:** `src/network/network_manager.cpp:556-610`

```cpp
// Only sync from ONE peer at a time
uint64_t current_sync_peer = sync_peer_id_.load(std::memory_order_acquire);
if (current_sync_peer == 0) {
  // No active sync, start with this peer
  sync_peer_id_.store(peer->id(), std::memory_order_release);
  request_headers_from_peer(peer);
  return;  // Syncing from this peer now
}
```

**No timeout mechanism** - if sync peer is slow, node waits indefinitely.

### The Attack

**Goal:** Prevent victim node from syncing by becoming the sync peer and stalling

**Steps:**
1. Attacker connects first (before honest peers)
2. Sends VERSION with high `start_height` (claims to be at tip)
3. Node selects attacker as **sync peer**
4. Attacker responds to GETHEADERS with:
   - **Option A:** Very slow responses (1 header per minute)
   - **Option B:** Valid but useless headers (side chain)
   - **Option C:** No response at all (timeout is very long)
5. Victim **waits indefinitely** for this peer
6. Other honest peers are **ignored** (only sync from one peer)

**Impact:**
- ✅ Victim node **never syncs**
- ✅ Remains on old chain state
- ✅ Vulnerable to double-spend if using outdated state

### Why It Works

```cpp
// Only sync from ONE peer
if (current_sync_peer != 0 && current_sync_peer != peer->id()) {
  return;  // Another peer is syncing, ignore this peer
}
```

No parallel sync, no failover, no timeout detection.

### Exploitation Difficulty

- **Easy:** Just respond slowly
- **Cost:** 1 connection

### Fix Required (CRITICAL)

```cpp
// Bitcoin Core approach: Parallel sync + timeout
static constexpr int64_t SYNC_TIMEOUT_SECONDS = 60;

// Check if current sync peer is stalled
if (current_sync_peer != 0) {
  int64_t stall_time = now - last_headers_received_;
  if (stall_time > SYNC_TIMEOUT_SECONDS) {
    LOG_WARN("Sync peer {} stalled for {}s, switching to {}",
             current_sync_peer, stall_time, peer->id());

    // Disconnect stalled peer
    disconnect_from(current_sync_peer);

    // Switch to new peer
    sync_peer_id_.store(peer->id());
  }
}

// Or better: Sync from MULTIPLE peers simultaneously
// Request different ranges from different peers
request_headers_range(peer1, 0, 1000);
request_headers_range(peer2, 1000, 2000);
```

---

## Attack Vector 7: Self-Connection Race Condition ⚠️ LOW RISK

### The Vulnerability

**File:** `src/network/peer.cpp:269-277`

```cpp
// Check for self-connection (nonce match)
if (msg.nonce == local_nonce_) {
  LOG_NET_WARN(
      "Self-connection detected (nonce match), disconnecting from {}",
      address());
  disconnect();
  return;
}
```

### The Attack

**Goal:** Cause temporary connection churn

**Steps:**
1. Attacker learns victim's nonce (from observing VERSION messages)
2. Attacker connects and sends VERSION with **victim's nonce**
3. Victim detects "self-connection" and disconnects
4. Attacker repeats from many IPs
5. Victim wastes connection slots on false self-detections

**Impact:**
- ⚠️ Minor annoyance
- ⚠️ Connection slot churn
- ⚠️ Potential for confusion in logs

### Why It Works

Nonce is not secret - transmitted in plain text over network.

### Mitigation

```cpp
// Add additional checks beyond nonce
if (msg.nonce == local_nonce_) {
  // Also check: Are we actually trying to connect to ourselves?
  if (is_loopback_address(address()) || is_local_lan_address(address())) {
    disconnect();  // Probably real self-connection
  } else {
    // Suspicious: nonce match but not our address
    LOG_WARN("Possible nonce spoofing from {}", address());
    Misbehaving(peer_id, 20, "nonce spoofing");
  }
}
```

---

## Attack Vector 8: Message Deserialization CPU Exhaustion ⚠️ MEDIUM RISK

### The Vulnerability

**File:** `src/network/peer.cpp:194-218` (on_transport_receive)

```cpp
// Check receive buffer flood protection
if (recv_buffer_.size() > protocol::DEFAULT_RECV_FLOOD_SIZE) {
  LOG_NET_ERROR("Receive buffer overflow ({} bytes, limit: {} bytes), disconnecting from {}",
                recv_buffer_.size(), protocol::DEFAULT_RECV_FLOOD_SIZE, address());
  disconnect();
  return;
}

// Process all complete messages in buffer
process_received_data(recv_buffer_);
```

**File:** `src/network/message.cpp` (deserialization)

```cpp
bool HeadersMessage::deserialize(const uint8_t *data, size_t size) {
  MessageDeserializer d(data, size);
  uint64_t count = d.read_varint();  // User-controlled

  headers.resize(count);  // ALLOCATE based on attacker's count

  for (uint64_t i = 0; i < count; ++i) {
    // Deserialize each header (CPU intensive)
    CBlockHeader header;
    // ...
  }
}
```

### The Attack

**Goal:** Cause CPU exhaustion via malformed messages

**Steps:**
1. Attacker sends message with header: `varint count = 0xFFFFFFFFFFFFFFFF` (2^64-1)
2. Node attempts to `resize(0xFFFFFFFFFFFFFFFF)` → **allocation failure or OOM**
3. Or: Attacker sends valid count but malformed data
4. Node spends CPU deserializing garbage
5. Repeat from 125 peers

**Impact:**
- ✅ CPU saturation (100% usage deserializing)
- ✅ Memory exhaustion (allocation failures)
- ✅ Node becomes unresponsive

### Why It Works

**No validation before allocation:**
```cpp
uint64_t count = d.read_varint();  // Attacker controls this
headers.resize(count);  // Allocation BEFORE validation
```

### Fix Required

```cpp
bool HeadersMessage::deserialize(const uint8_t *data, size_t size) {
  MessageDeserializer d(data, size);
  uint64_t count = d.read_varint();

  // VALIDATE before allocation
  if (count > protocol::MAX_HEADERS_SIZE) {
    LOG_ERROR("Invalid HEADERS count: {} (max={})", count, protocol::MAX_HEADERS_SIZE);
    return false;  // Reject message
  }

  // Also: Check that buffer size makes sense
  size_t expected_size = count * 80;  // 80 bytes per header
  if (size < expected_size) {
    LOG_ERROR("HEADERS message too small for claimed count");
    return false;
  }

  headers.reserve(count);  // Reserve, not resize
  // Then deserialize one at a time with validation
}
```

---

## Attack Vector 9: Lock Contention DoS ⚠️ LOW-MEDIUM RISK

### The Vulnerability

**File:** `src/network/peer_manager.cpp`

```cpp
// EVERY peer operation takes global mutex
std::lock_guard<std::mutex> lock(mutex_);

size_t PeerManager::peer_count() const {
  std::lock_guard<std::mutex> lock(mutex_);  // Lock for simple read
  return peers_.size();
}

std::vector<PeerPtr> PeerManager::get_all_peers() {
  std::lock_guard<std::mutex> lock(mutex_);  // Lock + copy all peers
  std::vector<PeerPtr> result;
  result.reserve(peers_.size());
  for (const auto &[id, peer] : peers_) {
    result.push_back(peer);
  }
  return result;
}
```

### The Attack

**Goal:** Cause lock contention to slow down all network operations

**Steps:**
1. Attacker connects 125 peers
2. All peers send messages simultaneously
3. Each message triggers:
   - `get_peer()` → Lock
   - `remove_peer()` → Lock
   - `add_peer()` → Lock
   - `Misbehaving()` → Lock
4. **Threads block waiting for mutex**
5. Network operations slow to a crawl

**Impact:**
- ⚠️ Increased latency for all network operations
- ⚠️ Poor scalability under load
- ⚠️ CPU cycles wasted spinning on locks

### Why It Works

**Single global mutex** protects entire peer map:
```cpp
std::mutex mutex_;
std::map<int, PeerPtr> peers_;  // All access requires mutex_
```

### Mitigation

```cpp
// Option 1: Fine-grained locking (per-peer locks)
std::map<int, std::pair<PeerPtr, std::mutex>> peers_;

// Option 2: Read-write lock (multiple readers, single writer)
std::shared_mutex mutex_;  // C++17

size_t peer_count() const {
  std::shared_lock lock(mutex_);  // Read lock (allows concurrent reads)
  return peers_.size();
}

// Option 3: Lock-free data structures
std::atomic<size_t> peer_count_;
```

---

## Attack Vector 10: Eclipse Attack via Anchor Manipulation 🔴 HIGH RISK

### The Vulnerability

**File:** `src/network/network_manager.cpp:1087-1138` (GetAnchors)

```cpp
std::vector<protocol::NetworkAddress> NetworkManager::GetAnchors() const {
  auto outbound_peers = peer_manager_->get_outbound_peers();

  // Take up to 3 outbound peers
  size_t anchor_count = std::min(outbound_peers.size(), size_t(3));
  for (size_t i = 0; i < anchor_count; ++i) {
    // ...
  }
}
```

**File:** `src/network/network_manager.cpp:102-110` (LoadAnchors)

```cpp
if (LoadAnchors(anchors_path)) {
  LOG_NET_INFO("Loaded anchors, will connect to them first");
}
```

### The Attack

**Goal:** Poison anchor peers to enable persistent eclipse attack

**Steps:**
1. **Phase 1: Initial Poisoning**
   - Attacker isolates victim node (Attack Vector 1)
   - Victim only connects to attacker's 8 outbound peers
   - Victim saves attacker peers as "anchors"
   - Victim shuts down

2. **Phase 2: Persistent Eclipse**
   - Victim restarts
   - Loads anchors → **all attacker addresses**
   - Connects to anchors **FIRST** (priority)
   - Attacker has 3/8 or 8/8 outbound slots
   - **Eclipse persists across restarts**

**Impact:**
- 🔴 **Permanent eclipse attack**
- 🔴 Victim sees attacker's fake chain
- 🔴 Double-spend vulnerability
- 🔴 Censorship of transactions/blocks

### Why It Works

1. **Anchors are trusted without validation**
2. **No diversity requirement** (all anchors could be same /16 subnet)
3. **No rotation** (same anchors used forever)
4. **Priority given to anchors** (fills slots before honest peers)

### Exploitation Difficulty

- **Moderate:** Requires multi-phase attack
- **Persistent:** Once poisoned, eclipse persists

### Fix Required (CRITICAL)

```cpp
// 1. Validate anchor diversity
bool SaveAnchors(const std::vector<NetworkAddress>& anchors) {
  // Require anchors from different /16 subnets
  std::set<uint32_t> subnets;
  for (const auto& addr : anchors) {
    uint32_t subnet = get_subnet_prefix(addr, 16);
    if (subnets.count(subnet)) {
      LOG_WARN("Anchors from same subnet, rejecting");
      return false;  // Don't save
    }
    subnets.insert(subnet);
  }
}

// 2. Rotate anchors periodically
if (time_since_anchor_save() > 24 * 3600) {
  RefreshAnchors();  // Get new anchor set
}

// 3. Don't prioritize anchors exclusively
// Connect to anchors + random outbound simultaneously
connect_to_anchors(2);  // Only 2/8 slots for anchors
connect_to_random_outbound(6);  // 6/8 slots for diverse peers
```

---

## Attack Vector 11: Timestamp Manipulation ⚠️ MEDIUM RISK

### The Vulnerability

**File:** `include/network/protocol.hpp:126-131`

```cpp
constexpr int64_t TIMESTAMP_ALLOWANCE_SEC = 2 * 60 * 60; // 2 hours
constexpr int64_t MAX_FUTURE_BLOCK_TIME = 2 * 60 * 60;   // 2 hours
```

### The Attack

**Goal:** Manipulate network time or block acceptance

**Steps:**
1. Attacker sends block headers with timestamps **+2 hours in future**
2. Node accepts them (within MAX_FUTURE_BLOCK_TIME)
3. If enough peers send future timestamps, could skew time consensus
4. Or: Attacker sends headers with **very old timestamps**
5. Could cause issues with time-based logic

**Impact:**
- ⚠️ Potential time skew
- ⚠️ Acceptance of "future" blocks
- ⚠️ Confusion in time-based security checks

### Mitigation

```cpp
// Bitcoin Core median time approach
int64_t GetMedianTimePast(const CBlockIndex* pindex) {
  // Use median of last 11 blocks
  // More robust against timestamp manipulation
}

// Also: Reject far-future timestamps more aggressively
if (header.nTime > GetAdjustedTime() + 2 * 3600) {
  if (header.nTime > GetAdjustedTime() + 10 * 60) {  // 10 minutes
    return "time-too-new";
  }
}
```

---

## Attack Vector 12: Race Condition in Peer Removal 🔴 MEDIUM RISK

### The Vulnerability

**File:** `src/network/peer_manager.cpp:44-54` (add_peer)

```cpp
if (is_inbound && current_inbound >= config_.max_inbound_peers) {
  // Release lock temporarily to call evict_inbound_peer
  mutex_.unlock();  // UNLOCK HERE
  bool evicted = evict_inbound_peer();
  mutex_.lock();    // RE-LOCK HERE

  if (!evicted) {
    return -1;
  }
}
```

### The Attack

**Goal:** Exploit race condition to exceed connection limits

**Steps:**
1. Fill all 125 inbound slots
2. Two threads simultaneously try to add new inbound peers (Thread A, Thread B)
3. Both threads see `current_inbound == 125`
4. **Thread A unlocks mutex**, calls `evict_inbound_peer()`
5. **Thread B unlocks mutex**, calls `evict_inbound_peer()`
6. Thread A evicts peer → 124 slots
7. Thread B evicts peer → 123 slots
8. **Thread A adds peer** → 124 slots
9. **Thread B adds peer** → 125 slots
10. Both succeed, **BUT** if more threads do this simultaneously, could exceed limit

**Impact:**
- ⚠️ Connection limit bypass (minor)
- ⚠️ Could allow 130+ connections instead of 125
- ⚠️ Resource consumption slightly higher

### Why It Works

**Time-of-check to time-of-use (TOCTOU) race:**
```cpp
// Check
if (current_inbound >= max) {
  unlock();  // RACE WINDOW HERE
  evict();
  lock();
  add();  // Might exceed limit if multiple threads
}
```

### Fix Required

```cpp
// Don't unlock mutex during eviction
if (is_inbound && current_inbound >= config_.max_inbound_peers) {
  // Option 1: Evict while holding lock (change evict_inbound_peer signature)
  bool evicted = evict_inbound_peer_unsafe();  // Assumes lock held

  if (!evicted) {
    return -1;
  }
}

// Or Option 2: Re-check condition after lock
mutex_.unlock();
bool evicted = evict_inbound_peer();
mutex_.lock();

// RE-CHECK: Did another thread add a peer while we were unlocked?
size_t new_count = count_inbound_peers();
if (!evicted || new_count >= max_inbound_peers) {
  return -1;
}
```

---

## Summary of Attack Vectors

| # | Attack | Risk | Difficulty | Impact |
|---|--------|------|------------|--------|
| 1 | Inbound Slot Exhaustion | 🔴 HIGH | Easy | Eclipse attack |
| 2 | Orphan Memory Exhaustion | 🔴 CRITICAL | Moderate | OOM crash |
| 3 | Unconnecting Counter Bypass | 🔴 CRITICAL | Easy | Infinite orphans |
| 4 | Ban Evasion (IP Rotation) | ⚠️ MEDIUM | Easy | Persistent DoS |
| 5 | GETHEADERS Amplification | ⚠️ MEDIUM | Very Easy | Bandwidth exhaustion |
| 6 | Sync Stalling | 🔴 CRITICAL | Easy | Prevent sync |
| 7 | Self-Connection Spoofing | ⚠️ LOW | Easy | Connection churn |
| 8 | Message Deserialization CPU | ⚠️ MEDIUM | Easy | CPU/memory exhaustion |
| 9 | Lock Contention DoS | ⚠️ LOW-MED | Moderate | Performance degradation |
| 10 | Anchor Poisoning Eclipse | 🔴 HIGH | Moderate | Persistent eclipse |
| 11 | Timestamp Manipulation | ⚠️ MEDIUM | Easy | Time skew |
| 12 | Peer Removal Race | ⚠️ MEDIUM | Hard | Limit bypass |

---

## Recommended Immediate Fixes (Priority Order)

### P0 - CRITICAL (Fix Immediately)

1. **Sync Stalling (Vector 6)**
   - Add timeout for sync peer (60 seconds)
   - Switch to new peer if stalled
   - **Impact:** Prevents node from never syncing

2. **Orphan Memory Exhaustion (Vector 2)**
   - Add global orphan limit (10,000 headers)
   - Evict oldest orphans when limit hit
   - **Impact:** Prevents OOM crash

3. **Unconnecting Counter Bypass (Vector 3)**
   - Use decay instead of reset
   - Track good:bad header ratio
   - **Impact:** Prevents infinite orphan injection

### P1 - HIGH (Fix Soon)

4. **Inbound Slot Exhaustion (Vector 1)**
   - Limit eviction protection to newest 8 peers
   - Force eviction if all slots protected
   - **Impact:** Prevents eclipse attack

5. **Anchor Poisoning (Vector 10)**
   - Validate anchor diversity (different subnets)
   - Rotate anchors periodically
   - Don't prioritize anchors exclusively
   - **Impact:** Prevents persistent eclipse

6. **GETHEADERS Amplification (Vector 5)**
   - Rate limit GETHEADERS (10/minute per peer)
   - Deduplicate identical requests
   - **Impact:** Prevents bandwidth DoS

### P2 - MEDIUM (Fix When Possible)

7. **Message Deserialization (Vector 8)**
   - Validate counts before allocation
   - Check buffer size matches claimed count

8. **Ban Evasion (Vector 4)**
   - Add subnet banning for repeat offenders
   - Escalate ban duration

9. **Lock Contention (Vector 9)**
   - Use read-write locks for peer map
   - Consider lock-free data structures

10. **Race Condition (Vector 12)**
    - Re-check condition after re-acquiring lock
    - Or hold lock during eviction

---

## Testing Recommendations

### Adversarial Test Suite

```cpp
// Test Vector 1: Inbound slot exhaustion
TEST_CASE("Cannot eclipse via connection rotation") {
  // Connect 125 peers, rotate every 9 seconds
  // Verify that eviction still works
}

// Test Vector 2: Orphan memory bounds
TEST_CASE("Orphan headers have global memory limit") {
  // Send orphans from 125 peers
  // Verify total orphans < global limit
}

// Test Vector 3: Unconnecting counter
TEST_CASE("Cannot bypass unconnecting limit via interleaving") {
  // Send 9 orphans, 1 good, repeat
  // Verify peer gets banned eventually
}

// Test Vector 6: Sync stalling
TEST_CASE("Sync switches peer if stalled") {
  // Make sync peer stop responding
  // Verify node switches to new peer after timeout
}
```

---

## Final Verdict

**The network infrastructure has good foundations but CRITICAL vulnerabilities:**

1. ✅ **Strong:** DoS protection framework, misbehavior tracking, flood limits
2. 🔴 **Weak:** Eviction logic, orphan limits, sync resilience, anchor validation
3. ⚠️ **Missing:** Request rate limiting, global resource bounds, sync timeout

**Most Dangerous Attacks:**
- Sync Stalling (Vector 6) → Node never syncs
- Orphan Memory (Vector 2) + Counter Bypass (Vector 3) → OOM crash
- Anchor Poisoning (Vector 10) → Persistent eclipse

**Estimated Time to Fix Critical Issues:** 1-2 weeks for experienced developer

**Risk if Deployed Now:** HIGH - Attackers could DOS or eclipse nodes with moderate resources (~$100/month)

---

## Response to "How Would You Attack This?"

If I were an attacker with $1,000 budget:

1. **Buy 125 cloud IPs** (~$50/month)
2. **Launch Inbound Slot Exhaustion** (Vector 1) to eclipse target
3. **Simultaneously:** Orphan Memory Attack (Vector 2+3) to crash node
4. **If node recovers:** Sync Stalling (Vector 6) to prevent sync
5. **Poison anchors** (Vector 10) for persistent eclipse across restarts

**Total cost:** ~$100/month to completely DOS a single node.

With $10,000: Could attack dozens of nodes simultaneously.

---

**Recommendation:** Implement P0 and P1 fixes before mainnet deployment.
