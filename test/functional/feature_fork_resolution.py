#!/usr/bin/env python3
# Fork resolution test - Multiple nodes with different chain lengths should converge to longest

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
    # Configuration
    BASE_PORT = 18444
    test_data = Path(__file__).parent.parent / "data"

    # Check test data exists
    if not test_data.exists():
        log("ERROR: Test data not found. Run generate_test_chains.py first!", RED)
        return 1

    test_dir = Path(tempfile.mkdtemp(prefix='cbc_fork_'))
    binary_path = Path(__file__).parent.parent.parent / "build" / "bin" / "coinbasechain"
    nodes = []

    try:
        log("\n" + "="*70, BLUE)
        log("FORK RESOLUTION TEST", BLUE)
        log("="*70, BLUE)
        log("Scenario:", YELLOW)
        log("  - Node0: 5 blocks (shortest)", YELLOW)
        log("  - Node1: 10 blocks (medium)", YELLOW)
        log("  - Node2: 15 blocks (longest)", YELLOW)
        log("Expected: All nodes converge to 15 blocks\n", YELLOW)

        # Create Node0 with 5-block chain
        log("Setting up Node0 with 5-block chain...", BLUE)
        node0_dir = test_dir / 'node0'
        shutil.copytree(test_data / 'chain_5', node0_dir)
        node0 = TestNode(0, node0_dir, binary_path,
                        extra_args=["--listen", f"--port={BASE_PORT}"])
        nodes.append(node0)
        node0.start()

        info = node0.get_info()
        log(f"✓ Node0 started: height={info['blocks']}, hash={info['bestblockhash'][:16]}...", GREEN)

        # Create Node1 with 10-block chain
        log("Setting up Node1 with 10-block chain...", BLUE)
        node1_dir = test_dir / 'node1'
        shutil.copytree(test_data / 'chain_10', node1_dir)
        node1 = TestNode(1, node1_dir, binary_path,
                        extra_args=["--listen", f"--port={BASE_PORT + 1}"])
        nodes.append(node1)
        node1.start()

        info = node1.get_info()
        log(f"✓ Node1 started: height={info['blocks']}, hash={info['bestblockhash'][:16]}...", GREEN)

        # Create Node2 with 15-block chain (longest)
        log("Setting up Node2 with 15-block chain (longest)...", BLUE)
        node2_dir = test_dir / 'node2'
        shutil.copytree(test_data / 'chain_15', node2_dir)
        node2 = TestNode(2, node2_dir, binary_path,
                        extra_args=["--listen", f"--port={BASE_PORT + 2}"])
        nodes.append(node2)
        node2.start()

        info = node2.get_info()
        expected_hash = info['bestblockhash']
        log(f"✓ Node2 started: height={info['blocks']}, hash={expected_hash[:16]}...", GREEN)
        log(f"\nExpected final hash: {expected_hash}\n", YELLOW)

        # Connect Node1 to Node0
        log("Connecting Node1 → Node0...", BLUE)
        node1.add_node(f"127.0.0.1:{BASE_PORT}", "add")
        time.sleep(1)

        # Connect Node2 to Node0 and Node1
        log("Connecting Node2 → Node0...", BLUE)
        node2.add_node(f"127.0.0.1:{BASE_PORT}", "add")
        time.sleep(1)

        log("Connecting Node2 → Node1...", BLUE)
        node2.add_node(f"127.0.0.1:{BASE_PORT + 1}", "add")
        time.sleep(1)

        log("✓ All nodes connected\n", GREEN)

        # Monitor convergence
        log("Monitoring convergence (max 60 seconds)...", BLUE)
        max_wait = 60
        check_interval = 2
        converged = [False, False, False]

        for elapsed in range(0, max_wait, check_interval):
            time.sleep(check_interval)

            # Check each node
            for i, node in enumerate(nodes):
                if converged[i]:
                    continue

                if not node.is_running():
                    log(f"\n✗ CRASH: Node{i} crashed!", RED)
                    raise RuntimeError(f"Node{i} crashed!")

                try:
                    info = node.get_info()
                    height = info['blocks']
                    bhash = info['bestblockhash']

                    if height == 15 and bhash == expected_hash:
                        converged[i] = True
                        log(f"  {elapsed+check_interval}s: Node{i} converged to height 15 ✓", GREEN)
                    else:
                        log(f"  {elapsed+check_interval}s: Node{i} height={height}/15", BLUE)

                except Exception as e:
                    log(f"  Node{i}: RPC error: {e}", RED)

            # Check if all converged
            if all(converged):
                log(f"\n✓ All nodes converged in {elapsed+check_interval} seconds!", GREEN)
                break

        print()

        # Final verification
        log("Final verification:", BLUE)
        log("-" * 70, BLUE)

        all_synced = True
        for i, node in enumerate(nodes):
            info = node.get_info()
            height = info['blocks']
            bhash = info['bestblockhash']
            match = "✓" if (height == 15 and bhash == expected_hash) else "✗"

            log(f"Node{i}: height={height}, hash={bhash[:16]}... {match}",
                GREEN if match == "✓" else RED)

            if height != 15 or bhash != expected_hash:
                all_synced = False

        print()

        if all_synced:
            log("="*70, GREEN)
            log("FORK RESOLUTION TEST PASSED ✓", GREEN)
            log("="*70, GREEN)
            log("All nodes successfully converged to the longest chain (15 blocks)", GREEN)
            return 0
        else:
            log("="*70, RED)
            log("FORK RESOLUTION TEST FAILED ✗", RED)
            log("="*70, RED)
            log("Not all nodes converged to the longest chain", RED)
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
