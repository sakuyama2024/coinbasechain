#!/usr/bin/env python3
"""Test peer eviction with mocktime.

This test verifies that:
1. Mocktime can be set and advanced via RPC
2. The mockable time system works correctly
3. Future: Test that inactivity timeout disconnects idle peers (20 minutes)
4. Future: Test that ping timeout disconnects unresponsive peers (20 minutes)
"""

import sys
import tempfile
import shutil
import time
from pathlib import Path

# Add test framework to path
sys.path.insert(0, str(Path(__file__).parent / "test_framework"))

from test_node import TestNode


def main():
    """Run the test."""
    print("Starting p2p_eviction test (mocktime)...")

    # Setup test directory
    test_dir = Path(tempfile.mkdtemp(prefix="coinbasechain_test_eviction_"))
    binary_path = Path(__file__).parent.parent.parent / "build" / "bin" / "coinbasechain"

    node0 = None
    node1 = None

    try:
        # Start node0 with listening enabled on port 19000
        print("Starting node0 (listening on port 19000)...")
        node0 = TestNode(0, test_dir / "node0", binary_path,
                        extra_args=["--listen", "--port=19000"])
        node0.start()

        # Start node1 on port 19001 (not listening)
        print("Starting node1 (port 19001)...")
        node1 = TestNode(1, test_dir / "node1", binary_path,
                        extra_args=["--port=19001"])
        node1.start()

        # Give nodes a moment to fully initialize
        time.sleep(1)

        # Connect node1 to node0
        print("Connecting node1 to node0 at 127.0.0.1:19000...")
        try:
            result = node1.add_node("127.0.0.1:19000", "add")
            print(f"Connection result: {result}")
        except Exception as e:
            print(f"Add node call: {e}")

        # Wait for connection to establish
        print("Waiting for connection to establish...")
        time.sleep(2)

        # Generate a few blocks to ensure connection is working
        print("Generating 3 blocks on node0...")
        node0.generate(3)

        info0 = node0.get_info()
        print(f"Node0 after mining: {info0['blocks']} blocks")

        # Wait for blocks to propagate
        print("Waiting for blocks to propagate...")
        time.sleep(2)

        # Check node1 state
        info1 = node1.get_info()
        print(f"Node1 state: {info1['blocks']} blocks")

        # Verify blocks propagated
        assert info1['blocks'] == 3, f"Expected node1 to have 3 blocks, got {info1['blocks']}"

        print("\n=== Testing mocktime functionality ===")

        # Get current time
        current_time = int(time.time())
        print(f"Current real time: {current_time}")

        # Test 1: Set mocktime to current time
        print(f"\nTest 1: Setting mocktime to {current_time}")
        result = node0.rpc("setmocktime", str(current_time))
        print(f"Result: {result}")

        # Test 2: Advance mocktime by 21 minutes (should trigger 20-minute inactivity timeout)
        future_time = current_time + (21 * 60)
        print(f"\nTest 2: Advancing mocktime to {future_time} (+21 minutes)")
        result = node0.rpc("setmocktime", str(future_time))
        print(f"Result: {result}")

        # Wait a bit for any timeout processing
        print("\nWaiting for timeout processing...")
        time.sleep(3)

        # Try to check peer info (may not be fully implemented yet)
        try:
            peers0 = node0.get_peer_info()
            print(f"Node0 peers after timeout: {peers0}")
        except Exception as e:
            print(f"Note: getpeerinfo not fully implemented: {e}")

        # Test 3: Reset mocktime
        print(f"\nTest 3: Resetting mocktime to 0 (disabled)")
        result = node0.rpc("setmocktime", "0")
        print(f"Result: {result}")

        # Verify node still works after mocktime operations
        print("\nVerifying node still works after mocktime operations...")
        node0.generate(2)
        time.sleep(2)

        info0_final = node0.get_info()
        info1_final = node1.get_info()
        print(f"Node0 final state: {info0_final['blocks']} blocks")
        print(f"Node1 final state: {info1_final['blocks']} blocks")

        assert info0_final['blocks'] == 5, f"Expected node0 to have 5 blocks, got {info0_final['blocks']}"
        assert info1_final['blocks'] == 5, f"Expected node1 to have 5 blocks, got {info1_final['blocks']}"

        print("\n✓ Test passed! Mocktime system is working")
        print("  - Mocktime can be set, advanced, and reset")
        print("  - Nodes continue to function correctly with mocktime")
        print("  Note: Full peer eviction testing requires getpeerinfo RPC to return real peer data")

    except Exception as e:
        print(f"\n✗ Test failed: {e}")
        import traceback
        traceback.print_exc()

        # Print logs on failure
        if node0:
            print("\nNode0 last 30 lines of debug.log:")
            print(node0.read_log(30))
        if node1:
            print("\nNode1 last 30 lines of debug.log:")
            print(node1.read_log(30))
        return 1

    finally:
        # Cleanup
        if node0 and node0.is_running():
            print("Stopping node0...")
            node0.stop()
        if node1 and node1.is_running():
            print("Stopping node1...")
            node1.stop()

        print(f"Cleaning up test directory: {test_dir}")
        shutil.rmtree(test_dir, ignore_errors=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())
