# Network Multi-Threading Improvements

**Status**: Future work (post-NetworkNotifications refactoring)
**Branch**: feature/network-notifications
**Priority**: Medium (performance optimization)

## Background

The NetworkNotifications refactoring (completed) eliminated circular dependencies and established a clean event-driven architecture. This makes it safe to scale to multiple I/O threads.

## Current State

- ✅ NetworkNotifications singleton with thread-safe subscription registry
- ✅ Components isolated with per-component locking
- ✅ RAII subscription management prevents use-after-free
- ✅ Single synchronization point for peer lifecycle events
- ⚠️ `io_threads = 1` (RealTransport constructor, network_manager.cpp)

## Proposed Improvements

### Phase 1: Increase I/O Threads (Low hanging fruit)
**Effort**: 1-2 days
**Risk**: Low (foundation is solid)

1. Increase `RealTransport io_threads` from 1 to 4-8
2. Run full test suite under ThreadSanitizer
3. Load test with multiple concurrent connections
4. Profile lock contention in NetworkNotifications

**Expected benefit**: 2-4x throughput for peer message handling

### Phase 2: Notification Thread Pool (Optional)
**Effort**: 3-5 days
**Risk**: Medium

Add dedicated thread pool for notification callbacks to avoid blocking publishers:

```cpp
class NetworkNotifications {
  boost::asio::thread_pool notification_pool_{4};

  void NotifyPeerDisconnected(...) {
    // Copy callbacks under lock
    std::vector<Callback> cbs;
    {
      std::lock_guard lock(mutex_);
      cbs = /* snapshot active callbacks */;
    }

    // Execute on thread pool (out of lock)
    for (auto& cb : cbs) {
      boost::asio::post(notification_pool_, [cb, ...]() { cb(...); });
    }
  }
};
```

**Expected benefit**: Reduce notification latency, prevent slow subscribers from blocking publishers

### Phase 3: Lock-Free Subscription Registry (Advanced)
**Effort**: 1-2 weeks
**Risk**: High (correctness-critical)

Replace `std::vector<CallbackEntry>` with lock-free data structure (e.g., boost::lockfree::queue or custom RCU).

**Expected benefit**: Eliminate contention on notification hot path

## Testing Requirements

Before increasing `io_threads`:
1. ✅ All existing tests pass (16,342 assertions)
2. ❌ ThreadSanitizer clean run
3. ❌ Stress test: 1000+ concurrent peers
4. ❌ Long-duration test: 24h stability run
5. ❌ Profile lock contention with `perf`

## Related Files

**Core Infrastructure**:
- `include/network/notifications.hpp` - Event system
- `src/network/notifications.cpp` - Implementation
- `src/network/network_manager.cpp:53` - RealTransport(io_threads)

**Thread-Safe Components**:
- `src/network/peer_manager.cpp` - Publisher (mutex-protected)
- `src/network/header_sync_manager.cpp` - Subscriber (sync_mutex_)
- `src/network/block_relay_manager.cpp` - Subscriber (announce_mutex_)
- `src/network/message_router.cpp` - Subscriber (addr_mutex_)

## References

- Bitcoin Core: Uses 1 thread for net processing (net.cpp:ThreadMessageHandler)
- Ethereum: Uses multi-threaded peer message handling
- Rust libp2p: Lock-free notification system

## Notes

- Current bottleneck is likely I/O, not CPU
- Profile before optimizing further
- Consider io_uring on Linux for even better I/O performance
