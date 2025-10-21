Unicity Proof of Work 
=====================================
Unicity PoW is a streamlined proof of work blockchain implementation designed to integrate with Unicity BFT. It serves as a headers-only blockchain with RandomX proof-of-work and ASERT difficulty adjustment.There are no transactions at the this layer. Each block is only a header and contains the miners address. 

unicity-pow is released under the terms of the MIT license.

## Quick Links

- [Architecture Documentation](ARCHITECTURE.md)
- [Protocol Specification](PROTOCOL_SPECIFICATION.md)
- [Test Documentation](#testing)
- [Binary Releases](https://github.com/sakuyama2024/unicity-pow/releases)
- [Build Instructions](#building-from-source)
- [Mining with RandomX](#mining-specifications)
- [Protocol Comparison Tool](tools/compare_protocols.py)

## Community Support

If you would like to support the development or have questions:
- **Issues**: [GitHub Issues](https://github.com/unicity-network/unicity-pow/issues)
- **Documentation Index**: [DOCUMENTATION_INDEX.md](DOCUMENTATION_INDEX.md)

## Architecture

CoinbaseChain implements a simplified blockchain architecture:

1. **Headers-Only Consensus**
   - No transaction processing or UTXO management
   - 100-byte block headers (vs Bitcoin's 80 bytes)
   - Simplified validation pipeline

2. **RandomX Proof of Work**
   - ASIC-resistant memory-hard algorithm
   - Democratic mining approach

3. **ASERT Difficulty Adjustment**
   - Per-block difficulty adjustment
   - Stable 1-hour block times
   - Half-life: 48 blocks (2 days)


## Summary of Key Features

### Headers-Only Design
Block headers contain all necessary information for consensus without transactions:

```cpp
struct CBlockHeader {
    int32_t nVersion;           // 4 bytes
    uint256 hashPrevBlock;      // 32 bytes
    uint160 minerAddress;       // 20 bytes (replaces merkleRoot)
    uint32_t nTime;            // 4 bytes
    uint32_t nBits;            // 4 bytes
    uint32_t nNonce;           // 4 bytes
    uint256 hashRandomX;       // 32 bytes (RandomX output)
};  // Total: 100 bytes
```

### Mining Specifications
- **Hash Function**: RandomX (ASIC-resistant)
- **Block Time**: 1 hour  (5x faster than Bitcoin)
- **Block Reward**: 142,857  UNCT 
- **Difficulty Adjustment**: ASERT (Absolutely Scheduled Exponentially Rising Targets)
  - Adjusts every block

### Genesis Block Details
- **Hash**: `0x598ac9fdac5831ba791763504c3183ba9c30a57573c544ee6c112dd22143659a`
- **Timestamp**: 1609459200 (November 10, 2025 00:00:00 UTC)
- **nBits**: 0x1e0fffff (starting difficulty)
- **Miner Address**: `0x598ac9fdac5831ba791763504c3183ba9c30a57573c544ee6c112dd22143659a`

### Network Parameters
- **Magic Bytes**: 0x554E4943 ("UNIC")
- **Default Port**: 9590 (P2P)
- **RPC Interface**: Unix socket (`datadir/node.sock`)
- **Protocol Version**: 1
- **Max Connections**: 125 inbound, 8 outbound


## The Major Changes from Bitcoin

**No Transaction Support**

Headers-only design eliminates all transaction processing:
```cpp
// No tx messages supported
// No mempool
// No UTXO set
```

**RandomX Hash Function**

To democratize mining we use the RandomX ASIC resistance hash function as used in Monero:
- Memory-hard algorithm t
- Prevents ASIC dominance
- CPU-friendly mining

**100-byte Block Headers**

Extended from Bitcoin's 80 bytes to include:
- Miner address (20 bytes) instead of merkle root
- RandomX hash (32 bytes) for PoW commitment

**1-Hour Block Time**

Chain grows at less than 1MB per year

**ASERT Difficulty Adjustment**

The Bitcoin Cash implementation of an exponential moving average approach:
- Adjusts every block (not every 2016 blocks)
- Always targets 120-second block time
- Smooth difficulty transitions

**Simplified P2P Protocol**

While maintaining 98% compatibility with Bitcoin's network layer:
- No block relay (headers only)
- No transaction relay
- No mempool synchronization
- Focused on header propagation

## Building from Source

### Prerequisites
- C++20 compatible compiler
- CMake 3.16+
- Boost 1.70+

### Build Instructions
```bash
git clone https://github.com/sakuyama2024/unicity-pow.git
cd unicity-pow
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Running Tests
```bash
./unicity-pow_tests
```

## Docker

### Quick Start with Docker

The easiest way to run a Unicity PoW node is using Docker:

```bash
# Pull the latest image
docker pull unicitynetwork/unicity-pow:latest

# Run a node with default settings
docker run -d --name unicity-pow \
  -p 9590:9590 \
  unicitynetwork/unicity-pow

# Run with persistent data volume
docker run -d --name unicity-pow \
  -p 9590:9590 \
  -v unicity-data:/data \
  unicitynetwork/unicity-pow
```

### Building Docker Image

```bash
# Build from source
docker build -t unicity-pow .

# Multi-stage build for smaller image
docker build -f Dockerfile.alpine -t unicity-pow:alpine .
```

### Docker Compose

For production deployments, use docker-compose:

```yaml
# docker-compose.yml
version: '3.8'
services:
  unicity-pow:
    image: unicitynetwork/unicity-pow:latest
    container_name: unicity-pow
    ports:
      - "9590:9590"  # P2P port
    volumes:
      - unicity-data:/data
      - ./unicity.conf:/data/unicity.conf:ro
    restart: unless-stopped
    environment:
      - DEBUG=net
      - DATADIR=/data
    command: ["-printtoconsole", "-datadir=/data"]

volumes:
  unicity-data:
```

Start with:
```bash
docker-compose up -d
```

### Container Management

```bash
# View logs
docker logs -f unicity-pow

# Execute RPC commands
docker exec unicity-pow unicity-pow-cli getblockcount

# Stop the node gracefully
docker stop unicity-pow

# Remove container (data persists in volume)
docker rm unicity-pow

# Clean up everything including data
docker-compose down -v
```

### Resource Requirements

Docker resource recommendations:
- **Memory**: 3GB minimum (2GB for RandomX + overhead)
- **CPU**: 2+ cores recommended
- **Disk**: 1GB for blockchain data + logs
- **Network**: Allow port 9590 (P2P)

```bash
# Run with resource limits
docker run -d --name unicity-pow \
  --memory="3g" \
  --cpus="2" \
  -p 9590:9590 \
  unicitynetwork/unicity-pow
```

## Running a Node

```bash
# Start with default settings
./bin/unicity-pow

# Specify data directory
./bin/unicity-pow -datadir=/path/to/data

# Enable debug output
./bin/unicity-pow -debug=all
```

## RPC Interface

### Important Note on RPC Design

**This implementation uses Unix domain sockets instead of TCP/IP networking for RPC.** This is a deliberate security design choice:

- **Local-only access**: RPC commands can only be executed on the same machine running the node
- **No network exposure**: Unlike Bitcoin's JSON-RPC over HTTP (port 8332), there is no network port
- **Enhanced security**: Eliminates entire classes of remote attacks and unauthorized access
- **Simpler authentication**: File system permissions control access instead of username/password

The RPC server creates a Unix socket at `datadir/node.sock` that the CLI tool connects to locally. This means:
- No `rpcport`, `rpcbind`, or `rpcallowip` configuration options
- No remote RPC access (must SSH to the server to run commands)
- No RPC credentials needed (relies on file system permissions)

If you need remote monitoring, you should:
1. SSH to the server and run `unicity-pow-cli` commands locally
2. Set up a monitoring agent that runs locally and exports metrics
3. Use a reverse proxy if you absolutely need remote access (not recommended)

### Basic RPC Commands
```bash
# CLI automatically connects to the local socket
./bin/unicity-pow-cli getblockcount
./bin/unicity-pow-cli getbestblockhash
./bin/unicity-pow-cli getblockheader <hash>
./bin/unicity-pow-cli getpeerinfo

# Specify custom datadir if needed
./bin/unicity-pow-cli -datadir=/custom/path getblockcount
```

Bitcoin Core Foundation
=====================================

This project builds upon the foundational work of Bitcoin Core, adapting its robust networking and consensus mechanisms for a headers-only blockchain implementation.

License
-------

unicity-pow is released under the terms of the MIT license. See [LICENSE](LICENSE) for more information or see https://opensource.org/licenses/MIT.

Development Process
-------------------

The `master` branch is regularly built and tested, but it is not guaranteed to be completely stable. Tagged releases are created for stable versions.

The contribution workflow follows standard GitHub practices. Please submit pull requests with comprehensive test coverage.

Testing
-------

Testing is critical for blockchain systems. All changes must include appropriate tests. The test suite includes 357 test cases with 4,806 assertions covering all critical components.

### Test Categories

#### Unit Tests
Core functionality tests for individual components:
```bash
./unicity-pow_tests "[unit]"
```
- **Block Structure Tests**: Header serialization, hashing, validation
- **Chain Management**: Block index, chain selection, reorganization
- **Time Validation**: Median time past, future time limits
- **Difficulty Adjustment**: ASERT algorithm, target calculation
- **Protocol Messages**: Serialization/deserialization of all message types
- **Ban Management**: Peer scoring, automatic banning, DoS protection

#### Integration Tests
End-to-end system testing:
```bash
./unicity-pow_tests "[integration]"
```
- **Network Handshake**: VERSION/VERACK exchange
- **Header Synchronization**: Initial block download, header propagation
- **Chain Synchronization**: Multi-peer sync, fork resolution
- **Peer Management**: Connection lifecycle, peer discovery
- **Orphan Handling**: Orphan header management, memory limits

#### Network Tests
P2P protocol and networking:
```bash
./unicity-pow_tests "[network]"
```
- **Message Routing**: Command dispatch, invalid message handling
- **Connection Types**: Inbound/outbound, manual connections
- **NAT Traversal**: UPnP support, port mapping
- **Address Management**: Peer discovery, address propagation
- **Transport Layer**: Socket handling, disconnection scenarios

#### Security Tests
Attack resistance and DoS protection:
```bash
./unicity-pow_tests "[security]"
```
- **DoS Attack Simulations**: Header spam, connection exhaustion
- **Ban System**: Misbehavior detection, score accumulation
- **Resource Limits**: Orphan limits, connection limits
- **Time Attack Prevention**: MTP validation, future time rejection
- **Fork Attack Resistance**: Deep reorg protection, work validation

#### Performance Tests
Benchmarking and stress testing:
```bash
./unicity-pow_tests "[performance]"
```
- **Header Validation Speed**: ~50ms full validation, ~1ms pre-check
- **Chain Update Performance**: Tip updates, reorg performance
- **Memory Usage**: RandomX dataset management
- **Concurrent Operations**: Thread safety, lock contention
- **Network Throughput**: Header propagation speed

#### RandomX Tests
Proof-of-work specific testing:
```bash
./unicity-pow_tests "[randomx]"
```
- **Hash Verification**: Correct RandomX implementation
- **Memory Management**: Dataset initialization, cache handling
- **Thread Safety**: Concurrent mining operations
- **Commitment Validation**: PoW commitment in headers

### Running Tests

```bash
# Run all tests
./unicity-pow_tests

# Run with detailed output
./unicity-pow_tests -v

# Run specific test category
./unicity-pow_tests "[network]"

# Run with timing information
./unicity-pow_tests --durations yes

# Run specific test case
./unicity-pow_tests "MessageRouter"

# List all available tests
./unicity-pow_tests --list-tests

# Run tests with JUnit output (for CI)
./unicity-pow_tests -r junit -o test-results.xml
```

### Test Coverage Summary

| Component | Test Cases | Assertions | Coverage |
|-----------|------------|------------|----------|
| Chain Management | 45 | 612 | 92% |
| Network Protocol | 89 | 1,245 | 98% |
| Consensus Rules | 38 | 487 | 91% |
| RandomX PoW | 24 | 156 | 88% |
| Peer Management | 67 | 893 | 94% |
| Security/DoS | 31 | 412 | 95% |
| Integration | 63 | 1,001 | 89% |
| **Total** | **357** | **4,806** | **93%** |

### Continuous Integration

The project uses automated testing on every commit:
- **Build Matrix**: Linux, macOS
- **Compiler Matrix**: GCC 10+, Clang 11+, 
- **Test Execution**: All test categories run automatically
- **Coverage Reports**: Generated for each build
- **Performance Regression**: Tracked across commits

### Manual Testing

Changes should be tested by someone other than the developer. For complex changes, include a test plan in the pull request description.

#### Test Plan Template
```markdown
## Test Plan
- [ ] Unit tests pass
- [ ] Integration tests pass
- [ ] Manual sync from genesis
- [ ] Peer connection testing
- [ ] Resource usage monitoring
- [ ] Attack simulation (if security-related)
```

---
