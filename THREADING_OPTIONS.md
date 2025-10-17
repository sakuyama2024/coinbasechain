# Threading Options for Validation Safety

## Problem Statement

We have multiple IO threads (from `boost::asio::io_context`) that can simultaneously:
- Process HEADERS messages from different peers
- Call validation functions (AcceptBlockHeader, ActivateBestChain)
- Modify shared data structures (m_block_index, m_chain, setBlockIndexCandidates)

This causes race conditions that will lead to crashes, data corruption, and consensus failures.

## The Options

---

## Option 0: Full cs_main (Unicity's Approach)

**Implementation**: Complete Bitcoin Core-style recursive mutex with batching

**Code Complexity**: ~300+ lines of changes across 10+ files

**What We Built So Far**:
- ✅ `util/threadsafety.hpp` - Thread safety annotations
- ✅ `util/sync.hpp` - LOCK macros, RecursiveMutex, UniqueLock
- ✅ `util/macros.hpp` - UNIQUE_NAME macro
- ✅ `validation/cs_main.{hpp,cpp}` - Global cs_main mutex
- ✅ `CS_MAIN_IMPLEMENTATION_PLAN.md` - Full implementation plan

**What's Left**:
- Add `m_chainstate_mutex` to ChainstateManager
- Split ActivateBestChain into ActivateBestChainStep
- Implement 32-block batching with lock release
- Add EXCLUSIVE_LOCKS_REQUIRED annotations everywhere
- Update all call sites to use LOCK(cs_main)
- Test thoroughly

**Pros**:
- ✅ Matches Unicity exactly
- ✅ Proven in production (Bitcoin Core, Unicity)
- ✅ Supports concurrent RPC during sync
- ✅ Proper lock ordering (cs_main before m_chainstate_mutex)
- ✅ Lock released every 32 blocks (responsive)
- ✅ Future-proof for full validation, mempool, etc.

**Cons**:
- ❌ Complex implementation (~300+ lines)
- ❌ More maintenance burden
- ❌ Overkill for headers-only chain
- ❌ Recursive mutex overhead (small)

**Use When**:
- You want to match Unicity exactly
- You're planning to add transactions/mempool
- You need RPC queries during sync
- You have >10 peers with high header throughput

---

## Option A: Validation Strand (Lightweight)

**Implementation**: Use `boost::asio::strand` to serialize all validation

**Code**:
```cpp
class ChainstateManager {
private:
    boost::asio::strand<boost::asio::io_context::executor_type> validation_strand_;

public:
    ChainstateManager(boost::asio::io_context& io_ctx, ...)
        : validation_strand_(boost::asio::make_strand(io_ctx)) {}

    bool ProcessNewBlockHeader(const CBlockHeader& header, ValidationState& state) {
        std::promise<bool> result;
        auto future = result.get_future();

        boost::asio::post(validation_strand_, [this, header, &state, &result]() {
            // This runs serialized - only one validation at a time
            chain::CBlockIndex* pindex = AcceptBlockHeader(header, state, true);
            if (pindex) {
                TryAddBlockIndexCandidate(pindex);
                result.set_value(ActivateBestChain(nullptr));
            } else {
                result.set_value(false);
            }
        });

        return future.get();
    }

    // Same pattern for RPC queries
    const CBlockIndex* GetTip() const {
        std::promise<const CBlockIndex*> result;
        auto future = result.get_future();

        boost::asio::post(validation_strand_, [this, &result]() {
            result.set_value(block_manager_.GetTip());
        });

        return future.get();
    }
};
```

**Code Complexity**: ~50-100 lines

**Pros**:
- ✅ Uses existing infrastructure (boost::asio)
- ✅ Automatic serialization (strand guarantees)
- ✅ No deadlocks possible
- ✅ Works for RPC queries too
- ✅ Simple to understand
- ✅ No explicit mutex management

**Cons**:
- ❌ Blocks network thread (synchronous wait)
- ❌ Promise/future overhead for every call
- ❌ Doesn't match Unicity pattern
- ❌ All operations serialized (no reader parallelism)

**Use When**:
- You want simple and correct
- You're okay with blocking network threads briefly
- You already use boost::asio
- You don't need to match Unicity exactly

---

## Option B: Validation Queue (Dedicated Thread)

**Implementation**: Single validation thread with work queue

