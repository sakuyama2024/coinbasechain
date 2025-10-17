# Testing with ThreadSanitizer (TSan)

## Overview

ThreadSanitizer (TSan) is a runtime data race detector for C/C++. It detects:
- **Data races**: Unsynchronized access to shared memory
- **Lock-order inversions**: Potential deadlocks from inconsistent lock ordering
- **Use-after-free**: Accessing freed memory

TSan runs at runtime and instruments your code to track all memory accesses and synchronization operations. When it detects a problem, it prints a detailed report with stack traces.

## Building with TSan

Clean the build directory and reconfigure with TSan enabled:

```bash
rm -rf build
mkdir build && cd build
cmake -DSANITIZE=thread ..
cmake --build . -j8
```

The binary will be ~5-10x slower and use 5-10x more memory, but it provides comprehensive race detection.

## Verify TSan is Enabled

Check that the TSan runtime library is linked:

```bash
# macOS
otool -L build/bin/coinbasechain | grep tsan

# Linux
ldd build/bin/coinbasechain | grep tsan
```

You should see: `libclang_rt.tsan_osx_dynamic.dylib` (macOS) or `libtsan.so` (Linux)

## Running Tests with TSan

### Unit Tests

```bash
cd build
./coinbasechain_tests
```

If TSan detects a race, it will print a report and exit with non-zero status.

### Functional Tests

```bash
# Make sure you're using the TSan-built binary
cd test/functional

# Fork resolution test (3 nodes, competing chains)
python3 feature_fork_resolution.py

# Chainstate persistence test (4 nodes, multiple restarts)
python3 feature_chainstate_persistence.py

# Chaos convergence test (20 nodes, concurrent mining) - BEST FOR RACE DETECTION
python3 feature_chaos_convergence.py
```

The **chaos convergence test** is the most effective for finding threading bugs because it:
- Runs 20 nodes concurrently
- Has nodes mining blocks simultaneously
- Nodes connect/disconnect dynamically
- Maximum contention on locks

### Running the Node

```bash
./build/bin/coinbasechain --regtest --datadir=/tmp/coinbasechain-tsan
```

TSan will print warnings to stderr if it detects races during normal operation.

## Understanding TSan Output

### Data Race Example

```
==================
WARNING: ThreadSanitizer: data race (pid=12345)
  Write of size 8 at 0x7b0400001234 by thread T2:
    #0 HeaderSync::ProcessHeaders() src/sync/header_sync.cpp:115
    #1 NetworkManager::handle_message() src/network/network_manager.cpp:584

  Previous read of size 8 at 0x7b0400001234 by thread T1:
    #0 HeaderSync::GetBestHeight() src/sync/header_sync.cpp:229
    #1 NetworkManager::check_initial_sync() src/network/network_manager.cpp:343

  Location is heap block of size 256 at 0x7b0400001200 allocated by main thread:
    #0 operator new
    #1 HeaderSync::HeaderSync()
==================
```

This tells you:
- **What**: Two threads accessed the same memory unsafely
- **Where**: Exact source file and line numbers
- **When**: Thread T2 wrote while T1 read (no synchronization)
- **Variable**: Memory location (can correlate to specific member variable)

### Lock-Order Inversion (Potential Deadlock)

```
==================
WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock)
  Cycle in lock order graph: M1 (0x7b04...) => M2 (0x7b05...) => M1

  Thread T1:
    pthread_mutex_lock M1
    pthread_mutex_lock M2

  Thread T2:
    pthread_mutex_lock M2  # OPPOSITE ORDER!
    pthread_mutex_lock M1
==================
```

This is a **potential** deadlock - TSan detected that locks are acquired in different orders by different threads.

## Common False Positives

TSan is very conservative and may report false positives in some cases:

### 1. Benign Races on Flags

```cpp
std::atomic<bool> shutdown_flag{false};  // Already atomic - no race
```

TSan understands `std::atomic`, so this won't trigger warnings.

### 2. Initialization Before Threading

```cpp
// Set before any threads are created - no race possible
int config_value = 42;
start_threads();  // Threads only read config_value
```

If TSan complains about initialization code, it may be a false positive.

### 3. Lock-Free Algorithms

Hand-written lock-free code may trigger warnings. Use `std::atomic` with proper memory ordering instead.

## Suppressing False Positives

If you're certain a warning is a false positive, you can suppress it:

### Method 1: Annotate the Code

```cpp
#include "util/threadsafety.h"

void SomeFunction() NO_THREAD_SAFETY_ANALYSIS {
    // TSan will not analyze this function
}
```

### Method 2: Suppression File

Create `tsan_suppressions.txt`:

```
race:HeaderSync::GetProgress
mutex:RandomXInit
```

Run with:

```bash
TSAN_OPTIONS="suppressions=tsan_suppressions.txt" ./coinbasechain_tests
```

## Performance Impact

| Metric | Normal Build | TSan Build |
|--------|--------------|------------|
| Runtime | 1x | 5-15x slower |
| Memory | 1x | 5-10x more |
| Binary Size | ~5MB | ~15MB |

**Don't use TSan builds in production** - they're for testing only.

## Best Practices

1. **Run TSan regularly during development**
   Catch races early before they become hard-to-debug production crashes

2. **Run functional tests with TSan before merging**
   The chaos convergence test is especially effective

3. **Fix races immediately**
   Threading bugs are hard to reproduce - fix them while TSan shows you exactly where they are

4. **Check CI logs for TSan warnings**
   Automated testing should include TSan runs

5. **TSan + chaos test = maximum confidence**
   If your code passes 20-node chaos test under TSan, it's very likely thread-safe

## Other Sanitizers

CMake also supports:

```bash
# AddressSanitizer (memory errors: use-after-free, buffer overruns)
cmake -DSANITIZE=address ..

# UndefinedBehaviorSanitizer (undefined behavior: integer overflow, null deref)
cmake -DSANITIZE=undefined ..
```

**Note**: You can't combine TSan with ASan (they're incompatible), but you can combine ASan+UBSan.

## References

- [ThreadSanitizer Documentation](https://github.com/google/sanitizers/wiki/ThreadSanitizerCppManual)
- [Clang Thread Safety Analysis](https://clang.llvm.org/docs/ThreadSafetyAnalysis.html)
- [LOCKING_ORDER.md](LOCKING_ORDER.md) - Our locking hierarchy documentation
