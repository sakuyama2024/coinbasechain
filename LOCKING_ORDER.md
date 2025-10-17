# Thread Safety and Locking Order

## Overview

This document defines the locking order for all mutexes in the codebase to **prevent deadlocks**. When acquiring multiple locks, **ALWAYS acquire them in the order listed below**.

**Deadlock Rule**: If you need locks A and B:
- Thread 1 acquires A then B → OK
- Thread 2 acquires A then B → OK
- Thread 1 acquires A then B, Thread 2 acquires B then A → **DEADLOCK!**

---

## Global Locking Hierarchy

Acquire locks in this order (from outer to inner):

```
1. ChainstateManager::validation_mutex_
   ↓
2. HeaderSync::mutex_
   ↓
3. PeerManager::mutex_
   ↓
4. Peer::send_queue_mutex_ (per-peer)
   ↓
5. AddressManager::mutex_
   ↓
6. RandomX::g_randomx_mutex (global)
   ↓
7. RandomX::VMRef::hashing_mutex (per-VM)
```

**Example (correct order)**:
```cpp
std::lock_guard<std::mutex> lock_chainstate(chainstate_manager_.validation_mutex_);
std::lock_guard<std::mutex> lock_peers(peer_manager_.mutex_);
// Safe: validation_mutex_ acquired before peer mutex
```

**Example (WRONG - will deadlock)**:
```cpp
std::lock_guard<std::mutex> lock_peers(peer_manager_.mutex_);           // ❌ Acquired first
std::lock_guard<std::mutex> lock_chainstate(chainstate_manager_.validation_mutex_); // ❌ Acquired second
// DEADLOCK RISK: Another thread might acquire in opposite order!
```

---

## Detailed Lock Documentation

### 1. ChainstateManager::validation_mutex_

**Location**: `include/validation/chainstate_manager.hpp:168` (mutable member)

**Protects**:
- BlockManager access (`block_manager_` calls)
- Blockchain state reads/writes
- Fork candidates (`setBlockIndexCandidates`)
- Chain tip updates
- Block validation

**Held during**:
- `AcceptBlockHeader()` - validates and adds block to index
- `ActivateBestChain()` - switches active chain
- `GetTip()`, `LookupBlockIndex()`, `IsOnActiveChain()` - state queries
- `Save()` / `Load()` - persistence operations

**Acquire BEFORE**:
- Any other mutex (this is the outermost lock)

**Thread safety notes**:
- Multiple threads can call `ChainstateManager` methods (from RPC, network handlers, mining)
- BlockManager has NO mutex - relies on ChainstateManager's lock
- This matches Bitcoins's `cs_main` pattern
- **Important**: BlockManager is a PRIVATE member and should ONLY be accessed through ChainstateManager methods
- All BlockManager access is protected by `validation_mutex_` through this encapsulation

**Example**:
```cpp
// validation/chainstate_manager.cpp:51
chain::CBlockIndex* ChainstateManager::AcceptBlockHeader(const CBlockHeader& header,
                                                         ValidationState& state,
                                                         bool min_pow_checked)
{
    std::lock_guard<std::mutex> lock(validation_mutex_);  // Held for entire validation

    // Safe: All BlockManager calls protected by this lock
    chain::CBlockIndex* pindex = block_manager_.LookupBlockIndex(hash);
    // ...
}
```

---

### 2. HeaderSync::mutex_

**Location**: `include/sync/header_sync.hpp:163` (mutable member)

**Protects**:
- Sync state (`state_` - IDLE/SYNCING/SYNCED)
- Last batch size (`last_batch_size_`)
- Sync state callback (`sync_state_callback_`)

**Held during**:
- `ProcessHeaders()` - updates batch size and state (briefly)
- `UpdateState()` - updates sync state and calls callback
- `GetState()` - reads state
- `SetSyncStateCallback()` - sets callback
- `ShouldRequestMore()` - reads batch size and checks sync status

**Acquire AFTER**:
- `ChainstateManager::validation_mutex_` (chainstate operations happen first)

**Acquire BEFORE**:
- `PeerManager::mutex_` (if both needed)

**Thread safety notes**:
- Multiple io_context threads call HeaderSync methods concurrently
- Mutex held only for brief state reads/writes, NOT during validation
- ChainstateManager::validation_mutex_ is acquired FIRST during ProcessHeaders()
- HeaderSync::mutex_ acquired AFTER for state updates
- Critical sections are very small (just state variable access)

