# Functional Testing - What We Actually Test and How

## Testing Architecture

### How It Works

The functional tests are **end-to-end black-box tests** that:

1. **Spawn REAL node processes** - Not mocks, actual `./bin/coinbasechain` binaries
2. **Communicate via RPC** - Use `./bin/coinbasechain-cli` to send commands
3. **Verify behavior** - Check that nodes behave correctly in real scenarios

```
Test Script (Python)
    ↓
  Spawns multiple coinbasechain processes
    ↓
  Sends RPC commands (generate, addnode, getinfo, etc.)
    ↓
  Verifies results (block heights, consensus, peers, etc.)
```

### Key Difference from Unit Tests

| Aspect | Unit Tests | Functional Tests |
|--------|-----------|------------------|
| What's tested | Individual C++ functions/classes | Entire system end-to-end |
| How | Link against test code | Spawn real binaries |
| Speed | Fast (milliseconds) | Slower (seconds) |
| Scope | Internal implementation | External behavior |
| Language | C++ (Catch2) | Python |

## What We Test

### 1. **basic_mining.py** - Single Node Mining

**What it tests:**
- Can a node start successfully?
- Can it mine blocks when requested?
- Does block height increase correctly?

**How:**
```python
node.start()                    # Start coinbasechain process
info = node.get_info()         # RPC: getinfo
assert info['blocks'] == 0     # At genesis

node.generate(10)              # RPC: generate 10
info = node.get_info()
assert info['blocks'] >= 10    # Mined blocks
```

**Verifies:**
- Process startup
- RPC server works
- Miner responds to commands
- State is tracked correctly

---

### 2. **p2p_connect.py** - Basic P2P Connection

**What it tests:**
- Can two nodes connect to each other?
- Do blocks propagate between connected nodes?
- Do nodes sync to the same chain tip?

**How:**
```python
# Start two nodes
node0.start(extra_args=["--listen", "--port=19000"])
node1.start(extra_args=["--port=19001"])

# Connect node1 to node0
node1.add_node("127.0.0.1:19000", "add")   # RPC: addnode

# Mine blocks on node0
node0.generate(5)

# Verify node1 received the blocks
assert node1.get_info()['blocks'] == 5
assert node0.get_info()['bestblockhash'] == node1.get_info()['bestblockhash']
```

**Verifies:**
- P2P networking stack works
- VERSION/VERACK handshake succeeds
- Block relay (INV/HEADERS messages)
- Block propagation
- Consensus between peers

---

### 3. **p2p_three_nodes.py** - Multi-Hop Block Relay

**What it tests:**
- Can blocks propagate through multiple hops?
- Does node1 relay blocks from node0 to node2?

**Setup:**
```
node0 (miner) → node1 (relay) → node2 (receiver)
```

**How:**
```python
# Build chain topology
node0.add_node("127.0.0.1:19001")  # node0 → node1
node1.add_node("127.0.0.1:19002")  # node1 → node2

# Mine on node0
node0.generate(10)

# Verify all nodes synced
assert node0['bestblockhash'] == node1['bestblockhash'] == node2['bestblockhash']
```

**Verifies:**
- Multi-peer networking
- Block relay works transitively
- No blocks lost in multi-hop relay
- All nodes reach consensus

---

### 4. **p2p_ibd.py** - Initial Block Download

**What it tests:**
- Can a fresh node download the entire chain from a peer?
- Does IBD (Initial Block Download) work correctly?

**How:**
```python
# Phase 1: Build chain on node0
node0.start()
node0.generate(50)  # Create 50-block chain

# Phase 2: Start fresh node1 (at genesis)
node1.start()
assert node1.get_info()['blocks'] == 0

# Phase 3: Connect to node0 (triggers IBD)
node1.add_node("127.0.0.1:19000")
time.sleep(5)  # Wait for sync

# Phase 4: Verify sync
assert node1.get_info()['blocks'] >= 50
assert node0['bestblockhash'] == node1['bestblockhash']
```

**Verifies:**
- GETHEADERS message works
- HEADERS batch download works
- Block validation during sync
- Node catches up from genesis
- Final consensus

---

## How RPC Communication Works

The test framework communicates with nodes using the CLI:

```python
def rpc(self, method, *params):
    """Call RPC via coinbasechain-cli"""
    cli_path = self.binary_path.parent / "coinbasechain-cli"
    
    args = [
        str(cli_path),
        f"--datadir={self.datadir}",
        method
    ] + params
    
    result = subprocess.run(args, capture_output=True)
    return json.loads(result.stdout)
```

Example:
```python
node.get_info()
# Executes: ./bin/coinbasechain-cli --datadir=/tmp/node0 getinfo
# Returns: {"blocks": 10, "bestblockhash": "abc123...", ...}

node.generate(5)
# Executes: ./bin/coinbasechain-cli --datadir=/tmp/node0 generate 5
# Returns: ["hash1", "hash2", "hash3", "hash4", "hash5"]
```

---

## What Makes Tests Pass or Fail?

### ✅ Tests PASS when:
- All RPC commands succeed
- Block heights match expectations (>= N blocks)
- **All nodes have the same bestblockhash** (consensus!)
- Connections establish successfully
- Blocks propagate within timeout

### ❌ Tests FAIL when:
- Node crashes during startup
- RPC commands timeout or error
- Blocks don't propagate
- **Nodes have different tips** (consensus failure!)
- Connections fail to establish

---

## Test Coverage

Our functional tests verify:

### ✅ Covered
1. **Process Management** - Start/stop nodes
2. **Mining** - Block generation works
3. **P2P Networking** - Connections, handshake
4. **Block Propagation** - INV/HEADERS relay
5. **Multi-hop Relay** - Blocks pass through intermediaries
6. **IBD** - Fresh nodes sync from genesis
7. **Consensus** - All nodes agree on chain tip

### ⚠️ Not Covered (Yet)
- Reorgs (covered in other tests like p2p_reorg.py)
- DOS protection (p2p_dos_headers.py)
- Peer eviction (p2p_eviction.py)
- Concurrent mining and conflicts
- Network partitions and recovery

---

## The "Regtest Race Condition"

### Why We Allow Extra Blocks

In regtest mode, mining is **instant** (no real POW). This causes:

```
Test requests: generate(10)
Miner finds:   Block 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 ✓
Test issues:   Stop mining!
Miner finds:   Block 11 (too fast!) ← Race condition
Result:        11 blocks instead of 10
```

**Solution:** Use `>=` instead of `==`
```python
assert info['blocks'] >= 10  # Allow 10, 11, or 12
```

**Why this is OK:**
- Only happens in regtest (instant mining)
- Production has real POW (no race)
- What matters: blocks propagate and nodes sync
- Exact count is less important than **consensus**

---

## Real-World Testing Value

These tests verify **the actual system works**, not just individual components:

- ✅ Binary compiles and runs
- ✅ RPC server accepts connections
- ✅ Network stack establishes peers
- ✅ P2P protocol messages work
- ✅ Blocks propagate across network
- ✅ **Nodes reach consensus** (most important!)

This catches integration issues that unit tests miss:
- Configuration problems
- Protocol bugs
- Timing issues
- Multi-node race conditions
- Real network behavior

---

## Summary

**What:** End-to-end tests of real node processes
**How:** Spawn binaries, send RPC commands, verify results
**Why:** Ensure the complete system works, not just individual parts
**Coverage:** P2P networking, mining, sync, consensus

The functional tests are your **integration test suite** - they prove the blockchain actually works as a distributed system, not just as isolated components.
