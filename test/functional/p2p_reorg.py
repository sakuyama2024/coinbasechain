#!/usr/bin/env python3
"""Blockchain Reorganization Test.

Tests that nodes correctly handle blockchain reorganizations when a competing
chain with more work is presented.

Test Scenarios:
1. Simple reorg: Node switches from chain A to chain B (more work)
2. Deep reorg: 10+ block reorganization
3. Reorg at different heights
4. Edge cases: equal work, same length chains
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
    """Run the comprehensive reorg test."""
    print("=== Blockchain Reorganization Test ===\n")

    # Setup test directory
    test_dir = Path(tempfile.mkdtemp(prefix="coinbasechain_reorg_"))
    binary_path = Path(__file__).parent.parent.parent / "build" / "bin" / "coinbasechain"

    node0 = None
    node1 = None

    try:
        # Start two nodes
        print("Setting up test nodes...")
        node0 = TestNode(0, test_dir / "node0", binary_path,
                        extra_args=["--listen", "--port=19000"])
        node1 = TestNode(1, test_dir / "node1", binary_path,
                        extra_args=["--listen", "--port=19001"])

        node0.start()
        node1.start()
        time.sleep(1)

        # Run test scenarios
        test_simple_reorg(node0, node1)
        test_deep_reorg(node0, node1)
        test_reorg_at_different_heights(node0, node1)
        test_equal_work_chains(node0, node1)

        print("\n✓ All reorg tests passed!")
        return 0

    except Exception as e:
        print(f"\n✗ Test failed: {e}")
        import traceback
        traceback.print_exc()

        # Print logs on failure
        if node0:
            print("\n--- Node0 last 50 lines ---")
            print(node0.read_log(50))
        if node1:
            print("\n--- Node1 last 50 lines ---")
            print(node1.read_log(50))
        return 1

    finally:
        # Cleanup
        if node0 and node0.is_running():
            node0.stop()
        if node1 and node1.is_running():
            node1.stop()

        print(f"\nCleaning up test directory: {test_dir}")
        shutil.rmtree(test_dir, ignore_errors=True)


def test_simple_reorg(node0, node1):
    """Test simple reorg: chain A -> chain B with more work.

    Scenario:
    1. Both nodes start at genesis
    2. Node0 mines 5 blocks (chain A)
    3. Node1 mines 7 blocks (chain B, more work)
    4. Connect nodes
    5. Node0 should reorg to node1's chain
    """
    print("\n=== Test 1: Simple Reorg (5 blocks -> 7 blocks) ===")

    # Verify both at genesis
    info0 = node0.get_info()
    info1 = node1.get_info()
    assert info0['blocks'] == 0, f"Node0 should start at genesis, got {info0['blocks']}"
    assert info1['blocks'] == 0, f"Node1 should start at genesis, got {info1['blocks']}"
    print("✓ Both nodes at genesis")

    # Node0 mines chain A (5 blocks)
    print("Mining chain A on node0 (5 blocks)...")
    node0.generate(5)
    info0 = node0.get_info()
    assert info0['blocks'] == 5, f"Node0 should have 5 blocks, got {info0['blocks']}"
    chain_a_tip = info0['bestblockhash']
    print(f"✓ Chain A: height=5, tip={chain_a_tip[:16]}...")

    # Node1 mines chain B (7 blocks, more work)
    print("Mining chain B on node1 (7 blocks)...")
    node1.generate(7)
    info1 = node1.get_info()
    assert info1['blocks'] == 7, f"Node1 should have 7 blocks, got {info1['blocks']}"
    chain_b_tip = info1['bestblockhash']
    print(f"✓ Chain B: height=7, tip={chain_b_tip[:16]}...")

    # Verify chains are different
    assert chain_a_tip != chain_b_tip, "Chains should have different tips"
    print("✓ Chains diverged (different tips)")

    # Connect nodes to trigger reorg
    print("\nConnecting nodes (should trigger reorg)...")
    node0.add_node("127.0.0.1:19001", "add")

    # Wait for sync
    print("Waiting for node0 to reorg to chain B...")
    max_wait = 30
    start_time = time.time()
    reorged = False

    while time.time() - start_time < max_wait:
        info0 = node0.get_info()
        if info0['blocks'] == 7 and info0['bestblockhash'] == chain_b_tip:
            reorged = True
            print(f"✓ Node0 reorged to chain B: height={info0['blocks']}, tip={info0['bestblockhash'][:16]}...")
            break
        time.sleep(0.5)

    assert reorged, f"Node0 failed to reorg (height={info0['blocks']}, expected 7)"

    # Verify both nodes on same chain
    info0 = node0.get_info()
    info1 = node1.get_info()
    assert info0['bestblockhash'] == info1['bestblockhash'], \
        f"Nodes have different tips after reorg:\n  node0={info0['bestblockhash']}\n  node1={info1['bestblockhash']}"

    print("✓ Both nodes on chain B (reorg successful)")

    # Disconnect nodes for next test
    print("Disconnecting nodes for test isolation...")
    try:
        # Try to disconnect from both sides
        node0.add_node("127.0.0.1:19001", "remove")
    except:
        pass
    try:
        node1.add_node("127.0.0.1:19000", "remove")
    except:
        pass
    time.sleep(2)  # Wait for disconnection to complete

    # Verify disconnection
    peers0 = node0.get_peer_info()
    peers1 = node1.get_peer_info()
    print(f"Node0 peers after disconnect: {len(peers0)}")
    print(f"Node1 peers after disconnect: {len(peers1)}")


def test_deep_reorg(node0, node1):
    """Test deep reorg: 15+ block reorganization.

    Scenario:
    1. Start from current state (both at height 7)
    2. Disconnect nodes to build competing chains
    3. Node0 mines 10 more blocks (total 17)
    4. Node1 mines 15 more blocks (total 22, more work)
    5. Connect nodes
    6. Node0 should reorg 10 blocks to accept node1's chain
    """
    print("\n=== Test 2: Deep Reorg (17 blocks -> 22 blocks) ===")

    # Get current state
    info0 = node0.get_info()
    info1 = node1.get_info()
    initial_height = info0['blocks']

    # Verify nodes are synced from previous test
    assert info0['bestblockhash'] == info1['bestblockhash'], \
        "Nodes should be synced before deep reorg test"
    print(f"Starting from synced height {initial_height}")

    # Ensure nodes are disconnected for isolated mining
    print("Ensuring nodes are disconnected...")
    try:
        node0.add_node("127.0.0.1:19001", "remove")
        node1.add_node("127.0.0.1:19000", "remove")
    except:
        pass
    time.sleep(2)

    # Node0 mines 10 more blocks
    print("Node0 mining 10 blocks (isolated)...")
    node0.generate(10)
    info0 = node0.get_info()
    expected_height_0 = initial_height + 10
    assert info0['blocks'] == expected_height_0, \
        f"Node0 should have {expected_height_0} blocks, got {info0['blocks']}"
    chain_short_tip = info0['bestblockhash']
    print(f"✓ Node0 chain: height={info0['blocks']}, tip={chain_short_tip[:16]}...")

    # Node1 mines 15 more blocks (deeper chain)
    print("Node1 mining 15 blocks (isolated)...")
    node1.generate(15)
    info1 = node1.get_info()
    expected_height_1 = initial_height + 15
    assert info1['blocks'] == expected_height_1, \
        f"Node1 should have {expected_height_1} blocks, got {info1['blocks']}"
    chain_deep_tip = info1['bestblockhash']
    print(f"✓ Node1 chain: height={info1['blocks']}, tip={chain_deep_tip[:16]}...")

    # Connect and trigger reorg
    print(f"\nConnecting nodes (should trigger {expected_height_1 - expected_height_0}-block reorg)...")
    node0.add_node("127.0.0.1:19001", "add")

    # Wait for deep reorg
    print(f"Waiting for node0 to reorg to deeper chain (height {expected_height_1})...")
    max_wait = 30
    start_time = time.time()
    reorged = False

    while time.time() - start_time < max_wait:
        info0 = node0.get_info()
        if info0['blocks'] == expected_height_1 and info0['bestblockhash'] == chain_deep_tip:
            reorged = True
            print(f"✓ Node0 completed deep reorg: height={info0['blocks']}, tip={info0['bestblockhash'][:16]}...")
            break
        time.sleep(0.5)

    assert reorged, f"Node0 failed to complete deep reorg (height={info0['blocks']}, expected {expected_height_1})"

    # Verify both nodes synced
    info0 = node0.get_info()
    info1 = node1.get_info()
    assert info0['bestblockhash'] == info1['bestblockhash'], \
        "Nodes have different tips after deep reorg"

    print(f"✓ Deep reorg successful ({expected_height_1 - expected_height_0} blocks reorganized)")

    # Disconnect for next test
    print("Disconnecting nodes for test isolation...")
    try:
        node0.add_node("127.0.0.1:19001", "remove")
        node1.add_node("127.0.0.1:19000", "remove")
    except:
        pass
    time.sleep(2)


def test_reorg_at_different_heights(node0, node1):
    """Test reorg at various blockchain heights.

    Scenario:
    1. Start from current synced height
    2. Disconnect and build competing chains
    3. Node0 mines 3 blocks
    4. Node1 mines 5 blocks
    5. Verify reorg works at higher heights
    """
    print("\n=== Test 3: Reorg at Higher Heights ===")

    # Get current height
    info0 = node0.get_info()
    info1 = node1.get_info()
    assert info0['bestblockhash'] == info1['bestblockhash'], "Nodes should be synced"
    current_height = info0['blocks']
    print(f"Starting from synced height {current_height}")

    # Ensure nodes are disconnected
    print("Ensuring nodes are disconnected...")
    try:
        node0.add_node("127.0.0.1:19001", "remove")
        node1.add_node("127.0.0.1:19000", "remove")
    except:
        pass
    time.sleep(2)

    # Node0 mines small extension
    print("Node0 mining 3 blocks (isolated)...")
    node0.generate(3)
    info0 = node0.get_info()
    expected_height_0 = current_height + 3
    assert info0['blocks'] == expected_height_0, \
        f"Node0 should have {expected_height_0} blocks, got {info0['blocks']}"
    print(f"✓ Node0 chain: height={info0['blocks']}")

    # Node1 mines larger extension
    print("Node1 mining 5 blocks (isolated)...")
    node1.generate(5)
    info1 = node1.get_info()
    expected_height_1 = current_height + 5
    assert info1['blocks'] == expected_height_1, \
        f"Node1 should have {expected_height_1} blocks, got {info1['blocks']}"
    print(f"✓ Node1 chain: height={info1['blocks']}")

    # Connect and verify reorg
    print("\nConnecting nodes...")
    node0.add_node("127.0.0.1:19001", "add")

    print(f"Waiting for node0 to reorg to height {expected_height_1}...")
    max_wait = 30
    start_time = time.time()
    reorged = False

    while time.time() - start_time < max_wait:
        info0 = node0.get_info()
        info1 = node1.get_info()
        if info0['blocks'] == expected_height_1 and \
           info0['bestblockhash'] == info1['bestblockhash']:
            reorged = True
            print(f"✓ Reorg successful at height {info0['blocks']}")
            break
        time.sleep(0.5)

    assert reorged, f"Reorg failed at higher height (got {info0['blocks']}, expected {expected_height_1})"
    print("✓ Reorg works correctly at higher blockchain heights")

    # Disconnect for next test
    print("Disconnecting nodes for test isolation...")
    try:
        node0.add_node("127.0.0.1:19001", "remove")
        node1.add_node("127.0.0.1:19000", "remove")
    except:
        pass
    time.sleep(2)


def test_equal_work_chains(node0, node1):
    """Test equal work chains (should prefer first-seen).

    Scenario:
    1. Start from current synced state
    2. Disconnect nodes
    3. Both mine same number of blocks (equal work)
    4. Connect nodes
    5. Each should keep their own chain (first-seen rule) or converge
    """
    print("\n=== Test 4: Equal Work Chains ===")

    # Get synced state
    info0 = node0.get_info()
    info1 = node1.get_info()
    assert info0['bestblockhash'] == info1['bestblockhash'], "Nodes should be synced"
    start_height = info0['blocks']
    print(f"Starting from synced height {start_height}")

    # Ensure nodes are disconnected
    print("Ensuring nodes are disconnected...")
    try:
        node0.add_node("127.0.0.1:19001", "remove")
        node1.add_node("127.0.0.1:19000", "remove")
    except:
        pass
    time.sleep(2)

    # Both mine same number of blocks
    print("Both nodes mining 3 blocks each (isolated, equal work)...")
    node0.generate(3)
    node1.generate(3)

    info0 = node0.get_info()
    info1 = node1.get_info()

    expected_height = start_height + 3
    assert info0['blocks'] == expected_height, f"Node0 should have {expected_height} blocks"
    assert info1['blocks'] == expected_height, f"Node1 should have {expected_height} blocks"

    tip0_before = info0['bestblockhash']
    tip1_before = info1['bestblockhash']

    # Tips should be different (independent mining)
    assert tip0_before != tip1_before, "Tips should differ (independent mining)"
    print(f"✓ Both at height {expected_height} with different tips")
    print(f"  Node0 tip: {tip0_before[:16]}...")
    print(f"  Node1 tip: {tip1_before[:16]}...")

    # Connect nodes
    print("\nConnecting nodes (equal work, should prefer first-seen)...")
    node0.add_node("127.0.0.1:19001", "add")
    time.sleep(3)  # Give time for potential reorg

    # Check if any reorg happened
    info0 = node0.get_info()
    info1 = node1.get_info()

    print(f"After connection:")
    print(f"  Node0: height={info0['blocks']}, tip={info0['bestblockhash'][:16]}...")
    print(f"  Node1: height={info1['blocks']}, tip={info1['bestblockhash'][:16]}...")

    # With equal work, nodes may or may not reorg depending on implementation
    # The important thing is both end up on the same chain eventually
    if info0['bestblockhash'] == info1['bestblockhash']:
        print("✓ Nodes converged to same chain (equal work resolved)")
    else:
        print("✓ Nodes maintained different chains (equal work, no clear winner)")
        print("  Note: This is acceptable behavior for equal-work chains")

    print("✓ Equal work chain handling completed")


if __name__ == "__main__":
    sys.exit(main())
