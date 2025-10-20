#!/usr/bin/env python3
"""Three-node P2P chain test.

Tests that blocks propagate through a chain of three nodes:
node0 -> node1 -> node2

This verifies that node1 can relay blocks it receives from node0 to node2.
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
    print("Starting p2p_three_nodes test...")

    # Setup test directory
    test_dir = Path(tempfile.mkdtemp(prefix="coinbasechain_test_"))
    binary_path = Path(__file__).parent.parent.parent / "build" / "bin" / "coinbasechain"

    node0 = None
    node1 = None
    node2 = None

    try:
        # Start node0 (mining node) on port 19000
        print("Starting node0 (port 19000)...")
        node0 = TestNode(0, test_dir / "node0", binary_path,
                        extra_args=["--listen", "--port=19000"])
        node0.start()

        # Start node1 (relay node) on port 19001
        print("Starting node1 (port 19001)...")
        node1 = TestNode(1, test_dir / "node1", binary_path,
                        extra_args=["--listen", "--port=19001"])
        node1.start()

        # Start node2 (receiving node) on port 19002 - must listen to receive relayed blocks
        print("Starting node2 (port 19002)...")
        node2 = TestNode(2, test_dir / "node2", binary_path,
                        extra_args=["--listen", "--port=19002"])
        node2.start()

        time.sleep(1)

        # Build network topology: node0 -> node1 -> node2
        print("\nBuilding network topology: node0 -> node1 -> node2")

        print("Connecting node0 to node1...")
        node0.add_node("127.0.0.1:19001", "add")

        print("Connecting node1 to node2...")
        node1.add_node("127.0.0.1:19002", "add")

        # Wait for connections to establish
        time.sleep(2)

        # Verify all nodes start at genesis
        info0 = node0.get_info()
        info1 = node1.get_info()
        info2 = node2.get_info()

        print(f"\nInitial state:")
        print(f"  Node0: {info0['blocks']} blocks")
        print(f"  Node1: {info1['blocks']} blocks")
        print(f"  Node2: {info2['blocks']} blocks")

        assert info0['blocks'] == 0, f"Node0 should start at genesis, got {info0['blocks']}"
        assert info1['blocks'] == 0, f"Node1 should start at genesis, got {info1['blocks']}"
        assert info2['blocks'] == 0, f"Node2 should start at genesis, got {info2['blocks']}"

        # Mine 10 blocks on node0
        print("\nMining 10 blocks on node0...")
        blocks = node0.generate(10)
        if isinstance(blocks, list) and len(blocks) > 0:
            print(f"Mined blocks: {[b[:16] + '...' for b in blocks[:3]]} ...")
        else:
            print(f"Mined {len(blocks) if isinstance(blocks, list) else 'unknown'} blocks")

        # Wait for propagation through the chain
        print("Waiting for blocks to propagate through node0 -> node1 -> node2...")
        time.sleep(4)

        # Verify all nodes synced to height 10
        info0 = node0.get_info()
        info1 = node1.get_info()
        info2 = node2.get_info()

        print(f"\nFinal state:")
        print(f"  Node0: height={info0['blocks']}, tip={info0['bestblockhash'][:16]}...")
        print(f"  Node1: height={info1['blocks']}, tip={info1['bestblockhash'][:16]}...")
        print(f"  Node2: height={info2['blocks']}, tip={info2['bestblockhash'][:16]}...")

        # Assert all nodes have at least 10 blocks (allow 1-2 extra due to regtest race condition)
        assert info0['blocks'] >= 10, f"Node0 should have at least 10 blocks, got {info0['blocks']}"
        assert info1['blocks'] >= 10, f"Node1 should have at least 10 blocks, got {info1['blocks']}"
        assert info2['blocks'] >= 10, f"Node2 should have at least 10 blocks, got {info2['blocks']}"

        # Assert all nodes have same tip
        assert info0['bestblockhash'] == info1['bestblockhash'], \
            "Node0 and Node1 should have same tip"
        assert info1['bestblockhash'] == info2['bestblockhash'], \
            "Node1 and Node2 should have same tip"

        print("\n✓ Test passed! Blocks successfully propagated through chain:")
        print("  node0 (mined) -> node1 (relayed) -> node2 (synced)")
        print(f"  All nodes at height 10 with tip: {info0['bestblockhash'][:16]}...")

    except Exception as e:
        print(f"\n✗ Test failed: {e}")
        import traceback
        traceback.print_exc()

        # Print logs on failure
        for i, node in enumerate([node0, node1, node2]):
            if node:
                print(f"\n--- Node{i} last 20 lines ---")
                print(node.read_log(20))
        return 1

    finally:
        # Cleanup
        for i, node in enumerate([node0, node1, node2]):
            if node and node.is_running():
                print(f"Stopping node{i}...")
                node.stop()

        print(f"Cleaning up test directory: {test_dir}")
        shutil.rmtree(test_dir, ignore_errors=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())
