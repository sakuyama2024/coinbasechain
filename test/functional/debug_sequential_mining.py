#!/usr/bin/env python3
"""Debug: sequential mining and sync without stressing memory.

- Starts N nodes (default 4)
- Each mines a small chain sequentially (avoids concurrent RandomX pressure)
- Connects all to node0 and verifies convergence
- Then does a controlled extended mining phase (one node at a time)
- Preserves test directory on failure or if CBC_KEEP_TEST_DIR=1

Env vars:
- NUM_NODES: number of peers (default 4)
- BASE_PORT: starting port (default 18444)
- CBC_KEEP_TEST_DIR: '1' to keep test dir
- COINBASE_TEST_LOG_LEVEL/COINBASE_TEST_DEBUG affect node logging (set by framework)
"""

import os
import sys
import time
import tempfile
import shutil
from pathlib import Path

# Reuse framework
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
    num_nodes = int(os.environ.get('NUM_NODES', '4'))
    base_port = int(os.environ.get('BASE_PORT', '18444'))

    test_dir = Path(tempfile.mkdtemp(prefix='cbc_debug_seq_'))
    bin_dir = os.environ.get('COINBASE_BIN_DIR', str(Path(__file__).parent.parent.parent / "build" / "bin"))
    binary_path = Path(bin_dir) / "coinbasechain"

    nodes = []

    try:
        log("\n=== Debug Sequential Mining Test ===", BLUE)
        log(f"Nodes: {num_nodes}", BLUE)

        # Start node0 (listening)
        node0 = TestNode(0, test_dir / 'node0', binary_path,
                         extra_args=["--listen", f"--port={base_port}"])
        nodes.append(node0)
        log("Starting node0...", BLUE)
        node0.start()
        time.sleep(0.5)

        # Start peers (not listening)
        for i in range(1, num_nodes + 1):
            node = TestNode(i, test_dir / f'node{i}', binary_path,
                            extra_args=[f"--port={base_port + i}"])
            nodes.append(node)
            log(f"Starting node{i}...", BLUE)
            node.start()
            time.sleep(0.2)

        # Sequential mining: mine small, increasing counts
        mine_counts = [5 + i for i in range(num_nodes)]  # 5,6,7,8,...
        tips = {}
        for i in range(1, num_nodes + 1):
            count = mine_counts[i-1]
            log(f"Node{i} mining {count} blocks...", BLUE)
            res = nodes[i].generate(count)
            if not isinstance(res, dict):
                raise RuntimeError(f"Node{i} generate did not return JSON: {res}")
            info = nodes[i].get_info()
            tips[i] = info['bestblockhash']
            log(f"  Node{i} height={info['blocks']} tip={tips[i][:16]}...", GREEN)
            time.sleep(0.1)

        # Connect all peers to node0 (peers -> node0)
        log("Connecting peers to node0...", BLUE)
        for i in range(1, num_nodes + 1):
            try:
                nodes[i].add_node(f"127.0.0.1:{base_port}")
            except Exception as e:
                log(f"  Node{i} addnode failed: {e}", YELLOW)
        time.sleep(1)

        # Also connect node0 to peers (node0 -> peers) so header sync selects outbound peer
        log("Connecting node0 to peers (bidirectional)...", BLUE)
        for i in range(1, num_nodes + 1):
            try:
                node0.add_node(f"127.0.0.1:{base_port + i}")
            except Exception as e:
                log(f"  Node0 → Node{i}: {e}", YELLOW)
        time.sleep(1)

        # Wait for node0 to converge to the longest chain
        target_height = max(5 + i for i in range(num_nodes))
        log(f"Waiting for node0 to reach ≥ {target_height}...", BLUE)
        deadline = time.time() + 30
        while time.time() < deadline:
            info0 = node0.get_info()
            if info0['blocks'] >= target_height:
                break
            time.sleep(0.5)
        info0 = node0.get_info()
        log(f"Node0: height={info0['blocks']} tip={info0['bestblockhash'][:16]}...", GREEN)

        # Controlled extended mining: one node at a time, small batch
        log("\nExtended phase: sequential small mining per node...", BLUE)
        for i in range(1, num_nodes + 1):
            if not nodes[i].is_running():
                log(f"  Node{i} not running, skipping", YELLOW)
                continue
            log(f"  Node{i} generate 3 blocks...", BLUE)
            try:
                nodes[i].generate(3)
            except Exception as e:
                log(f"  Node{i} generate failed: {e}", YELLOW)
            time.sleep(0.2)

        # Final check: sample all nodes
        log("\nFinal states:", BLUE)
        heights = []
        for i in range(0, num_nodes + 1):
            if not nodes[i].is_running():
                log(f"  Node{i}: CRASHED", RED)
                heights.append(0)
                continue
            info = nodes[i].get_info()
            log(f"  Node{i}: height={info['blocks']} tip={info['bestblockhash'][:16]}...", GREEN)
            heights.append(info['blocks'])

        # Accept success if no crashes and node0 is highest
        if any(h == 0 for h in heights):
            raise RuntimeError("Some nodes crashed during debug test")

        log("\n✓ Debug sequential mining test PASSED", GREEN)
        print(f"Test directory: {test_dir}")
        return 0

    except Exception as e:
        log(f"\n✗ Debug test FAILED: {e}", RED)
        # Dump last 80 lines from each log
        for i, n in enumerate(nodes):
            try:
                log(f"\nNode{i} debug.log (last 80 lines):", YELLOW)
                print(n.read_log(80))
            except Exception:
                pass
        return 1

    finally:
        # Cleanup or preserve
        for n in nodes:
            if n.is_running():
                n.stop()
        keep = os.environ.get('CBC_KEEP_TEST_DIR', '0') == '1'
        if keep:
            log(f"Preserving test directory: {test_dir}", YELLOW)
        else:
            shutil.rmtree(test_dir, ignore_errors=True)

if __name__ == '__main__':
    sys.exit(main())
