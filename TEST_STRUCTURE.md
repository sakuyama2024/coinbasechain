# Test Directory Structure Explained

## Overview

```
test/
├── unit/              ← Fast, isolated C++ tests (individual functions)
├── integration/       ← Medium C++ tests (multiple components together)
├── network/           ← C++ network simulation tests (simulated network)
├── functional/        ← Slow Python tests (real binaries, real network)
└── data/              ← Pre-generated test chains
```

---

## 1. `test/unit/` - Unit Tests (C++ with Catch2)

**What:** Test individual functions and classes in isolation  
**Language:** C++  
**Framework:** Catch2  
**Speed:** Very fast (milliseconds)  
**Network:** No networking (pure logic)  

**What they test:**
- Individual functions work correctly
- Data structures behave as expected
- Algorithms produce correct results
- Edge cases are handled

**Examples:**
```cpp
test/unit/uint_tests.cpp         - Test uint256 arithmetic
test/unit/block_tests.cpp        - Test block serialization
test/unit/chain_tests.cpp        - Test Chain class methods
test/unit/pow_tests.cpp          - Test POW validation
test/unit/addr_manager_tests.cpp - Test AddressManager logic
```

**How they run:**
```bash
./coinbasechain_tests "[unit]"
```

**Characteristics:**
- ✅ Very fast (run 100+ tests in seconds)
- ✅ No external dependencies
- ✅ Test pure logic
- ❌ Don't test integration between components
- ❌ Don't test real network behavior

---

## 2. `test/integration/` - Integration Tests (C++ with Catch2)

**What:** Test multiple components working together  
**Language:** C++  
**Framework:** Catch2  
**Speed:** Medium (seconds)  
**Network:** Uses C++ code, but may not spawn processes

**What they test:**
- Components interact correctly
- Subsystems work together
- Complex scenarios with multiple parts

**Examples:**
```cpp
test/integration/header_sync_adversarial_tests.cpp  - Header sync under attack
test/integration/invalidateblock_tests.cpp          - Block invalidation
test/integration/orphan_dos_tests.cpp               - Orphan block DoS protection
test/integration/security_attack_simulations.cpp    - Various attack scenarios
test/integration/banman_adversarial_tests.cpp       - Peer banning under attack
```

**How they run:**
```bash
./coinbasechain_tests "[integration]"
```

**Characteristics:**
- ✅ Test component interactions
- ✅ More realistic scenarios than unit tests
- ✅ Still fast enough to run frequently
- ❌ Don't test full end-to-end system
- ❌ Don't use real network processes

---

## 3. `test/network/` - Network Simulation Tests (C++ with Catch2)

**What:** Test P2P networking using SIMULATED network infrastructure  
**Language:** C++  
**Framework:** Catch2 + Custom SimulatedNetwork  
**Speed:** Medium (seconds)  
**Network:** **Simulated** (in-memory message passing, no real sockets)

**What they test:**
- P2P protocol message handling
- Multi-node scenarios
- Network attacks and edge cases
- Block propagation logic
- Peer discovery and management

**Examples:**
```cpp
test/network/peer_connection_tests.cpp        - Peer connection lifecycle
test/network/sync_ibd_tests.cpp               - IBD simulation
test/network/reorg_partition_tests.cpp        - Network partition reorgs
test/network/attack_simulation_tests.cpp      - Network attack scenarios
test/network/misbehavior_penalty_tests.cpp    - DoS protection
test/network/peer_discovery_tests.cpp         - Peer discovery via ADDR
test/network/invalidateblock_functional_tests.cpp - InvalidateBlock RPC
```

**Key Infrastructure:**
```cpp
SimulatedNetwork   - In-memory message bus (not real TCP)
SimulatedNode      - Node with simulated transport
BridgedTransport   - Connects nodes via SimulatedNetwork
```

**How they run:**
```bash
./coinbasechain_tests "[network]"
```

**Characteristics:**
- ✅ Fast (no real network latency)
- ✅ Deterministic (controlled timing)
- ✅ Test complex multi-node scenarios
- ✅ Can inject messages directly
- ⚠️ Uses **simulated network** (not real TCP/IP)
- ❌ Don't test real binary execution
- ❌ Don't test RPC layer

---

## 4. `test/functional/` - Functional Tests (Python)

**What:** End-to-end tests with REAL node processes and REAL network  
**Language:** Python  
**Framework:** unittest/subprocess  
**Speed:** Slow (seconds to minutes)  
**Network:** **Real TCP/IP** (localhost but real sockets)

**What they test:**
- Complete system works end-to-end
- Real binaries execute correctly
- RPC server works
- Real P2P networking (TCP sockets)
- Multi-process coordination
- Real consensus across nodes