**Code**:
```cpp
class ChainstateManager {
private:
    std::thread validation_thread_;
    std::queue<std::function<void()>> validation_queue_;
    std::mutex queue_mutex_;  // Only for queue access, NOT chain state
    std::condition_variable queue_cv_;
    std::atomic<bool> shutdown_{false};

    void ValidationThreadFunc() {
        while (!shutdown_) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [this] {
                    return !validation_queue_.empty() || shutdown_;
                });

                if (shutdown_) break;

                task = std::move(validation_queue_.front());
                validation_queue_.pop();
            }

            // Execute outside queue lock (no chain state lock needed!)
            task();
        }
    }

public:
    void Start() {
        validation_thread_ = std::thread(&ChainstateManager::ValidationThreadFunc, this);
    }

    void Stop() {
        shutdown_ = true;
        queue_cv_.notify_all();
        if (validation_thread_.joinable()) {
            validation_thread_.join();
        }
    }

    bool ProcessNewBlockHeader(const CBlockHeader& header, ValidationState& state) {
        std::promise<bool> result;
        auto future = result.get_future();

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            validation_queue_.push([this, header, &state, &result]() {
                // Runs on validation thread - single threaded!
                chain::CBlockIndex* pindex = AcceptBlockHeader(header, state, true);
                if (pindex) {
                    TryAddBlockIndexCandidate(pindex);
                    result.set_value(ActivateBestChain(nullptr));
                } else {
                    result.set_value(false);
                }
            });
        }
        queue_cv_.notify_one();

        return future.get();
    }
};
```

**Code Complexity**: ~150 lines

**Pros**:
- ✅ True single-threaded validation
- ✅ Clean separation (validation thread vs network threads)
- ✅ Works for RPC queries
- ✅ No chain state mutex needed
- ✅ More control than strand

**Cons**:
- ❌ More code than Option A
- ❌ Manual thread management
- ❌ Still blocks callers (synchronous)
- ❌ Queue overhead

**Use When**:
- You want explicit control over threading
- You don't want to use boost::asio strand
- You want to decouple validation from networking

---

## Option C: Atomic Flag Guard (Minimalist)

**Implementation**: Single atomic boolean to prevent concurrent validation

### Variant C1: Skip on Contention

**Code**:
```cpp
class ChainstateManager {
private:
    std::atomic<bool> validation_in_progress_{false};

public:
    bool ProcessNewBlockHeader(const CBlockHeader& header, ValidationState& state) {
        // Try to acquire validation "lock"
        bool expected = false;
        if (!validation_in_progress_.compare_exchange_strong(expected, true)) {
            // Another validation in progress - skip this header
            LOG_DEBUG("Validation busy, skipping header");
            return true;  // Header will be re-requested
        }

        // We own validation now
        chain::CBlockIndex* pindex = AcceptBlockHeader(header, state, true);
        if (pindex) {
            TryAddBlockIndexCandidate(pindex);
            ActivateBestChain(nullptr);
        }

        validation_in_progress_.store(false);
        return pindex != nullptr;
    }

    // RPC: Fail during sync
    const CBlockIndex* GetTip() const {
        if (validation_in_progress_.load()) {
            throw std::runtime_error("Chain unavailable during sync");
        }
        return block_manager_.GetTip();
    }
};
```

**Code Complexity**: ~10 lines

**Pros**:
- ✅ Simplest possible solution
- ✅ Zero threading library dependencies
- ✅ No deadlocks possible
- ✅ Fast (atomic ops are cheap)
- ✅ Easy to verify correctness
- ✅ Works great for low-concurrency scenarios

**Cons**:
- ❌ Drops concurrent header messages
- ❌ RPC fails during sync
- ❌ No waiting/queuing
- ❌ Wastes work (dropped headers)

### Variant C2: Spin-Wait

**Code**:
```cpp
bool ProcessNewBlockHeader(const CBlockHeader& header, ValidationState& state) {
    // Wait until validation is available
    bool expected = false;
    while (!validation_in_progress_.compare_exchange_strong(expected, true)) {
        expected = false;
        std::this_thread::yield();
    }

    // ... validation ...

    validation_in_progress_.store(false);
    return true;
}
```

**Pros**:
- ✅ Guarantees header processing
- ✅ Still simple

**Cons**:
- ❌ Busy-waiting wastes CPU
- ❌ Blocks network thread

### Variant C3: Reader/Writer Pattern

