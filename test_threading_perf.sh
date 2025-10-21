#!/bin/bash
# Test multi-threading performance by comparing sync speeds

set -e

echo "=== Multi-Threading Performance Test ==="
echo ""

# Build test if needed
if [ ! -f "./build/coinbasechain_tests" ]; then
    echo "Building tests..."
    cmake --build build --target coinbasechain_tests -j8
fi

echo "Running network sync test (this measures multi-threaded sync speed)..."
echo ""

# Run the sync test and time it
time ./build/coinbasechain_tests "[sync_ibd]" 2>&1 | grep -E "(PASSED|test cases|assertions|Syncing)"

echo ""
echo "=== Thread Count Test ==="
echo ""

# Start a node temporarily
echo "Starting test node..."
./build/bin/coinbasechain --datadir=/tmp/thread-test --regtest --listen --port=39444 > /tmp/thread-test.log 2>&1 &
NODE_PID=$!

echo "Node started with PID: $NODE_PID"
sleep 3

echo ""
echo "Checking thread count..."
THREAD_COUNT=$(ps -M $NODE_PID 2>/dev/null | wc -l)
echo "Thread count: $THREAD_COUNT"

if [ $THREAD_COUNT -gt 8 ]; then
    echo "✅ Multi-threading is working! ($THREAD_COUNT threads detected)"
else
    echo "⚠️  Expected more threads (got $THREAD_COUNT, expected 10+)"
fi

echo ""
echo "Active threads:"
ps -M $NODE_PID 2>/dev/null | head -20

# Clean up
kill $NODE_PID 2>/dev/null || true
sleep 1
rm -rf /tmp/thread-test

echo ""
echo "=== Test Complete ==="