**Example**:
```cpp
// sync/header_sync.cpp:41-50
bool HeaderSync::ProcessHeaders(const std::vector<CBlockHeader>& headers, int peer_id)
{
    // ... validation code calls chainstate_manager methods (acquires validation_mutex) ...

    // THEN update our state under our mutex
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_batch_size_ = headers.size();
    }

    UpdateState();  // Also holds mutex_ briefly
    return true;
}
```

---

### 3. PeerManager::mutex_

**Location**: `include/network/peer_manager.hpp:141` (mutable member)

**Protects**:
- Peer list (`peers_` map)
- Peer addition/removal
- Peer count queries

**Held during**:
- `add_peer()` - adds peer to map
- `remove_peer()` - removes peer from map
- `get_peer()`, `get_all_peers()`, `get_outbound_peers()` - queries

**Acquire AFTER**:
- `ChainstateManager::validation_mutex_` (if both needed)
- `HeaderSync::mutex_` (if both needed)

**Acquire BEFORE**:
- `Peer::send_queue_mutex_` (peer-specific locks)

**Thread safety notes**:
- Multiple io_context threads call PeerManager methods concurrently
- Peer objects are owned by shared_ptr, so they remain valid after removal
- Lock is coarse-grained (entire map) for simplicity

**Example**:
```cpp
// network/peer_manager.cpp:XX
bool PeerManager::add_peer(PeerPtr peer) {
    std::lock_guard<std::mutex> lock(mutex_);  // Protects peers_ map
    peers_[peer->id()] = peer;
    return true;
}
```

---

### 4. Peer::send_queue_mutex_ (Per-Peer)

**Location**: `include/network/peer.hpp:197` (member)

**Protects**:
- `send_queue_` - outgoing messages
- `writing_` flag - write operation in progress

**Held during**:
- `send_message()` - adds message to queue
- `do_write()` - sends next message
- `schedule_next_write()` - prepares next send

**Acquire AFTER**:
- `PeerManager::mutex_` (if iterating peers and sending messages)

**Acquire BEFORE**:
- Network I/O operations (but released before async_write!)

**Thread safety notes**:
- Multiple io_context threads can call `send_message()` on the same peer
- Mutex protects queue, but NOT the socket (Boost.Asio handles that)
- **Critical**: Mutex is released BEFORE calling `boost::asio::async_write()` to avoid deadlock

**Example**:
```cpp
// network/peer.cpp:125-140
void Peer::send_message(std::unique_ptr<message::Message> msg) {
    boost::asio::post(io_context_, [self = shared_from_this(), data = std::move(full_message)]() mutable {
        bool should_write = false;
        {
            std::lock_guard<std::mutex> lock(self->send_queue_mutex_);  // Lock scope
            self->send_queue_.push(std::move(data));
            if (!self->writing_) {
                self->writing_ = true;
                should_write = true;
            }
        }  // Mutex released here

        // Call start_write OUTSIDE the mutex to avoid deadlock
        if (should_write) {
            self->do_write();
        }
    });
}
```

---

### 5. AddressManager::mutex_

**Location**: `include/network/addr_manager.hpp:XX` (member)

**Protects**:
- Address tables (tried, new buckets)
- Address selection and marking

**Held during**:
- `add()`, `add_multiple()` - adds addresses
- `select()` - picks address for connection
- `attempt()`, `good()`, `connected()` - marks address state

**Acquire AFTER**:
- `PeerManager::mutex_` (if adding addresses from peer list)

**Thread safety notes**:
- Multiple threads access AddressManager (network threads, RPC)
- Lock is coarse-grained for simplicity
- No nested locking with other components currently

---

### 6. RandomX::g_randomx_mutex (Global)

**Location**: `src/crypto/randomx_pow.cpp:45`

**Protects**:
- `g_randomx_initialized` - initialization flag
- `g_randomx_dataset` - global dataset
- VM cache access (until per-VM locks)

**Held during**:
- `InitializeRandomX()` - one-time setup
- `CheckProofOfWorkRandomX()` - gets VM from pool

**Acquire AFTER**:
- `ChainstateManager::validation_mutex_` (during block validation)

**Acquire BEFORE**:
- `RandomX::VMRef::hashing_mutex` (per-VM lock)

**Thread safety notes**:
- Global mutex causes contention during concurrent POW checks
- **TODO**: Move to lock-free VM pool (per CONCURRENCY_ANALYSIS.md)

---

### 7. RandomX::VMRef::hashing_mutex (Per-VM)

**Location**: `src/crypto/randomx_pow.cpp:XX`

**Protects**:
- Individual RandomX VM during hash calculation

**Held during**:
- `randomx_calculate_hash()` call

**Acquire AFTER**:
- `RandomX::g_randomx_mutex` (after getting VM from pool)

