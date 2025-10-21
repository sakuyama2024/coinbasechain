# Disabled Tests

These tests are marked with `[.]` tag and are skipped during normal test runs.

**Total: 6 disabled tests**

## Stress Tests (1)

- **test/integration/stress_threading_tests.cpp:289**
  - Test: "Long-running concurrent operations"
  - Tags: `[stress][threading][.slow]`
  - Reason: Long-running test, only run when explicitly requested

## Network Tests (1)

- **test/network/peer_tests.cpp:434**
  - Test: "Peer - HandshakeTimeout"
  - Tags: `[.][timeout]`
  - Reason: Slow timeout test
  - Run with: `./coinbasechain_tests "[timeout]"`

## NAT/UPnP Integration Tests (2)

- **test/network/nat_manager_tests.cpp:92**
  - Test: "NAT Manager - UPnP Integration"
  - Tags: `[nat][network][integration][.]`
  - Reason: Real UPnP/NAT integration test (slow, requires network)

- **test/network/nat_manager_tests.cpp:124**
  - Test: "NAT Manager - Start Twice"
  - Tags: `[nat][network][integration][.]`
  - Reason: Real UPnP/NAT integration test

## Scale Tests (2)

- **test/network/reorg_partition_tests.cpp:591**
  - Test: "ScaleTest - HundredNodes"
  - Tags: `[.][scaletest][network]`
  - Reason: Very slow scale test (100 nodes)

- **test/network/reorg_partition_tests.cpp:640**
  - Test: "ScaleTest - ThousandNodeStressTest"
  - Tags: `[.][scaletest][network]`
  - Reason: Extremely slow stress test (1000 nodes)

---

## Running Disabled Tests

To run specific disabled tests:

```bash
# Run all disabled tests
./build/coinbasechain_tests "[.]"

# Run specific category
./build/coinbasechain_tests "[timeout]"
./build/coinbasechain_tests "[nat]"
./build/coinbasechain_tests "[scaletest]"
./build/coinbasechain_tests "[.slow]"

# Run network tests excluding disabled ones
./build/coinbasechain_tests "[network]~[.]"
```

## Test Count Summary

- **Active tests:** 362 test cases
- **Disabled tests:** 6 test cases
- **Total tests:** 368 test cases
