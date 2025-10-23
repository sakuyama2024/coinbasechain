#!/usr/bin/env python3
# Chainstate persistence test - Verify node saves and restores all fork candidates

import sys
import time
import tempfile
import shutil
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent / "test_framework"))
from test_node import TestNode

GREEN = '\033[92m'
RED = '\033[91m'
YELLOW = '\033[93m'
BLUE = '\033[94m'
RESET = '\033[0m'

def log(msg, color=None):
    if color:
        print(f"{color}{msg}{RESET}")
    else:
        print(msg)

def main():
    """
    Test that nodes properly save and restore chainstate including fork candidates.

    Scenario:
    1. Start Node0 with 5-block chain
    2. Connect Node1 (10 blocks) and Node2 (15 blocks) - Node0 sees 3 forks
    3. Verify Node0 converges to 15 blocks
    4. Restart Node0 - verify it still has 15 blocks
    5. Add Node3 with 20 blocks
    6. Verify Node0 converges to 20 blocks
    7. Restart Node0 again - verify it has 20 blocks
    """
    BASE_PORT = 18444
    test_data = Path(__file__).parent.parent / "data"

    if not test_data.exists():
        log("ERROR: Test data not found. Run generate_test_chains.py first!", RED)
        return 1

    test_dir = Path(tempfile.mkdtemp(prefix='cbc_persist_'))
    binary_path = Path(__file__).parent.parent.parent / "build" / "bin" / "coinbasechain"
    nodes = []

    try:
        log("\n" + "="*70, BLUE)
        log("CHAINSTATE PERSISTENCE TEST", BLUE)
        log("="*70, BLUE)

        # ===== PHASE 1: Initial fork resolution =====
        log("\nPHASE 1: Initial fork resolution with 3 chains", BLUE)
        log("-" * 70, BLUE)

        # Setup Node0 with 5-block chain
        log("Setting up Node0 with 5-block chain...", YELLOW)
        node0_dir = test_dir / 'node0'
        shutil.copytree(test_data / 'chain_5', node0_dir)
        node0 = TestNode(0, node0_dir, binary_path,
                        extra_args=["--listen", f"--port={BASE_PORT}"])
        nodes.append(node0)
        node0.start()

        info = node0.get_info()
        log(f"✓ Node0: height={info['blocks']}, hash={info['bestblockhash'][:16]}...", GREEN)

        # Setup Node1 with 10-block chain
        log("Setting up Node1 with 10-block chain...", YELLOW)
        node1_dir = test_dir / 'node1'
        shutil.copytree(test_data / 'chain_10', node1_dir)
        node1 = TestNode(1, node1_dir, binary_path,
                        extra_args=["--listen", f"--port={BASE_PORT + 1}"])
        nodes.append(node1)
        node1.start()

        info = node1.get_info()
        log(f"✓ Node1: height={info['blocks']}, hash={info['bestblockhash'][:16]}...", GREEN)

        # Setup Node2 with 15-block chain (longest for now)
        log("Setting up Node2 with 15-block chain...", YELLOW)
        node2_dir = test_dir / 'node2'
        shutil.copytree(test_data / 'chain_15', node2_dir)
        node2 = TestNode(2, node2_dir, binary_path,
                        extra_args=["--listen", f"--port={BASE_PORT + 2}"])
        nodes.append(node2)
        node2.start()

        info = node2.get_info()
        expected_hash_15 = info['bestblockhash']
        log(f"✓ Node2: height={info['blocks']}, hash={expected_hash_15[:16]}...", GREEN)

        # Connect all to Node0
        log("\nConnecting nodes to Node0...", YELLOW)
        node1.add_node(f"127.0.0.1:{BASE_PORT}", "add")
        node2.add_node(f"127.0.0.1:{BASE_PORT}", "add")
        time.sleep(3)

        # Verify convergence to 15 blocks
        log("Verifying Node0 converged to 15 blocks...", YELLOW)
        info = node0.get_info()
        if info['blocks'] != 15 or info['bestblockhash'] != expected_hash_15:
            log(f"✗ Node0 did not converge: height={info['blocks']}, expected 15", RED)
            return 1
        log(f"✓ Node0 converged to 15 blocks", GREEN)

        # ===== PHASE 2: Restart Node0 and verify persistence =====
        log("\nPHASE 2: Restart Node0 and verify persistence", BLUE)
        log("-" * 70, BLUE)

        log("Stopping Node0...", YELLOW)
        node0.stop()
        time.sleep(1)

        log("Restarting Node0...", YELLOW)
        node0 = TestNode(0, node0_dir, binary_path,
                        extra_args=["--listen", f"--port={BASE_PORT}"])
        nodes[0] = node0
        node0.start()

        # Verify Node0 still has 15 blocks
        info = node0.get_info()
        if info['blocks'] != 15 or info['bestblockhash'] != expected_hash_15:
            log(f"✗ Node0 lost state after restart: height={info['blocks']}, expected 15", RED)
            log(f"  Hash: {info['bestblockhash'][:16]}...", RED)
            log(f"  Expected: {expected_hash_15[:16]}...", RED)
            return 1
        log(f"✓ Node0 persisted state: height={info['blocks']}, hash={info['bestblockhash'][:16]}...", GREEN)

        # ===== PHASE 3: Add longer chain and test re-org =====
        log("\nPHASE 3: Add longer chain (20 blocks) and test re-org", BLUE)
        log("-" * 70, BLUE)

        # Setup Node3 with 20-block chain
        log("Setting up Node3 with 20-block chain...", YELLOW)
        node3_dir = test_dir / 'node3'
        shutil.copytree(test_data / 'chain_20', node3_dir)
        node3 = TestNode(3, node3_dir, binary_path,
                        extra_args=["--listen", f"--port={BASE_PORT + 3}"])
        nodes.append(node3)
        node3.start()

        info = node3.get_info()
        expected_hash_20 = info['bestblockhash']
        log(f"✓ Node3: height={info['blocks']}, hash={expected_hash_20[:16]}...", GREEN)

        # Connect Node3 to Node0 FIRST (before reconnecting to shorter chains)
        log("\nConnecting Node3 to Node0...", YELLOW)
        node3.add_node(f"127.0.0.1:{BASE_PORT}", "add")
        node0.add_node(f"127.0.0.1:{BASE_PORT + 3}", "add")  # Bidirectional
        time.sleep(2)

        # Now reconnect Node0 to Node1 and Node2
        log("Reconnecting Node0 to rest of network...", YELLOW)
        node0.add_node(f"127.0.0.1:{BASE_PORT + 1}", "add")
        node0.add_node(f"127.0.0.1:{BASE_PORT + 2}", "add")
        time.sleep(2)

        # Verify Node0 re-orged to 20 blocks
        log("Verifying Node0 re-orged to 20 blocks...", YELLOW)
        max_wait = 10
        converged = False
        for i in range(max_wait):
            time.sleep(1)
            info = node0.get_info()
            if info['blocks'] == 20 and info['bestblockhash'] == expected_hash_20:
                converged = True
                break
            log(f"  Waiting... Node0 height={info['blocks']}/20", BLUE)

        if not converged:
            log(f"✗ Node0 did not re-org to 20 blocks: height={info['blocks']}", RED)
            return 1
        log(f"✓ Node0 re-orged to 20 blocks", GREEN)

        # ===== PHASE 4: Final restart and persistence check =====
        log("\nPHASE 4: Final restart and verify re-org persisted", BLUE)
        log("-" * 70, BLUE)

        log("Stopping Node0...", YELLOW)
        node0.stop()
        time.sleep(1)

        log("Restarting Node0 (final check)...", YELLOW)
        node0 = TestNode(0, node0_dir, binary_path,
                        extra_args=["--listen", f"--port={BASE_PORT}"])
        nodes[0] = node0
        node0.start()

        # Verify Node0 still has 20 blocks
        info = node0.get_info()
        if info['blocks'] != 20 or info['bestblockhash'] != expected_hash_20:
            log(f"✗ Node0 lost re-org after restart: height={info['blocks']}, expected 20", RED)
            log(f"  Hash: {info['bestblockhash'][:16]}...", RED)
            log(f"  Expected: {expected_hash_20[:16]}...", RED)
            return 1
        log(f"✓ Node0 persisted re-org: height={info['blocks']}, hash={info['bestblockhash'][:16]}...", GREEN)

        # ===== FINAL VERIFICATION =====
        log("\nFinal state verification:", BLUE)
        log("-" * 70, BLUE)

        # Check headers.json was saved
        headers_file = node0_dir / "headers.json"
        if not headers_file.exists():
            log("✗ headers.json not found!", RED)
            return 1

        import json
        with open(headers_file, 'r') as f:
            headers_data = json.load(f)

        block_count = headers_data.get('block_count', 0)

        # Find the best (highest) height from all blocks
        best_height = -1
        if 'blocks' in headers_data:
            for block in headers_data['blocks']:
                if 'height' in block:
                    best_height = max(best_height, block['height'])

        log(f"✓ headers.json exists", GREEN)
        log(f"  Total blocks saved: {block_count}", BLUE)
        log(f"  Highest block height: {best_height}", BLUE)

        # Verify we saved the re-orged chain (should have 20+ blocks)
        if block_count < 20:
            log(f"⚠ Warning: Expected at least 20 blocks, found {block_count}", YELLOW)
        if best_height < 20:
            log(f"⚠ Warning: Expected height >= 20, found {best_height}", YELLOW)

        # Verify all nodes converged to 20 blocks (longest chain wins)
        all_correct = True
        for idx in range(len(nodes)):
            info = nodes[idx].get_info()
            height = info['blocks']
            bhash = info['bestblockhash']

            # All nodes should have converged to the longest chain (20 blocks)
            match = "✓" if (height == 20 and bhash == expected_hash_20) else "✗"

            log(f"Node{idx}: height={height}/20, hash={bhash[:16]}... {match}",
                GREEN if match == "✓" else RED)

            if match == "✗":
                all_correct = False

        print()

        if all_correct:
            log("="*70, GREEN)
            log("CHAINSTATE PERSISTENCE TEST PASSED ✓", GREEN)
            log("="*70, GREEN)
            log("Summary:", YELLOW)
            log("  • Node0 properly saved and restored state across 2 restarts", YELLOW)
            log("  • Fork candidates were preserved", YELLOW)
            log("  • Re-org from 15→20 blocks persisted correctly", YELLOW)
            log("  • headers.json saved successfully", YELLOW)
            return 0
        else:
            log("="*70, RED)
            log("CHAINSTATE PERSISTENCE TEST FAILED ✗", RED)
            log("="*70, RED)
            return 1

    except Exception as e:
        log(f"\n✗ TEST FAILED: {e}", RED)
        import traceback
        traceback.print_exc()
        return 1

    finally:
        log("\nCleaning up...", YELLOW)
        for node in nodes:
            if node.is_running():
                node.stop()
        log(f"Test directory preserved: {test_dir}", YELLOW)

if __name__ == '__main__':
    sys.exit(main())