**Thread safety notes**:
- Fine-grained per-VM locking allows parallel hashing
- Still bottlenecked by global mutex during VM acquisition

---

## Atomic Variables (Lock-Free)

These use lock-free atomics instead of mutexes:

### NetworkManager Sync State

**Variables**:
- `std::atomic<uint64_t> sync_peer_id_`
- `std::atomic<int64_t> sync_start_time_`
- `std::atomic<int64_t> last_headers_received_`

**Memory ordering**:
- Writes: `std::memory_order_release`
- Reads: `std::memory_order_acquire`
- Compare-exchange: `compare_exchange_strong()`

**Why atomic**:
- Multiple io_context threads check/set sync peer
- No mutex needed for simple integer operations
- Acquire/release ensures visibility of related updates

**Example**:
```cpp
// network_manager.cpp:480
uint64_t expected = 0;
if (sync_peer_id_.compare_exchange_strong(expected, peer->id(),
                                         std::memory_order_release,
                                         std::memory_order_acquire)) {
    // Atomically claimed sync peer - no race!
    sync_start_time_.store(now_us, std::memory_order_release);
}
```

---

## Common Deadlock Scenarios and Solutions

### Scenario 1: Validation During Peer Message Handling

**Bad** (potential deadlock):
```cpp
// Thread 1: Processing HEADERS message
{
    std::lock_guard<std::mutex> lock_peers(peer_manager_.mutex_);
    Peer* peer = peer_manager_.get_peer(peer_id);

    // Now need to validate header
    std::lock_guard<std::mutex> lock_chainstate(chainstate_manager_.validation_mutex_);
    chainstate_manager_.AcceptBlockHeader(header);  // DEADLOCK RISK!
}

// Thread 2: Mining new block
{
    std::lock_guard<std::mutex> lock_chainstate(chainstate_manager_.validation_mutex_);
    chainstate_manager_.ConnectTip(new_block);

    // Now need to relay to peers
    std::lock_guard<std::mutex> lock_peers(peer_manager_.mutex_);  // DEADLOCK RISK!
    for (auto* peer : peer_manager_.get_all_peers()) { ... }
}
```

**Good** (correct order):
```cpp
// Always acquire validation_mutex_ first!
std::lock_guard<std::mutex> lock_chainstate(chainstate_manager_.validation_mutex_);
chainstate_manager_.AcceptBlockHeader(header);

// Then acquire peer mutex if needed
std::lock_guard<std::mutex> lock_peers(peer_manager_.mutex_);
for (auto* peer : peer_manager_.get_all_peers()) { ... }
```

### Scenario 2: Nested Peer Locks

**Bad** (potential deadlock):
```cpp
// Thread 1: Iterating peers
{
    std::lock_guard<std::mutex> lock_pm(peer_manager_.mutex_);
    for (auto& [id, peer] : peers_) {
        std::lock_guard<std::mutex> lock_peer(peer->send_queue_mutex_);  // OK
    }
}

// Thread 2: Adding peer while sending message
{
    std::lock_guard<std::mutex> lock_peer(some_peer->send_queue_mutex_);  // Acquired first
    // ... some operation ...
    std::lock_guard<std::mutex> lock_pm(peer_manager_.mutex_);  // DEADLOCK RISK!
}
```

**Good** (correct order):
```cpp
// Always acquire PeerManager::mutex_ before Peer::send_queue_mutex_
std::lock_guard<std::mutex> lock_pm(peer_manager_.mutex_);
Peer* peer = peer_manager_.get_peer(peer_id);

if (peer) {
    std::lock_guard<std::mutex> lock_peer(peer->send_queue_mutex_);
    // ... safe to access send_queue
}
```

---

## Testing for Deadlocks

### Static Analysis: ThreadSanitizer

Build with TSan to detect race conditions and deadlocks:

```bash
cmake -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" -DCMAKE_BUILD_TYPE=Debug ..
cmake --build build
./test/functional/feature_chaos_convergence.py
```

TSan will report:
- **Data races**: Unsynchronized access to shared memory
- **Lock-order-inversion**: Potential deadlock from inconsistent lock ordering
- **Deadlocks**: Actual detected deadlocks

### Runtime Testing

Run stress tests with many concurrent operations:
```bash
./test/functional/feature_chaos_convergence.py  # 20 peers, concurrent mining
```

---

## References

- **Unicity/Bitcoin Core**: Uses `cs_main` (global) + per-node locks (`cs_vSend`, `cs_vRecv`)
- **CONCURRENCY_ANALYSIS.md**: Future lock-free improvements (RCU, lock-free queues)
- **Peer mutex fix**: commit message explains send_queue race condition fix
