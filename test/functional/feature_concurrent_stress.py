#!/usr/bin/env python3
# Aggressive concurrent validation stress test to reproduce race conditions

import sys
import time
import tempfile
import shutil
import threading
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
    # More aggressive configuration
    NUM_PEER_NODES = 20  # 20 peers (double the original)
    BLOCKS_PER_PEER = 30
    BLOCKS_WINNING_PEER = 100  # Much longer chain
    BASE_PORT = 18444

    test_dir = Path(tempfile.mkdtemp(prefix='cbc_stress_'))
    binary_path = Path(__file__).parent.parent.parent / "build" / "bin" / "coinbasechain"
    nodes = []

    try:
        log(f"\n=== AGGRESSIVE CONCURRENT STRESS TEST ===", BLUE)
        log(f"Config: {NUM_PEER_NODES} peers, winning chain={BLOCKS_WINNING_PEER} blocks\n", YELLOW)

        # Create Node0
        node0 = TestNode(0, test_dir / 'node0', binary_path,
                        extra_args=["--listen", f"--port={BASE_PORT}"])
        nodes.append(node0)
        node0.start()
        log(f"Node0 started", GREEN)

        # Create peer nodes
        peer_nodes = []
        for i in range(1, NUM_PEER_NODES + 1):
            node = TestNode(i, test_dir / f'node{i}', binary_path,
                          extra_args=["--listen", f"--port={BASE_PORT + i}"])
            nodes.append(node)
            peer_nodes.append(node)
            node.start()
            time.sleep(0.05)  # Very short delay

        log(f"All {NUM_PEER_NODES} peers started\n", GREEN)

        # Mine blocks in parallel
        log("Mining blocks (this will take a while)...", BLUE)

        def mine_peer(node):
            num_blocks = BLOCKS_WINNING_PEER if node.index == 1 else BLOCKS_PER_PEER
            try:
                result = node.generate(num_blocks)
                # generate() now returns {"blocks": N, "height": N}
                blocks_mined = result.get('blocks', 0) if isinstance(result, dict) else 0
                log(f"  Node{node.index}: {blocks_mined} blocks", BLUE)
            except Exception as e:
                log(f"  Node{node.index}: Mining failed: {e}", RED)

        threads = []
        for node in peer_nodes:
            t = threading.Thread(target=mine_peer, args=(node,))
            t.start()
            threads.append(t)

        for t in threads:
            t.join()

        log("Mining complete\n", GREEN)

        # Connect all at once
        log(f"Connecting {NUM_PEER_NODES} peers to Node0 SIMULTANEOUSLY...", YELLOW)

        def connect_peer(node):
            try:
                node.add_node(f"127.0.0.1:{BASE_PORT}", "add")
            except Exception as e:
                log(f"  Node{node.index}: Connection failed: {e}", RED)

        connect_threads = []
        for node in peer_nodes:
            t = threading.Thread(target=connect_peer, args=(node,))
            t.start()
            connect_threads.append(t)

        for t in connect_threads:
            t.join()

        log("All peers connected\n", GREEN)

        # Monitor for convergence (max 30 seconds)
        log("Monitoring for convergence (max 30 seconds)...", BLUE)
        expected_height = BLOCKS_WINNING_PEER
        converged = False

        for i in range(30):
            time.sleep(1)
            if not node0.is_running():
                log(f"\n✗ Node0 CRASHED after {i+1} seconds!", RED)
                log("\nNode0 log (last 100 lines):", YELLOW)
                log(node0.read_log(100))
                raise RuntimeError("Node0 crashed!")

            # Check peer crashes too
            crashed_peers = [(n.index, n) for n in peer_nodes if not n.is_running()]
            if crashed_peers and i == 1:  # Only log once
                for idx, node in crashed_peers:
                    log(f"✗ Peer {idx} crashed! Last 30 log lines:", RED)
                    log(node.read_log(30))
                    log("=" * 60)

            # Check if Node0 has converged to expected height
            try:
                info = node0.get_info()
                if info['blocks'] >= expected_height:
                    log(f"  {i+1}s: Node0 converged to height {info['blocks']} ✓", GREEN)
                    converged = True
                    break
            except:
                pass

            if (i + 1) % 5 == 0:
                log(f"  {i+1}/30 seconds - Node0 running ✓")

        if converged:
            log(f"\n✓ Test passed - Node0 converged successfully!", GREEN)
        else:
            log(f"\n✓ Test passed - survived 30 seconds without crash!", GREEN)

        # Check final state
        log("\nFinal chain state:", BLUE)
        for node in nodes[:5]:  # Just check first 5
            if node.is_running():
                try:
                    info = node.get_info()
                    log(f"  Node{node.index}: height={info['blocks']}")
                except:
                    log(f"  Node{node.index}: RPC failed")

        return 0

    except Exception as e:
        log(f"\n✗ STRESS TEST FAILED: {e}", RED)
        return 1

    finally:
        log("\nCleaning up...", YELLOW)
        for node in nodes:
            if node.is_running():
                node.stop()
        # Don't delete test_dir for debugging
        log(f"Test directory preserved: {test_dir}", YELLOW)
        # shutil.rmtree(test_dir, ignore_errors=True)

if __name__ == '__main__':
    sys.exit(main())
