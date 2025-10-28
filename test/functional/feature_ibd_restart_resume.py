#!/usr/bin/env python3
# IBD resume after restart: ensure mid-sync restart resumes cleanly

import sys
import time
import tempfile
import shutil
import json
from pathlib import Path

# Add test framework to path
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

def wait_for_height(node: TestNode, target: int, timeout: int = 30) -> bool:
    start = time.time()
    while time.time() - start < timeout:
        try:
            info = node.get_info()
            if info.get('blocks', -1) >= target:
                return True
        except Exception:
            pass
        time.sleep(0.2)
    return False

def main():
    log("\n=== IBD Resume After Restart Test ===\n", BLUE)

    BASE_PORT = 18444
    CHAIN_LEN = 120  # long enough to catch mid-sync, short enough to mine fast

    test_dir = Path(tempfile.mkdtemp(prefix='cbc_ibd_resume_'))
    binary_path = Path(__file__).parent.parent.parent / "build" / "bin" / "coinbasechain"

    nodes = []
    try:
        # Seed node with pre-mined chain
        nodeA_dir = test_dir / 'nodeA'
        nodeA = TestNode(0, nodeA_dir, binary_path, extra_args=["--listen", f"--port={BASE_PORT}"])
        nodes.append(nodeA)
        nodeA.start()

        log(f"Mining {CHAIN_LEN} blocks on NodeA...", BLUE)
        res = nodeA.generate(CHAIN_LEN)
        if not isinstance(res, dict) or res.get('height', 0) < CHAIN_LEN:
            raise RuntimeError(f"Failed to mine chain on NodeA: {res}")
        tipA = nodeA.get_info()['bestblockhash']
        log(f"✓ NodeA tip: {tipA[:16]}... height={res['height']}", GREEN)

        # Syncing node (slow down IO threads to increase chance of mid-sync stop)
        nodeB_dir = test_dir / 'nodeB'
        nodeB = TestNode(1, nodeB_dir, binary_path, extra_args=[f"--port={BASE_PORT+1}", "--threads=1", "--debug=network"])
        nodes.append(nodeB)
        nodeB.start()

        # Connect B -> A and begin IBD
        log("Connecting NodeB to NodeA and starting IBD...", BLUE)
        nodeB.add_node(f"127.0.0.1:{BASE_PORT}", "add")

        # Wait for partial progress then stop mid-sync (using log signal)
        mid_height = None
        # Wait until we see a 'synchronizing block headers' log entry
        if nodeB.wait_for_log('synchronizing block headers', timeout=10):
            # Parse the latest height from the log tail
            import re
            log_tail = nodeB.read_log(100)
            matches = re.findall(r"synchronizing block headers, height: (\d+)", log_tail)
            if matches:
                mid_height = int(matches[-1])
        else:
            # Fallback: poll height briefly
            start = time.time()
            while time.time() - start < 5:
                infoB = nodeB.get_info()
                h = infoB.get('blocks', 0)
                if h >= 1 and h < CHAIN_LEN:
                    mid_height = h
                    break
                time.sleep(0.05)

        if mid_height is None or mid_height >= CHAIN_LEN:
            # As a last resort, extend chain and try again quickly
            log("Did not catch mid-sync via logs; extending chain by 50 and retrying quick sample...", YELLOW)
            nodeA.generate(50)
            target = nodeA.get_info()['blocks']
            start = time.time()
            while time.time() - start < 5:
                infoB = nodeB.get_info()
                h = infoB.get('blocks', 0)
                if h >= 1 and h < target:
                    mid_height = h
                    break
                time.sleep(0.05)

        if mid_height is None:
            raise RuntimeError("Failed to observe mid-sync progress on NodeB")

        log(f"Stopping NodeB mid-sync at height {mid_height}...", YELLOW)
        nodeB.stop()
        time.sleep(1)

        # Verify headers.json saved with progress
        headers_path = nodeB_dir / 'headers.json'
        if not headers_path.exists():
            raise RuntimeError("headers.json not found after stop")
        with open(headers_path, 'r') as f:
            headers_data = json.load(f)
        saved_blocks = headers_data.get('block_count', 0)
        highest_saved = -1
        for blk in headers_data.get('blocks', []):
            if isinstance(blk, dict) and 'height' in blk:
                highest_saved = max(highest_saved, blk['height'])
        log(f"Saved headers: count={saved_blocks}, highest={highest_saved}")
        if highest_saved < mid_height:
            raise RuntimeError(f"Saved highest height {highest_saved} < observed {mid_height}")

        # Restart NodeB and confirm it resumes from saved height (not reset)
        log("Restarting NodeB...", BLUE)
        nodeB = TestNode(1, nodeB_dir, binary_path, extra_args=[f"--port={BASE_PORT+1}", "--threads=1"])
        nodes[1] = nodeB
        nodeB.start()

        info_after_restart = nodeB.get_info()
        h_after = info_after_restart.get('blocks', 0)
        log(f"NodeB after restart height={h_after} (was {mid_height})")
        if h_after < mid_height:
            raise RuntimeError("NodeB did not restore saved height after restart")

        # Reconnect and finish sync
        nodeB.add_node(f"127.0.0.1:{BASE_PORT}", "add")
        target_height = nodeA.get_info()['blocks']
        log(f"Waiting for NodeB to reach target height {target_height}...", BLUE)
        if not wait_for_height(nodeB, target_height, timeout=60):
            raise RuntimeError("NodeB failed to complete sync after restart")

        infoA = nodeA.get_info()
        infoB = nodeB.get_info()
        if infoA['bestblockhash'] != infoB['bestblockhash']:
            raise RuntimeError("Best block hash mismatch after sync")

        print()
        log("="*60, GREEN)
        log("✓ IBD resume after restart test PASSED", GREEN)
        log("="*60, GREEN)
        return 0

    except Exception as e:
        log(f"\n✗ Test FAILED: {e}", RED)
        import traceback
        traceback.print_exc()
        return 1

    finally:
        log("\nCleaning up...", YELLOW)
        for node in nodes:
            try:
                if node.is_running():
                    node.stop()
            except Exception:
                pass
        log(f"Test directory preserved: {test_dir}", YELLOW)

if __name__ == '__main__':
    sys.exit(main())