**Examples:**
```python
test/functional/p2p_connect.py                      - Real P2P connection
test/functional/p2p_ibd.py                          - Real IBD over network
test/functional/p2p_reorg.py                        - Real reorg scenarios
test/functional/feature_chaos_convergence.py        - Chaos testing
test/functional/feature_fork_resolution.py          - Fork resolution
```

**How they work:**
```python
# Spawn REAL processes
node0 = TestNode(0, datadir, binary_path)
node0.start()  # Spawns: ./bin/coinbasechain --regtest

# Real RPC calls via CLI
node0.generate(10)  # Executes: ./bin/coinbasechain-cli generate 10

# Real P2P connection
node1.add_node("127.0.0.1:19000")  # Real TCP socket connection
```

**How they run:**
```bash
python3 test/functional/p2p_connect.py
# or
python3 test/functional/test_runner.py  # Run all
```

**Characteristics:**
- ✅ Test the ACTUAL system (not mocks)
- ✅ Real networking (TCP/IP on localhost)
- ✅ Real RPC server
- ✅ Test binary execution
- ✅ Catch integration bugs unit tests miss
- ❌ Slow (spawn processes, network overhead)
- ❌ Less control than simulated tests
- ❌ Harder to debug failures

---

## Key Differences Summary

| Aspect | Unit | Integration | Network | Functional |
|--------|------|-------------|---------|------------|
| **Language** | C++ | C++ | C++ | Python |
| **Speed** | ⚡ Very fast | 🏃 Fast | 🚶 Medium | 🐌 Slow |
| **Network** | None | Minimal | Simulated | **Real TCP** |
| **Processes** | Single | Single | Single | **Multiple** |
| **Scope** | Function | Component | Protocol | **System** |
| **Isolation** | High | Medium | Low | None |
| **Realism** | Low | Medium | High | **Highest** |

---

## When to Use Each

### Use Unit Tests (`test/unit/`) when:
- Testing a single function or class
- Testing data structures
- Testing algorithms
- Testing edge cases in isolation
- Need very fast feedback

**Example:** Does `uint256::Add()` correctly add two numbers?

---

### Use Integration Tests (`test/integration/`) when:
- Testing multiple components together
- Testing subsystem interactions
- Testing attack scenarios
- Need faster than functional but more realistic than unit

**Example:** Does orphan block handling correctly interact with the DoS protection system?

---

### Use Network Tests (`test/network/`) when:
- Testing P2P protocol logic
- Testing multi-node scenarios
- Need deterministic network behavior
- Want to inject specific messages
- Need fast but realistic network testing

**Example:** Does IBD work correctly when peer sends batches of headers?

---

### Use Functional Tests (`test/functional/`) when:
- Testing the complete system
- Testing RPC commands
- Testing multi-process scenarios
- Need to verify real binary execution
- Testing deployment scenarios

**Example:** Can a fresh node sync the entire chain from a peer using real P2P networking?

---

## Test Pyramid

The ideal distribution follows the test pyramid:

```
        /\
       /  \      ← Functional (20 tests)  - Slow, high-level
      /    \     
     /------\    ← Network (15 tests)     - Medium, simulated P2P
    /        \
   /----------\  ← Integration (10 tests) - Component interactions
  /            \
 /--------------\ ← Unit (50+ tests)      - Fast, low-level
```

**More unit tests at the bottom** (fast feedback)  
**Fewer functional tests at the top** (catch integration issues)

---

## Current Test Coverage

```bash
# Count tests in each category
$ find test/unit -name "*_tests.cpp" | wc -l
15

$ find test/integration -name "*_tests.cpp" | wc -l
9

$ find test/network -name "*_tests.cpp" | wc -l
16

$ find test/functional -name "*.py" | grep -v framework | grep -v runner | wc -l
20
```

**Your project has excellent test coverage across all levels!** 🎉

---

## Example Test Flow

When you run `./coinbasechain_tests`, it executes:

1. **Unit tests** - Validate basic building blocks
2. **Integration tests** - Validate subsystems work together
3. **Network tests** - Validate P2P protocol with simulated network

When you run `python3 test/functional/test_runner.py`:

- Spawns real node processes
- Tests complete end-to-end scenarios
- Validates the system works in production-like conditions

---

## Summary

- **`test/unit/`** - Fast C++ tests of individual functions (no network)
- **`test/integration/`** - Medium C++ tests of component interactions
- **`test/network/`** - C++ tests using **simulated** network (fast, deterministic)
- **`test/functional/`** - Slow Python tests using **real** processes and **real** TCP network

The main distinction is:
- **Network tests** = Simulated network (in-memory message passing)
- **Functional tests** = Real network (actual TCP sockets on localhost)

Both are valuable for different reasons!
