# Network Test Architecture - High Level Overview

## Core Concept: Real Code, Simulated Transport

The network tests run **100% production P2P code** but replace TCP sockets with in-memory message passing.

```
┌─────────────────────────────────────────────────────────┐
│                    Production Code                       │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐              │
│  │ Peer.cpp │  │ Network  │  │ Header   │              │
│  │          │  │ Manager  │  │ Sync     │              │
│  │ (Real)   │  │ (Real)   │  │ (Real)   │              │
│  └─────┬────┘  └─────┬────┘  └──────────┘              │
│        │             │                                   │
│        │      Calls send() on                           │
│        │      Transport interface                       │
│        ▼             ▼                                   │
├────────────────────────────────────────────────────────┤
│                  Transport Layer                        │
│                                                          │
│  Production: TCP sockets (RealTransport)                │
│  Tests:      In-memory queues (NetworkBridgedTransport) │
└─────────────────────────────────────────────────────────┘
```

## Key Components

### 1. Transport Abstraction (`Transport` interface)

**Purpose**: Allow production code to be network-agnostic

**Production**: `RealTransport` - uses boost::asio TCP sockets
**Tests**: `NetworkBridgedTransport` - bridges to `SimulatedNetwork`

**Why it works**: Peer and NetworkManager don't know if they're using real TCP or simulated queues. They just call `connection->send(data)`.

### 2. SimulatedNetwork (Test Harness)

**Purpose**: In-memory P2P network simulator

**What it does**:
- Maintains a priority queue of pending messages
- Each message has a `delivery_time_ms` calculated as: `current_time + latency`
- When you call `AdvanceTime(T)`, it delivers all messages with `delivery_time <= T`

**Key insight**: This is a **discrete event simulator** - nothing happens automatically. You must explicitly advance time.

### 3. NetworkBridgedTransport (Glue Layer)

**Purpose**: Connects production Peer/NetworkManager to SimulatedNetwork

**How it works**:
```cpp
// When production code calls send():
bool NetworkBridgedTransport::send(data) {
    // Forward to SimulatedNetwork
    sim_network_->SendMessage(from_node_id, to_node_id, data);
    return true;
}

// When SimulatedNetwork delivers a message:
void NetworkBridgedTransport::on_message_received(from, data) {
    // Call production code's receive callback
    if (receive_callback_) {
        receive_callback_(data);  // -> Peer::on_transport_receive()
    }
}
```

## How A Test Works

### Example: Two Nodes Exchanging Headers

```cpp
// 1. Create simulated network
SimulatedNetwork network(seed);

// 2. Create nodes (each wraps REAL production NetworkManager + Peer)
SimulatedNode node1(1, &network);  // Has REAL Peer, HeaderSync, etc.
SimulatedNode node2(2, &network);

// 3. Connect (creates NetworkBridgedTransport connections)
node1.ConnectTo(2);  // Peer.start() -> send VERSION

// 4. Advance time to process handshake
network.AdvanceTime(100);  // VERSION delivered, VERACK sent

// 5. Node 1 mines a block (production code)
node1.MineBlock();  // HeaderSync processes, Peer sends INV

// 6. Advance time to process INV -> GETHEADERS -> HEADERS
network.AdvanceTime(200);  // INV delivered
network.AdvanceTime(400);  // GETHEADERS delivered
network.AdvanceTime(600);  // HEADERS delivered

// 7. Verify (check production chainstate)
EXPECT_EQ(node2.GetTipHeight(), 1);  // HeaderSync updated chain
```

## Why It's Brittle: The Time Advancement Problem

### The Root Cause

Messages are queued at: **`delivery_time = current_time_ms + latency`**

**Problem**: When you advance time in a large jump, any messages sent DURING processing are scheduled relative to the NEW current time.

### Example of Breakage

```cpp
// Scenario: 500ms latency configured
network.SetNetworkConditions({.latency_min = 500ms});

node1.MineBlock();  // Sends INV at time 100ms

// ❌ WRONG: Jump ahead 5 seconds
network.AdvanceTime(5000);
```

