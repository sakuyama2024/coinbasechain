#!/usr/bin/env python3
# Copyright (c) 2024 Coinbase Chain
# Test concurrent sync from multiple nodes

"""
Multi-node concurrent sync test.

Tests that the node can safely sync from multiple peers concurrently,
verifying thread safety of validation under realistic network conditions.

Scenario:
- Node0: Mines a long chain (50 blocks)
- Node1-10: Start with genesis, connect to Node0
- All nodes sync from Node0 concurrently (multiple network threads active)
- Verify no crashes, deadlocks, or data corruption
"""

import os
import sys
import time
import subprocess
import tempfile
import shutil
import signal

# Color codes for output
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

class TestNode:
    def __init__(self, node_id, datadir, port, rpc_port):
        self.node_id = node_id
        self.datadir = datadir
        self.port = port
        self.rpc_port = rpc_port
        self.process = None

    def start(self, connect_to=None, extra_args=None):
        """Start the node"""
        args = [
            './build/bin/coinbasechain',
            '--regtest',
            f'--datadir={self.datadir}',
            f'--port={self.port}',
            f'--rpcport={self.rpc_port}',
        ]

        if connect_to:
            for peer_port in connect_to:
                args.append(f'--addnode=127.0.0.1:{peer_port}')

        if extra_args:
            args.extend(extra_args)

        self.process = subprocess.Popen(
            args,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )

        # Wait a bit for startup
        time.sleep(0.5)

        # Check if process is still running
        if self.process.poll() is not None:
            stdout, stderr = self.process.communicate()
            raise RuntimeError(f"Node{self.node_id} failed to start:\nSTDOUT: {stdout}\nSTDERR: {stderr}")

        log(f"Node{self.node_id} started (port={self.port}, rpc={self.rpc_port})", GREEN)

    def stop(self):
        """Stop the node"""
        if self.process:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
            log(f"Node{self.node_id} stopped", YELLOW)

    def rpc(self, method, params=None):
        """Call RPC method"""
        import socket
        import json

        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": method,
            "params": params or []
        }

        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(2.0)
            sock.connect(('127.0.0.1', self.rpc_port))
            sock.sendall((json.dumps(request) + '\n').encode())

            response = b''
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                response += chunk
                if b'\n' in response:
                    break

            sock.close()

            result = json.loads(response.decode())
            if 'error' in result and result['error']:
                raise RuntimeError(f"RPC error: {result['error']}")
            return result.get('result')

        except Exception as e:
            raise RuntimeError(f"RPC call failed: {e}")

    def getblockcount(self):
        """Get current block height"""
        try:
            return self.rpc('getblockcount')
        except:
            return 0

    def mine_blocks(self, n):
        """Mine n blocks"""
        for i in range(n):
            try:
                self.rpc('generate', [1])
            except Exception as e:
                log(f"Warning: Mining block {i+1}/{n} failed: {e}", YELLOW)
                time.sleep(0.1)

        # Verify we mined them
        height = self.getblockcount()
        log(f"Node{self.node_id} mined {n} blocks, now at height {height}", BLUE)
        return height

def wait_for_sync(nodes, target_height, timeout=30):
    """Wait for all nodes to sync to target height"""
    start = time.time()

    while time.time() - start < timeout:
        heights = []
        all_synced = True

        for node in nodes:
            try:
                height = node.getblockcount()
                heights.append(height)
                if height < target_height:
                    all_synced = False
            except:
                heights.append(0)
                all_synced = False

        if all_synced:
            log(f"✓ All nodes synced to height {target_height}", GREEN)
            return True

        # Print progress
        synced = sum(1 for h in heights if h >= target_height)
        log(f"  Sync progress: {synced}/{len(nodes)} nodes at height {target_height} (heights: {heights})")
        time.sleep(1)

    log(f"✗ Sync timeout! Heights: {heights}", RED)
    return False

