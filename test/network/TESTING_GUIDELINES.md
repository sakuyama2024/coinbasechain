# Network Testing Guidelines

## Simulated Network Time Advancement

### The Problem

When testing with simulated network latency, advancing time in large jumps breaks message chains and causes tests to fail unpredictably.

### Root Cause

Messages in SimulatedNetwork are queued with:
```cpp
delivery_time = current_time_ms + latency
```

When you advance time in a large jump:

1. Message A arrives and is processed
2. Node responds by sending Message B
3. Message B is queued at: **current_time_ms** + latency
4. But current_time_ms is NOW the time you just jumped to
5. Result: Message B is scheduled far in the future, breaking the message chain

### Example of Broken Pattern

```cpp
// ❌ WRONG - This will fail with latency!
node1.ConnectTo(2);
network.AdvanceTime(100);  // Handshake

network.SetNetworkConditions(conditions);  // 500ms latency
node1.MineBlock();

network.AdvanceTime(5000);  // Skip ahead - BREAKS!
EXPECT_EQ(node2.GetTipHeight(), 1);  // FAILS
```

**Why it fails:**
- INV sent at 100ms, arrives at 600ms
- Test jumps to 5000ms
- GETHEADERS is queued at 5000 + 500 = 5500ms
- GETHEADERS never processes because time doesn't advance further

### Correct Pattern

```cpp
// ✅ CORRECT - Gradual advancement
node1.ConnectTo(2);
network.AdvanceTime(100);  // Handshake

network.SetNetworkConditions(conditions);  // 500ms latency
node1.MineBlock();

// Advance time gradually in small increments
for (int i = 0; i < 20; i++) {
    time_ms += 200;
    network.AdvanceTime(time_ms);
}

EXPECT_EQ(node2.GetTipHeight(), 1);  // PASSES
```

**Why it works:**
- INV arrives at 600ms during first iteration
- GETHEADERS is queued at 600 + 500 = 1100ms
- GETHEADERS processes during 6th iteration (1200ms)
- HEADERS is queued and delivered naturally
- Message chain completes successfully

## Best Practices

### 1. Use Fixed Latency for Deterministic Tests

```cpp
// Use fixed latency, not random ranges
conditions.latency_min = std::chrono::milliseconds(500);
conditions.latency_max = std::chrono::milliseconds(500);  // Same as min
conditions.jitter_max = std::chrono::milliseconds(0);     // No jitter
```

### 2. Advance Time in Small Steps

```cpp
// Advance in 100-200ms increments
for (int i = 0; i < num_iterations; i++) {
    time_ms += 200;
    network.AdvanceTime(time_ms);
}
```

### 3. Complete Handshake Before Applying Latency

```cpp
// Complete handshake with zero latency first
node1.ConnectTo(2);
network.AdvanceTime(100);

// THEN apply high latency
network.SetNetworkConditions(high_latency_conditions);
```

### 4. Account for Bitcoin's Time-Based Features

Some features (like periodic tip announcements) happen on 30-second intervals:

```cpp
// For testing periodic re-announcements
time_ms += 35000;  // Wait 35 seconds (30s interval + buffer)
network.AdvanceTime(time_ms);
```

## Common Pitfalls

### Pitfall 1: Testing with Random Latency

```cpp
// ❌ Causes non-deterministic test failures
conditions.latency_min = std::chrono::milliseconds(500);
conditions.latency_max = std::chrono::milliseconds(1000);  // Random!
```

**Fix:** Use fixed latency for deterministic tests.

### Pitfall 2: Single Large Time Jump

```cpp
// ❌ Breaks message chains
network.AdvanceTime(10000);
```

**Fix:** Use loop with small increments.

### Pitfall 3: Applying Latency Before Handshake

```cpp
// ❌ Handshake messages may be dropped
network.SetNetworkConditions(high_latency_conditions);
node1.ConnectTo(2);
```

**Fix:** Complete handshake first, then apply adverse conditions.

## Debug Tips

### Enable Message Tracing

The debug output shows message delivery times:

```
[DEBUG] SendMessage: from=1, to=2, delivery_time=600 ms, QUEUED
```

If you see delivery times far in the future (e.g., 10000ms+), you're likely skipping ahead.

### Check Message Chains

A typical block propagation chain:
```
INV (announce) → GETHEADERS (request) → HEADERS (deliver)
```

Each hop adds latency. With 500ms latency:
- INV: 100ms + 500ms = 600ms
- GETHEADERS: 600ms + 500ms = 1100ms
- HEADERS: 1100ms + 500ms = 1600ms

Minimum wait time: ~1600ms, but use 2-4x buffer for safety.

## Summary

**Golden Rule:** When testing with network latency, advance time gradually in small steps (100-200ms), not in large jumps. This allows message chains to complete naturally without artificial delays.
