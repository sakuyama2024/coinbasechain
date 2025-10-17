#!/usr/bin/env python3
"""P2P connection test.

Tests that two nodes can connect to each other and maintain the connection.
"""

import sys
import tempfile
import shutil
import time
from pathlib import Path

# Add test framework to path
sys.path.insert(0, str(Path(__file__).parent / "test_framework"))

from test_node import TestNode
from util import wait_until, wait_for_peers


def main():
    """Run the test."""
    print("Starting p2p_connect test...")

    # Setup test directory
    test_dir = Path(tempfile.mkdtemp(prefix="coinbasechain_test_"))
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

        # Check initial state - both should have 0 peers
        info0 = node0.get_info()
        info1 = node1.get_info()
        print(f"Node0 initial state: {info0.get('connections', 0)} connections")
        print(f"Node1 initial state: {info1.get('connections', 0)} connections")

        # Connect node1 to node0
        print("Connecting node1 to node0 at 127.0.0.1:19000...")
        try:
            result = node1.add_node("127.0.0.1:19000", "add")
            print(f"Connection result: {result}")
        except Exception as e:
            print(f"Add node call returned: {e}")
            # This is expected to work, but may not show immediate peers

        # Wait a bit for connection to establish
        print("Waiting for connection to establish...")
        time.sleep(2)

        # Check peer info
        try:
            peers0 = node0.get_peer_info()
            peers1 = node1.get_peer_info()
            print(f"Node0 peers: {peers0}")
            print(f"Node1 peers: {peers1}")
        except Exception as e:
            print(f"Note: getpeerinfo may not be fully implemented yet: {e}")

        # Test basic functionality - generate blocks on node0
        print("Generating 5 blocks on node0...")
        node0.generate(5)

        info0 = node0.get_info()
        print(f"Node0 after mining: {info0['blocks']} blocks")

        # Wait for blocks to propagate
        print("Waiting for blocks to propagate...")
        time.sleep(3)

        # Check node1 state
        info1 = node1.get_info()
        print(f"Node1 state: {info1['blocks']} blocks")

        # Verify blocks propagated
        assert info1['blocks'] == 5, f"Expected node1 to have 5 blocks, got {info1['blocks']}"

        # Verify both nodes have same tip
        assert info0['bestblockhash'] == info1['bestblockhash'], \
            f"Nodes have different tips: node0={info0['bestblockhash']}, node1={info1['bestblockhash']}"

        print("✓ Test passed! Blocks propagated successfully")
        print(f"  Both nodes at height 5 with tip: {info0['bestblockhash'][:16]}...")

    except Exception as e:
        print(f"✗ Test failed: {e}")
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