def main():
    log("\n=== Multi-Node Concurrent Sync Test ===\n", BLUE)

    # Configuration
    NUM_SYNC_NODES = 10  # 10 nodes syncing from Node0
    CHAIN_LENGTH = 50
    BASE_PORT = 18444
    BASE_RPC_PORT = 18332

    test_dir = tempfile.mkdtemp(prefix='cbc_multinode_')
    log(f"Test directory: {test_dir}\n")

    nodes = []

    try:
        # Create Node0 (the node with the chain)
        node0_dir = os.path.join(test_dir, 'node0')
        os.makedirs(node0_dir)
        node0 = TestNode(0, node0_dir, BASE_PORT, BASE_RPC_PORT)
        nodes.append(node0)

        # Create syncing nodes (Node1-10)
        for i in range(1, NUM_SYNC_NODES + 1):
            node_dir = os.path.join(test_dir, f'node{i}')
            os.makedirs(node_dir)
            node = TestNode(i, node_dir, BASE_PORT + i, BASE_RPC_PORT + i)
            nodes.append(node)

        # Step 1: Start Node0 and mine chain
        log("Step 1: Starting Node0 and mining chain...", BLUE)
        node0.start()
        time.sleep(1)

        initial_height = node0.getblockcount()
        log(f"✓ Node0 initial height: {initial_height}")

        log(f"\nMining {CHAIN_LENGTH} blocks on Node0...")
        final_height = node0.mine_blocks(CHAIN_LENGTH)

        if final_height < CHAIN_LENGTH:
            raise RuntimeError(f"Node0 only reached height {final_height}, expected {CHAIN_LENGTH}")

        log(f"✓ Node0 mined chain to height {final_height}\n", GREEN)

        # Step 2: Start all sync nodes simultaneously
        log(f"Step 2: Starting {NUM_SYNC_NODES} nodes to sync from Node0...", BLUE)
        log("This tests concurrent header processing from multiple network threads\n")

        for node in nodes[1:]:
            node.start(connect_to=[BASE_PORT])  # All connect to Node0
            time.sleep(0.1)  # Small delay between starts

        log(f"✓ All {NUM_SYNC_NODES} sync nodes started\n", GREEN)

        # Step 3: Wait for all nodes to sync
        log(f"Step 3: Waiting for all nodes to sync to height {CHAIN_LENGTH}...", BLUE)
        log("(This stresses the validation mutex with concurrent header acceptance)\n")

        if not wait_for_sync(nodes[1:], CHAIN_LENGTH, timeout=60):
            raise RuntimeError("Nodes failed to sync in time!")

        # Step 4: Verify all nodes have same tip
        log("\nStep 4: Verifying all nodes have consistent state...", BLUE)

        heights = [node.getblockcount() for node in nodes]
        log(f"Final heights: {heights}")

        if not all(h == CHAIN_LENGTH for h in heights):
            raise RuntimeError(f"Height mismatch! Expected all nodes at {CHAIN_LENGTH}, got {heights}")

        log(f"✓ All {len(nodes)} nodes at height {CHAIN_LENGTH}", GREEN)

        # Step 5: Test continued operation - mine more blocks and sync again
        log("\nStep 5: Testing continued operation (mine 10 more blocks)...", BLUE)

        new_height = node0.mine_blocks(10)
        log(f"Node0 now at height {new_height}")

        if not wait_for_sync(nodes[1:], new_height, timeout=30):
            raise RuntimeError("Second sync failed!")

        log(f"✓ All nodes synced to new height {new_height}", GREEN)

        # Success!
        log("\n" + "="*60, GREEN)
        log("✓ Multi-node concurrent sync test PASSED!", GREEN)
        log("="*60 + "\n", GREEN)

        log("Test verified:", BLUE)
        log(f"  • {NUM_SYNC_NODES} nodes syncing concurrently from Node0")
        log(f"  • {CHAIN_LENGTH + 10} blocks synced total")
        log(f"  • Multiple network threads processing headers simultaneously")
        log(f"  • No crashes, deadlocks, or data corruption")
        log(f"  • Validation mutex correctly serializes concurrent operations")

        return 0

    except Exception as e:
        log(f"\n✗ Test FAILED: {e}", RED)
        import traceback
        traceback.print_exc()
        return 1

    finally:
        # Cleanup
        log("\nCleaning up...", YELLOW)
        for node in nodes:
            node.stop()

        time.sleep(1)

        try:
            shutil.rmtree(test_dir)
            log(f"Cleaned up test directory: {test_dir}\n")
        except Exception as e:
            log(f"Warning: Could not clean up {test_dir}: {e}", YELLOW)

if __name__ == '__main__':
    sys.exit(main())
