#!/usr/bin/env python3
"""Test node management for functional tests."""

import os
import subprocess
import time
import tempfile
import shutil
import json
from pathlib import Path


class TestNode:
    """Represents a coinbasechain node for testing."""

    def __init__(self, index, datadir, binary_path, extra_args=None):
        """
        Initialize a test node.

        Args:
            index: Node index (0, 1, 2, etc.)
            datadir: Data directory for this node
            binary_path: Path to coinbasechain binary
            extra_args: Additional command-line arguments
        """
        self.index = index
        self.datadir = Path(datadir)
        self.binary_path = Path(binary_path)
        self.extra_args = extra_args or []
        self.process = None
        self.rpc_socket = self.datadir / "node.sock"

    def start(self, extra_args=None):
        """Start the node process."""
        if self.process:
            raise Exception(f"Node {self.index} already running")

        # Ensure datadir exists
        self.datadir.mkdir(parents=True, exist_ok=True)

        # Build command
        args = [
            str(self.binary_path),
            "--regtest",
            f"--datadir={self.datadir}",
        ]
        args.extend(self.extra_args)
        if extra_args:
            args.extend(extra_args)

        # Ensure verbose logging for functional tests unless explicitly overridden
        has_loglevel = any(a.startswith("--loglevel=") for a in args)
        has_debug = any(a.startswith("--debug=") for a in args)

        # Default to TRACE for better diagnostics
        if not has_loglevel:
            # Allow override via env var COINBASE_TEST_LOG_LEVEL
            loglevel = os.environ.get("COINBASE_TEST_LOG_LEVEL", "trace")
            args.append(f"--loglevel={loglevel}")

        if not has_debug:
            # Allow override via env var COINBASE_TEST_DEBUG (comma-separated)
            debug_components = os.environ.get("COINBASE_TEST_DEBUG", "network,chain")
            args.append(f"--debug={debug_components}")

        # Start process
        # Allow inheriting stdio for debugging (ASan, stack traces): set CBC_TEST_INHERIT_STDIO=1
        inherit_stdio = os.environ.get('CBC_TEST_INHERIT_STDIO', '0') == '1'
        stdout_target = None if inherit_stdio else subprocess.DEVNULL
        stderr_target = None if inherit_stdio else subprocess.DEVNULL
        self.process = subprocess.Popen(
            args,
            stdout=stdout_target,
            stderr=stderr_target,
            text=True
        )

        # Wait for RPC socket to be created
        self.wait_for_rpc_connection()

    def stop(self):
        """Stop the node process."""
        if not self.process:
            return

        self.process.terminate()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()

        self.process = None

    def cleanup(self):
        """Clean up node data directory."""
        if self.datadir.exists():
            shutil.rmtree(self.datadir)

    def wait_for_rpc_connection(self, timeout=30):
        """Wait for RPC socket to be available and accepting connections."""
        start_time = time.time()
        while time.time() - start_time < timeout:
            # Check if process crashed
            if not self.is_running():
                # Process died, read output
                stdout, stderr = self.process.communicate() if self.process else ("", "")
                log_content = self.read_log() if self.get_log_path().exists() else ""
                raise Exception(
                    f"Node {self.index} process died during startup.\n"
                    f"Stdout: {stdout}\n"
                    f"Stderr: {stderr}\n"
                    f"Log: {log_content}"
                )

            if self.rpc_socket.exists():
                # Socket exists, try to actually connect
                try:
                    self.get_info()
                    # Success! RPC is working
                    return
                except Exception as e:
                    # RPC not ready yet, wait a bit
                    time.sleep(0.5)
                    continue

            time.sleep(0.1)

        # Timeout - provide debug info
        log_content = self.read_log() if self.get_log_path().exists() else "No log file"
        raise TimeoutError(
            f"Node {self.index} RPC socket not available after {timeout}s.\n"
            f"Process running: {self.is_running()}\n"
            f"Expected socket: {self.rpc_socket}\n"
            f"Last log lines:\n{log_content}"
        )

    def is_running(self):
        """Check if node process is running."""
        if not self.process:
            return False
        return self.process.poll() is None

    def get_log_path(self):
        """Get path to debug.log."""
        return self.datadir / "debug.log"

    def read_log(self, lines=50):
        """Read last N lines from debug.log."""
        log_path = self.get_log_path()
        if not log_path.exists():
            return ""

        with open(log_path, 'r') as f:
            all_lines = f.readlines()
            return ''.join(all_lines[-lines:])

    def wait_for_log(self, pattern, timeout=10):
        """Wait for a pattern to appear in the log."""
        start_time = time.time()
        while time.time() - start_time < timeout:
            log_content = self.read_log()
            if pattern in log_content:
                return True
            time.sleep(0.5)
        return False

    def rpc(self, method, *params, timeout=30):
        """
        Call RPC method.

        Uses coinbasechain-cli to communicate via Unix socket.
        """
        cli_path = self.binary_path.parent / "coinbasechain-cli"

        args = [
            str(cli_path),
            f"--datadir={self.datadir}",
            method
        ]
        args.extend(str(p) for p in params)

        result = subprocess.run(
            args,
            capture_output=True,
            text=True,
            timeout=timeout
        )

        if result.returncode != 0:
            raise Exception(f"RPC {method} failed: {result.stderr}")

        # Try to parse JSON response
        try:
            return json.loads(result.stdout)
        except json.JSONDecodeError as e:
            # Don't silently return string - raise error with context
            raise Exception(
                f"RPC {method} returned invalid JSON:\n"
                f"Error: {e}\n"
                f"Output: {result.stdout[:500]}"  # First 500 chars for debugging
            )

    def generate(self, nblocks, address=None, timeout=120):
        """Generate blocks with configurable timeout.

        Returns dict with 'blocks' and 'height' keys.
        """
        if address is None:
            address = "0000000000000000000000000000000000000000"
        return self.rpc("generate", nblocks, address, timeout=timeout)

    def get_info(self, timeout=30):
        """Get node info with configurable timeout."""
        return self.rpc("getinfo", timeout=timeout)

    def get_peer_info(self, timeout=30):
        """Get peer connection info with configurable timeout."""
        return self.rpc("getpeerinfo", timeout=timeout)

    def add_node(self, node_addr, command="add"):
        """Add a peer node."""
        return self.rpc("addnode", node_addr, command)

    def __repr__(self):
        status = "running" if self.is_running() else "stopped"
        return f"<TestNode {self.index} ({status})>"