**Code**:
```cpp
class ChainstateManager {
private:
    std::atomic<int> active_readers_{0};
    std::atomic<bool> writer_active_{false};

public:
    // Write operations
    bool ProcessNewBlockHeader(...) {
        while (active_readers_.load() > 0) {
            std::this_thread::yield();
        }

        bool expected = false;
        if (!writer_active_.compare_exchange_strong(expected, true)) {
            return true;  // Another writer
        }

        // ... validation ...

        writer_active_.store(false);
        return true;
    }

    // Read operations (RPC)
    const CBlockIndex* GetTip() const {
        while (writer_active_.load()) {
            std::this_thread::yield();
        }

        active_readers_++;
        auto* tip = block_manager_.GetTip();
        active_readers_--;

        return tip;
    }
};
```

**Code Complexity**: ~30 lines

**Pros**:
- ✅ Handles RPC correctly
- ✅ Multiple readers allowed
- ✅ No mutex needed

**Cons**:
- ❌ Still busy-waiting
- ❌ Getting complex (close to just using mutex!)

**Use When (C1)**:
- Simple deployments (1-2 peers)
- Testing/MVP phase
- You want absolute minimum code
- RPC during sync is not required
- Header collisions are rare (they are!)

---

## Comparison Matrix

| Feature | Option 0 (cs_main) | Option A (Strand) | Option B (Queue) | Option C (Atomic) |
|---------|-------------------|-------------------|------------------|-------------------|
| **Code Lines** | ~300+ | ~50-100 | ~150 | ~10-30 |
| **Complexity** | High | Medium | Medium | Low |
| **Matches Unicity** | ✅ Yes | ❌ No | ❌ No | ❌ No |
| **RPC During Sync** | ✅ Yes | ✅ Yes | ✅ Yes | ❌ No (C1) / ✅ Yes (C3) |
| **Handles Concurrency** | ✅ Queues | ⚠️ Blocks | ⚠️ Blocks | ❌ Drops (C1) |
| **Lock Overhead** | Medium | Low | Low | Minimal |
| **Future-Proof** | ✅ Yes | ⚠️ Maybe | ⚠️ Maybe | ❌ No |
| **Deadlock Risk** | ⚠️ Possible | ✅ None | ✅ None | ✅ None |
| **Testing Burden** | High | Medium | Medium | Low |

---

## Real-World Considerations

### Header Arrival Pattern

In practice, headers arrive:
- **Sequentially**: 10-2000ms apart
- **From one peer**: Usually sync from single peer initially
- **Rarely concurrent**: Only during initial sync from multiple peers

**This means**: Actual contention is RARE. Option C will almost never drop headers.

### RPC Usage During Sync

How often do you query chain state during sync?
- **Rarely**: Most RPC calls happen after sync
- **Monitoring**: Maybe check progress every few seconds
- **Not critical**: Can wait for sync to finish

**This means**: Option C's "RPC fails during sync" is acceptable.

### Performance Impact

Validation time per header: ~0.1-1ms (PoW check dominates)
Network thread blocked: Option A/B block for this duration
Header drop rate: Option C1 drops maybe 1-5% during heavy sync

**This means**: All options have acceptable performance.

---

## Recommendation by Use Case

### For MVP / Testing / Simple Deployment
→ **Option C1 (Atomic Flag - Skip)**
- Minimal code
- Easy to verify
- Good enough for 1-10 peers
- Can always upgrade later

### For Production (Headers-Only)
→ **Option A (Strand)**
- Good balance of simplicity and correctness
- Uses existing infrastructure
- Handles RPC properly
- Professional solution

### For Full Node Future
→ **Option 0 (cs_main)**
- Matches Unicity exactly
- Proven pattern
- Supports everything
- Worth the complexity

### For Custom Requirements
→ **Option B (Queue)**
- Full control
- Clean separation
- Custom scheduling possible

---

## Migration Path

Start simple, upgrade if needed:

1. **Start**: Option C1 (10 lines)
2. **If RPC needed during sync**: Upgrade to Option A (add ~50 lines)
3. **If planning mempool**: Upgrade to Option 0 (full cs_main)

Each step is backward compatible - existing code keeps working.

---

## Current Status

We have built the infrastructure for Option 0:
- Threading primitives (sync.hpp, threadsafety.hpp)
- Global cs_main declaration
- Implementation plan

**Decision Point**: Continue with Option 0, or pivot to simpler option?

**My Recommendation**: Start with **Option C1** for now:
- Get something working FAST
- Test with your current scenarios
- Upgrade to Option A or 0 later if needed
- Infrastructure is already built (not wasted)

The threading primitives we built are still useful:
- Option A can use them
- Option B can use them
- Even Option C might use GUARDED_BY annotations for documentation

---

## Code Examples in Context

See:
- `CS_MAIN_IMPLEMENTATION_PLAN.md` - Full Option 0 plan
- This file - All other options

Ready to implement whichever you choose!