**What happens**:
1. At `AdvanceTime(5000)`, current time jumps from 100ms → 5000ms
2. INV message (queued at 100+500=600ms) is delivered
3. Node 2 processes INV → sends GETHEADERS
4. **GETHEADERS is queued at: `5000 + 500 = 5500ms`** ← BUG!
5. We're already at 5000ms, so GETHEADERS is in the future
6. Test never advances past 5000ms → GETHEADERS never processes
7. Test fails: "Expected tip height 1, got 0"

### Why This Is Brittle

**The brittleness comes from the coupling between**:
1. How fast you advance time (step size)
2. How many round-trips the protocol needs (INV → GETHEADERS → HEADERS = 3 hops)
3. The configured latency

**If you change ANY of these, tests break.**

### The Fix: Gradual Time Advancement

```cpp
// ✓ CORRECT: Small incremental steps
for (int i = 0; i < 20; i++) {
    time_ms += 200;  // Advance 200ms at a time
    network.AdvanceTime(time_ms);
}
```

**Why this works**:
- INV arrives at 600ms during iteration 3 (time=600)
- GETHEADERS queued at 600+500=1100ms
- GETHEADERS arrives at 1100ms during iteration 6 (time=1200)
- HEADERS queued at 1100+500=1600ms
- HEADERS arrives at 1600ms during iteration 8 (time=1600)
- Message chain completes naturally ✓

## Production Code Integration Points

### What Production Code Sees

1. **Peer.cpp**: Calls `connection_->send(data)` - doesn't know it's simulated
2. **NetworkManager.cpp**: Calls `transport_->connect()` - doesn't know it's simulated
3. **HeaderSync.cpp**: Processes headers normally - doesn't know it's a test

### What Gets Replaced

1. **TCP sockets** → In-memory queues (SimulatedNetwork)
2. **Async I/O** → Synchronous event delivery (AdvanceTime)
3. **Wall-clock time** → Simulated time (util::SetMockTime)

### What Stays The Same

- All message serialization/deserialization (100% real)
- All protocol logic (handshake, sync, DoS checks) (100% real)
- All chainstate operations (AcceptBlockHeader, ActivateBestChain) (100% real)

## Benefits

✓ **High fidelity**: Tests run actual production P2P code
✓ **Deterministic**: Fixed random seed → reproducible behavior
✓ **Fast**: No real network I/O or thread scheduling delays
✓ **Controllable**: Can inject latency, packet loss, partitions

## Drawbacks

✗ **Brittle time advancement**: Must advance time carefully in small steps
✗ **Manual event loop**: Must explicitly call AdvanceTime() to make progress
✗ **Coupling**: Test setup tightly coupled to protocol round-trip counts
✗ **Debugging difficulty**: When tests fail, hard to see "what's in the queue"

## Recommendations for Improvement

### 1. Auto-Drain Helper
```cpp
// Instead of manual loops, provide:
network.DrainAllMessages(max_time_ms);
// Automatically advances time until queue is empty or timeout
```

### 2. Better Diagnostics
```cpp
// When tests fail, dump queue state:
network.DumpPendingMessages();
// Shows: "3 messages pending: INV@600ms, GETHEADERS@1100ms, ..."
```

### 3. Protocol-Aware Helpers
```cpp
// Instead of:
for (int i = 0; i < 20; i++) {
    network.AdvanceTime(time_ms += 200);
}

// Provide:
network.CompleteHandshake(node1, node2);
network.SyncHeaders(from_node, to_node);
```

### 4. Hybrid Approach
Consider using **real threads + mock time** instead of discrete event simulation:
- Production code runs in real threads with real async I/O
- Mock `util::GetTime()` for time-based features
- Use real in-memory transport (e.g., Unix domain sockets)
- Less brittle, but loses determinism

## Summary

**The Good**: Tests run 100% real production code, which gives high confidence.

**The Bad**: The discrete event simulation with manual time advancement is extremely brittle. Any mismatch between time step size, latency, and protocol round-trips causes cascading delays and test failures.

**The Fundamental Issue**: **Queueing times are relative to current_time, so advancing time changes the reference point for future messages.** This creates coupling between test structure and protocol timing.

**Workaround**: Always advance time in small, gradual increments (100-200ms steps) to allow message chains to complete naturally. See `TESTING_GUIDELINES.md` for detailed patterns.
