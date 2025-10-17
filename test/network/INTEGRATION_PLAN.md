# Integration Plan: Real NetworkManager with SimulatedNetwork

## Current Status

We have a working test harness that tests the simulation framework itself, but NOT the real P2P code:
- ✅ 16/24 tests passing (67%)
- ✅ SimulatedNetwork (message routing, latency, packet loss, partitions)
- ✅ MockChainstateManager (chain state, reorgs)
- ❌ Real NetworkManager NOT integrated
- ❌ Real PeerManager NOT integrated
- ❌ Real message protocol NOT used

## Problem

The `Peer` class uses `boost::asio::ip::tcp::socket` directly in private members. We cannot easily substitute a mock socket because:
1. Socket is a move-only type with platform-specific internals
2. `Peer` constructor takes `boost::asio::ip::tcp::socket` by value
3. Creating a drop-in replacement for boost::asio::ip::tcp::socket is extremely complex

## Solution Options

### Option A: Socket Virtualization (REJECTED - Too Complex)
Create a `SimulatedSocket` that implements the full boost::asio::ip::tcp::socket interface.
- **Pros**: Clean separation
- **Cons**: Requires implementing ~50+ socket methods, complex ASIO integration

### Option B: Intercept at io_context Level (COMPLEX)
Create a custom io_context that intercepts socket creation and routes through SimulatedNetwork.
- **Pros**: Minimal changes to Peer code
- **Cons**: Requires deep ASIO internals knowledge, fragile

### Option C: Keep Current Approach + Document Limitations (RECOMMENDED)
Keep the current simplified test harness, but clearly document that it tests the simulation framework, not the real P2P code.

Then create SEPARATE integration tests that:
- Run real nodes in separate processes
- Use actual TCP sockets on localhost
- Test real NetworkManager/PeerManager behavior
- Use Docker/network namespaces for network conditions

## Recommendation: Hybrid Approach

1. **Keep current test harness** (fast unit tests):
   - Test network simulation logic
   - Test MockChainstate reorg logic
   - Test connection limits, bans, etc. in simplified environment
   - Fix remaining 7 test failures

2. **Add separate integration test suite** (slower, more realistic):
   - Run 2-3 real nodes with actual TCP
   - Test real NetworkManager/PeerManager/HeaderSync
   - Test real message protocol
   - Add as optional "integration_tests" target

## Next Steps

Given the complexity of true socket virtualization, I recommend:

1. **SHORT TERM**: Fix the 7 remaining test failures in current harness
   - Add block relay tracking to prevent relay storms
   - Implement disconnect message handling
   - Tune reorg logic
   - These are simple fixes to the test harness code

2. **MEDIUM TERM**: Document test coverage
   - Clearly label what is tested (simulation framework)
   - Clearly label what is NOT tested (real P2P code)
   - Add comments about limitations

3. **LONG TERM**: Add true integration tests
   - Separate test suite with real nodes
   - Use actual TCP on localhost
   - Can use tools like `tc` (traffic control) for network conditions

## Conclusion

The current test harness is valuable for fast iteration on network simulation logic, but does NOT test the real P2P code. True integration testing requires actual sockets and separate processes.

Would you like me to:
A) Fix the 7 remaining test failures in the current harness (quick)
B) Attempt socket virtualization (complex, may take hours)
C) Start designing a separate integration test framework (medium effort)
