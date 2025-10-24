# Memory Leak Detection

This document explains how to detect memory leaks in production code (not test infrastructure).

## Platform-Specific Tools

### macOS (Current Platform)

On macOS ARM64 (Apple Silicon), LeakSanitizer is **not supported**. Instead, use Apple's native `leaks` tool:

#### Option 1: Manual Check (Most Control)

```bash
# Build normally (no special flags needed)
cd build && cmake --build . --parallel

# Run tests in background with malloc logging
MallocStackLogging=1 ./coinbasechain_tests "[banman]" &
TEST_PID=$!

# Wait for tests to complete
wait $TEST_PID

# Check for leaks
leaks $TEST_PID
```

#### Option 2: Quick Check (Recommended)

```bash
# Check leaks after running tests
MallocStackLogging=1 ./build/coinbasechain_tests "[banman]"

# In another terminal while tests run:
leaks coinbasechain_tests
```

#### Option 3: Use Helper Script

```bash
# Run all tests and get leak report
./check_leaks_macos.sh

# Run specific test category
./check_leaks_macos.sh "[banman]"
```

#### Option 4: Use Instruments GUI

```bash
# Open Instruments app
open /Applications/Xcode.app/Contents/Applications/Instruments.app

# Choose "Leaks" template
# Select your test binary: build/coinbasechain_tests
# Add arguments: "[banman]"
# Click Record
```

### Linux

On Linux, use AddressSanitizer with LeakSanitizer:

```bash
# 1. Build with sanitizers
rm -rf build && mkdir build && cd build
cmake -DSANITIZE=address ..
cmake --build . --parallel

# 2. Run tests with leak detection
./run_leak_check.sh              # All tests
./run_leak_check.sh "[network]"  # Specific category
```

The `lsan.supp` file suppresses known test infrastructure leaks (Catch2, test fixtures), so only **production code leaks** are reported.

## Suppression File (`lsan.supp`)

The suppression file tells LeakSanitizer to ignore allocations from:

- **Catch2 test framework** (`Catch::`, `catch_amalgamated`)
- **Test fixtures** (`*Test*::SetUp`, `*TestFixture*`)
- **Test helpers** (`SimulatedNetwork::`, `SimulatedNode::`)
- **Logging in tests** (`spdlog::`, `*test_logger*`)
- **Test-only Boost ASIO** (production should clean these up)

**This means:** Any leaks reported are in **production code** (network/, chain/, validation/) and should be fixed.

## What Gets Detected

### AddressSanitizer (Linux)
- Memory leaks
- Heap buffer overflow
- Stack buffer overflow
- Use-after-free
- Use-after-return
- Double-free

### macOS Leaks Tool
- Memory leaks only
- Native integration with malloc zones
- Works with all Apple Silicon binaries

## Example: Fixing a Leak

If you see a leak like this:

```
Direct leak of 1024 bytes in 1 object(s) allocated from:
    #0 operator new
    #1 NetworkManager::AddPeer() at peer_manager.cpp:145
    #2 NetworkManager::ConnectToNode() at peer_manager.cpp:89
```

**Action:** Review `peer_manager.cpp:145` - the `AddPeer()` function is allocating memory that's not being freed. Likely missing a destructor call or smart pointer.

## Testing Strategy

1. **Run leak check on specific components:**
   ```bash
   ./check_leaks_macos.sh "[banman]"    # Test ban manager
   ./check_leaks_macos.sh "[network]"   # Test network code
   ./check_leaks_macos.sh "[chain]"     # Test blockchain code
   ```

2. **Fix any leaks found** - They're in production code, not test infrastructure

3. **Re-run to verify fix**

4. **Run full test suite** before committing

## Current Status

✅ Build configured with AddressSanitizer (`-DSANITIZE=address`)
✅ Leak suppression file created (`lsan.supp`)
✅ Helper scripts created for both platforms
⚠️  macOS ARM64: LSan not supported, use native `leaks` tool

## Notes

- **Test infrastructure leaks are expected** and suppressed
- **Production code leaks should be zero**
- AddressSanitizer adds ~2x slowdown but catches critical bugs
- Always test with sanitizers before major releases
