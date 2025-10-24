# Quick Start: Memory Leak Detection

## Simple 2-Step Process

### Step 1: Run tests with malloc logging

```bash
cd build
export MallocStackLogging=1
./coinbasechain_tests "[banman]"
```

The tests will run normally. Keep the terminal open.

### Step 2: Check for leaks

In **the same terminal** (or any terminal), run:

```bash
leaks coinbasechain_tests
```

## Example Output

### No Leaks (Good!)
```
Process:         coinbasechain_tests [12345]
Path:            /Users/you/build/coinbasechain_tests
Load Address:    0x100000000
Identifier:      coinbasechain_tests
Version:         0
Code Type:       ARM64
Parent Process:  zsh [12344]

Date/Time:       2025-10-21 15:30:00.000 -0700
Launch Time:     2025-10-21 15:29:55.000 -0700
OS Version:      macOS 14.3 (23D56)
Report Version:  7

leaks Report Version: 4.0
Process 12345: 0 leaks for 0 total leaked bytes.
```

###With Leaks (Need to Fix!)
```
leaks Report Version: 4.0
Process 12345: 2 leaks for 1056 total leaked bytes.

Leak: 0x600001234000  size=1024  zone: DefaultMallocZone_0x100000000
        0x00000001001abcd0  coinbasechain::network::Peer::Peer()  peer.cpp:123
        0x00000001001abc00  coinbasechain::network::NetworkManager::AddPeer()  peer_manager.cpp:456
        0x00000001001ab000  TEST_CASE()  banman_tests.cpp:89
```

This tells you:
- **Production leak** in `Peer::Peer()` at `peer.cpp:123`
- Called from `NetworkManager::AddPeer()` at `peer_manager.cpp:456`
- Triggered by test at `banman_tests.cpp:89`

## Filtering Test Infrastructure Leaks

Look at the call stack. If you see:
- `Catch::` - Test framework leak (ignore)
- `spdlog::` in test code - Test logging leak (ignore)
- `SimulatedNetwork::` - Test helper leak (ignore)

If you see:
- `coinbasechain::network::Peer` - **PRODUCTION CODE LEAK** (fix!)
- `coinbasechain::chain::BlockManager` - **PRODUCTION CODE LEAK** (fix!)
- `coinbasechain::validation::` - **PRODUCTION CODE LEAK** (fix!)

## Alternative: Use Instruments GUI

For a visual interface:

```bash
# 1. Open Instruments
open /Applications/Xcode.app/Contents/Applications/Instruments.app

# 2. Select "Leaks" template
# 3. Click "Choose Target" → Choose File → build/coinbasechain_tests
# 4. In "Arguments" field: [banman]
# 5. Click the red Record button
# 6. Watch leaks appear in real-time with call stacks
```

## Testing Different Components

```bash
export MallocStackLogging=1

# Test ban manager
./coinbasechain_tests "[banman]"

# Test network code
./coinbasechain_tests "[network]"

# Test blockchain
./coinbasechain_tests "[chain]"

# Test all
./coinbasechain_tests
```

Then run `leaks coinbasechain_tests` after each.

## Notes

- ✅ No compiler flags needed for basic leak detection
- ✅ Works on all macOS versions
- ✅ Built into the OS (no installation needed)
- ⚠️  Check call stacks to identify production vs test leaks
- 💡 For automated filtering, use Linux with `lsan.supp`
