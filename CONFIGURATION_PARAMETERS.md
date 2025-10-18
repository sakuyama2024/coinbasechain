# Configuration Parameters Reference

**Last Updated**: 2025-10-17
**Version**: 1.0

This document provides a comprehensive reference of all configurable parameters in the CoinbaseChain implementation, extracted from the codebase.

---

## Table of Contents

1. [Protocol Constants](#protocol-constants)
2. [Network Configuration](#network-configuration)
3. [Peer Management](#peer-management)
4. [Consensus Parameters](#consensus-parameters)
5. [Proof of Work (PoW)](#proof-of-work-pow)
6. [Synchronization](#synchronization)
7. [Security & DoS Protection](#security--dos-protection)
8. [Address Management](#address-management)
9. [Timing & Intervals](#timing--intervals)
10. [Resource Limits](#resource-limits)
11. [Chain-Specific Parameters](#chain-specific-parameters)

---

## Protocol Constants

### Protocol Version
**Location**: `include/network/protocol.hpp:12-13`

```cpp
constexpr uint32_t PROTOCOL_VERSION = 70016;       // Bitcoin Core 0.16.0+ compatible
constexpr uint32_t MIN_PEER_PROTO_VERSION = 70001; // Minimum peer protocol version
```

**Description**: Protocol version for compatibility checking with other nodes.

**Recommendations**:
- `PROTOCOL_VERSION`: Should match the Bitcoin protocol version you're targeting
- `MIN_PEER_PROTO_VERSION`: Minimum version to accept connections from

---

### Network Magic Bytes
**Location**: `include/network/protocol.hpp:17-21`

```cpp
namespace magic {
    constexpr uint32_t MAINNET = 0xC0C0C0C0;   // Custom mainnet magic
    constexpr uint32_t TESTNET = 0xC0C0C0C1;   // Custom testnet magic
    constexpr uint32_t REGTEST = 0xC0C0C0C2;   // Custom regtest magic
}
```

**Description**: Network identifiers that prevent cross-network message relay.

**Recommendations**:
- Use unique values for mainnet (avoid Bitcoin's 0xF9BEB4D9)
- Testnet and regtest should differ from mainnet
- All nodes on same network must use identical magic bytes

---

### Default Ports
**Location**: `include/network/protocol.hpp:24-28`

```cpp
namespace ports {
    constexpr uint16_t MAINNET = 8333;
    constexpr uint16_t TESTNET = 18333;
    constexpr uint16_t REGTEST = 18444;
}
```

**Description**: Default TCP ports for P2P networking.

**Recommendations**:
- Mainnet: Use unique port (8333 conflicts with Bitcoin)
- Testnet: Typically mainnet_port + 10000
- Regtest: For local testing, arbitrary high port
- **Security**: Ensure firewall rules match your port choice

---

### Service Flags
**Location**: `include/network/protocol.hpp:32-35`

```cpp
enum ServiceFlags : uint64_t {
    NODE_NONE = 0,
    NODE_NETWORK = (1 << 0),  // Can serve block headers
};
```

**Description**: Advertised capabilities of a node.

**Current Implementation**: Headers-only chain, only `NODE_NETWORK` used.

---

### Message Types
**Location**: `include/network/protocol.hpp:39-59`

```cpp
namespace commands {
    // Handshake
    constexpr const char* VERSION = "version";
    constexpr const char* VERACK = "verack";

    // Peer discovery
    constexpr const char* ADDR = "addr";
    constexpr const char* GETADDR = "getaddr";

    // Block announcements and requests
    constexpr const char* INV = "inv";
    constexpr const char* GETDATA = "getdata";
    constexpr const char* NOTFOUND = "notfound";
    constexpr const char* GETHEADERS = "getheaders";
    constexpr const char* HEADERS = "headers";
    constexpr const char* SENDHEADERS = "sendheaders";

    // Keep-alive
    constexpr const char* PING = "ping";
    constexpr const char* PONG = "pong";
}
```

**Description**: Protocol message command names (12 bytes, null-padded).

---

### Inventory Types
**Location**: `include/network/protocol.hpp:63-66`

```cpp
enum class InventoryType : uint32_t {
    ERROR = 0,
    MSG_BLOCK = 2,  // Used for block hash announcements
};
```

**Description**: Types of inventory items announced via INV messages.

---

## Network Configuration

### NetworkManager::Config
**Location**: `include/network/network_manager.hpp:40-61`

```cpp
struct Config {
    uint32_t network_magic;              // Network magic bytes
    uint16_t listen_port;                // Port to listen on (0 = don't listen)
    bool listen_enabled;                 // Enable inbound connections
    size_t io_threads;                   // Number of IO threads
    int par_threads;                     // Number of parallel RandomX verification threads
    std::string datadir;                 // Data directory (for banlist.json)

    std::chrono::seconds connect_interval;      // Time between connection attempts
    std::chrono::seconds maintenance_interval;  // Time between maintenance tasks

    // Defaults:
    Config()
        : network_magic(protocol::magic::MAINNET)
        , listen_port(protocol::ports::MAINNET)
        , listen_enabled(false)
        , io_threads(4)
        , par_threads(0)                 // Auto-detect
        , datadir("")                    // Empty = no persistent bans
        , connect_interval(std::chrono::seconds(5))
        , maintenance_interval(std::chrono::seconds(30))
    {}
};
```

#### Parameters:

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `network_magic` | uint32_t | `0xC0C0C0C0` | Network identifier |
| `listen_port` | uint16_t | `8333` | TCP port for inbound connections |
| `listen_enabled` | bool | `false` | Accept inbound connections |
| `io_threads` | size_t | `4` | Number of async I/O threads |
| `par_threads` | int | `0` | RandomX verification threads (0=auto) |
| `datadir` | string | `""` | Data directory path |
| `connect_interval` | seconds | `5` | Time between outbound connection attempts |
| `maintenance_interval` | seconds | `30` | Time between periodic cleanup tasks |

**Tuning Recommendations**:

- **`io_threads`**:
  - Default: 4
  - Recommended: 2-8 for typical deployments
  - High-end servers: 8-16
  - Low-end devices: 1-2

- **`par_threads`**:
  - Default: 0 (auto-detect CPU cores)
  - Set manually if you want to limit CPU usage
  - Recommended: Leave at 0 for auto-detection

- **`connect_interval`**:
  - Default: 5 seconds
  - Faster: 2-3 seconds (more aggressive peering)
  - Slower: 10-30 seconds (conservative)

- **`maintenance_interval`**:
  - Default: 30 seconds
  - Faster: 10-15 seconds (more frequent cleanup)
  - Slower: 60-120 seconds (less overhead)

---

## Peer Management

### PeerManager::Config
**Location**: `include/network/peer_manager.hpp:75-81`

```cpp
struct Config {
    size_t max_outbound_peers;     // Max outbound connections
    size_t max_inbound_peers;      // Max inbound connections
    size_t target_outbound_peers;  // Try to maintain this many outbound

    Config()
        : max_outbound_peers(8)
        , max_inbound_peers(125)
        , target_outbound_peers(8)
    {}
};
```

#### Parameters:

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `max_outbound_peers` | size_t | `8` | Maximum outbound connections |
| `max_inbound_peers` | size_t | `125` | Maximum inbound connections |
| `target_outbound_peers` | size_t | `8` | Target number of outbound peers |

**Tuning Recommendations**:

- **`max_outbound_peers`**:
  - Default: 8 (matches Bitcoin Core)
  - Low bandwidth: 4-6
  - High bandwidth: 10-15
  - **Note**: More outbound = more secure but more bandwidth

- **`max_inbound_peers`**:
  - Default: 125 (matches Bitcoin Core)
  - Non-listening nodes: 0
  - Low bandwidth: 10-25
  - High bandwidth servers: 200-500
  - **Note**: Inbound peers help network but consume resources

- **`target_outbound_peers`**:
  - Default: 8
  - Should be ≤ `max_outbound_peers`
  - Network will try to maintain this many connections

**Connection Budget**:
```
Total connections = outbound + inbound
Recommended: max 133 total (8 out + 125 in)
Memory usage: ~1-2 MB per peer (for buffers, state)
Bandwidth: ~100 KB/s per active peer during sync
```

---

### Peer Eviction Protection
**Location**: `src/network/peer_manager.cpp:221-227`

```cpp
// Protection rules:
// 1. Never evict outbound peers
// 2. Protect recently connected peers (last 10 seconds)
// 3. Prefer evicting peers with worst ping times

auto connection_age = std::chrono::duration_cast<std::chrono::seconds>(
    now - peer->stats().connected_time
);
if (connection_age.count() < 10) {
    continue;  // Protect recently connected
}
```

**Parameters**:
- **Recently connected protection**: 10 seconds
  - **Tuning**: Increase to 30-60s for more stability
  - **Security tradeoff**: Longer protection makes eclipse attacks easier

---

## Consensus Parameters

### ConsensusParams
**Location**: `include/chain/chainparams.hpp:34-62`

```cpp
struct ConsensusParams {
    // Proof of Work
    uint256 powLimit;                    // Maximum difficulty (easiest target)
    int64_t nPowTargetSpacing{120};      // 2 minutes between blocks

    int64_t nRandomXEpochDuration{7 * 24 * 60 * 60};  // 1 week (604800 seconds)

    // ASERT difficulty adjustment
    int64_t nASERTHalfLife{2 * 24 * 60 * 60};  // 2 days (172800 seconds)

    // ASERT anchor block height
    int32_t nASERTAnchorHeight{1};

    // Hash of genesis block
    uint256 hashGenesisBlock;

    // Minimum cumulative chain work for IBD completion
    uint256 nMinimumChainWork;
};
```

#### Parameters:

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `powLimit` | uint256 | Network-specific | Maximum target (minimum difficulty) |
| `nPowTargetSpacing` | int64_t | `120` seconds | Target time between blocks (2 minutes) |
| `nRandomXEpochDuration` | int64_t | `604800` seconds | RandomX key change interval (1 week) |
| `nASERTHalfLife` | int64_t | `172800` seconds | ASERT difficulty adjustment half-life (2 days) |
| `nASERTAnchorHeight` | int32_t | `1` | Height of anchor block for ASERT |
| `hashGenesisBlock` | uint256 | Network-specific | Genesis block hash |
| `nMinimumChainWork` | uint256 | Network-specific | Minimum PoW for IBD completion |

**Block Time (nPowTargetSpacing)**:
- **Current**: 120 seconds (2 minutes)
- **Tradeoffs**:
  - Faster (60s): Higher orphan rate, less security per block
  - Slower (300s): Lower throughput, longer confirmation times
- **Recommendation**: Keep at 120s unless specific requirements

**RandomX Epoch Duration (nRandomXEpochDuration)**:
- **Current**: 1 week (604800 seconds)
- **Purpose**: Balance between:
  - Short epochs: More frequent key changes (harder to optimize ASICs)
  - Long epochs: Better VM caching, less overhead
- **Tradeoffs**:
  - 1 day: More ASIC-resistant but higher overhead
  - 1 month: Less overhead but easier to build specialized hardware
- **Recommendation**: 1 week is good balance

**ASERT Half-Life (nASERTHalfLife)**:
- **Current**: 2 days (172800 seconds)
- **Purpose**: Difficulty doubles/halves after half-life ahead/behind schedule
- **Calculation**:
  ```
  Half-life = 2 days = 48 hours = 2880 minutes = 1440 blocks (at 2min/block)
  ```
- **Tradeoffs**:
  - Shorter (1 day): Faster difficulty adjustment, more responsive
  - Longer (7 days): Slower adjustment, more stable
- **Recommendation**: 2 days matches Bitcoin Cash experience

---

## Proof of Work (PoW)

### RandomX Configuration
**Location**: `include/crypto/randomx_pow.hpp:62-65`

```cpp
/** Faster RandomX computation but requires more memory */
static constexpr bool DEFAULT_RANDOMX_FAST_MODE = false;

/** Number of epochs to cache. Minimum is 1. */
static constexpr int DEFAULT_RANDOMX_VM_CACHE_SIZE = 2;
```

#### Parameters:

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `DEFAULT_RANDOMX_FAST_MODE` | bool | `false` | Use fast mode (more memory, faster) |
| `DEFAULT_RANDOMX_VM_CACHE_SIZE` | int | `2` | Number of epoch VMs to cache |

**RandomX Modes**:

1. **Light Mode** (Default: `false` = enabled):
   - Memory: ~256 MB per VM
   - Speed: ~100-300 hash/s (CPU-dependent)
   - Use: Default, IBD, low-memory systems

2. **Fast Mode** (`true`):
   - Memory: ~2-3 GB per VM
   - Speed: ~1000-3000 hash/s (CPU-dependent)
   - Use: Mining, high-performance servers
   - **Warning**: Not recommended for nodes with < 4GB RAM

**VM Cache Size**:
- **Default**: 2 epochs cached
- **Why 2**:
  - Cache current epoch + previous epoch
  - Handles blocks near epoch boundary
  - Prevents re-initialization thrashing
- **Memory usage**:
  - Light mode: 2 × 256 MB = 512 MB
  - Fast mode: 2 × 2.5 GB = 5 GB
- **Recommendations**:
  - Low memory (< 2 GB RAM): Set to 1
  - Normal (2-4 GB RAM): Keep at 2
  - High memory (> 8 GB RAM): Can increase to 3-4

---

## Synchronization

### Header Sync Timeout
**Location**: `src/network/network_manager.cpp:355`

```cpp
constexpr int64_t SYNC_TIMEOUT_SECONDS = 60;
```

**Description**: Maximum time without receiving headers before resetting sync peer.

**Tuning**:
- **Default**: 60 seconds
- **Slower networks**: 90-120 seconds
- **Faster networks**: 30-45 seconds
- **Tradeoff**: Longer timeout = more patient, shorter = faster peer rotation

---

### Tip Announcement Interval
**Location**: `src/network/network_manager.cpp:962`

```cpp
constexpr int64_t ANNOUNCE_INTERVAL_SECONDS = 30;
```

**Description**: How often to re-announce chain tip to peers (matches Bitcoin Core).

**Tuning**:
- **Default**: 30 seconds
- **More frequent**: 15-20 seconds (higher bandwidth)
- **Less frequent**: 60-90 seconds (lower bandwidth)
- **Purpose**: Ensure peers know our chain state even if tip hasn't changed

---

### Anchor Connections
**Location**: `src/network/network_manager.cpp:726`

```cpp
const size_t MAX_ANCHORS = 2;
```

**Description**: Number of reliable outbound peers to persist for reconnection.

**Purpose**: Eclipse attack resistance (Bitcoin Core behavior).

**Tuning**:
- **Default**: 2 (matches Bitcoin Core)
- **More resilient**: 3-4 (but diminishing returns)
- **Recommendation**: Keep at 2

---

### Reorg Detection
**Location**: `src/validation/chainstate_manager.cpp:20`

```cpp
static constexpr int SUSPICIOUS_REORG_DEPTH = 100;
```

**Description**: Halt if reorganization exceeds this depth (potential attack).

**Tuning**:
- **Default**: 100 blocks
- **More paranoid**: 50 blocks
- **Less paranoid**: 200 blocks
- **At 2min/block**: 100 blocks = 3.3 hours
- **Purpose**: Detect eclipse/long-range attacks

---

## Security & DoS Protection

### Message Size Limits
**Location**: `include/network/protocol.hpp:74-77`

```cpp
constexpr uint32_t MAX_MESSAGE_SIZE = 32 * 1024 * 1024;  // 32 MB
constexpr uint32_t MAX_INV_SIZE = 50000;
constexpr uint32_t MAX_HEADERS_SIZE = 2000;
constexpr uint32_t MAX_ADDR_SIZE = 1000;
```

#### Parameters:

| Parameter | Value | Description |
|-----------|-------|-------------|
| `MAX_MESSAGE_SIZE` | 32 MB | Maximum size of any P2P message |
| `MAX_INV_SIZE` | 50,000 | Maximum inventory items per INV message |
| `MAX_HEADERS_SIZE` | 2,000 | Maximum headers per HEADERS message |
| `MAX_ADDR_SIZE` | 1,000 | Maximum addresses per ADDR message |

**Security Notes**:
- ⚠️ **MAX_INV_SIZE** of 50,000 is very high (50K × 36 bytes = 1.8 MB per message)
- ⚠️ **MAX_HEADERS_SIZE** of 2,000 is high (2K × 80 bytes = 160 KB, but triggers 2K DB lookups)
- **Recommendation**: Consider reducing:
  - `MAX_INV_SIZE`: 1,000 (sufficient for block announcements)
  - `MAX_HEADERS_SIZE`: 2,000 is OK (matches Bitcoin)
  - `MAX_ADDR_SIZE`: 1,000 is OK (matches Bitcoin)

---

### Timeouts
**Location**: `include/network/protocol.hpp:80-83`

```cpp
constexpr int VERSION_HANDSHAKE_TIMEOUT_SEC = 60;   // 1 minute for handshake
constexpr int PING_INTERVAL_SEC = 120;              // 2 minutes between pings
constexpr int PING_TIMEOUT_SEC = 20 * 60;           // 20 minutes - peer must respond
constexpr int INACTIVITY_TIMEOUT_SEC = 20 * 60;     // 20 minutes - matches Bitcoin
```

#### Parameters:

| Parameter | Value | Description |
|-----------|-------|-------------|
| `VERSION_HANDSHAKE_TIMEOUT_SEC` | 60s | Max time for VERSION/VERACK exchange |
| `PING_INTERVAL_SEC` | 120s | Time between PING messages |
| `PING_TIMEOUT_SEC` | 1200s | Max time to wait for PONG response |
| `INACTIVITY_TIMEOUT_SEC` | 1200s | Max idle time before disconnect |

**Tuning Recommendations**:

- **Handshake Timeout**:
  - Default: 60 seconds
  - Slow networks: 90-120 seconds
  - Fast networks: 30 seconds

- **Ping Interval**:
  - Default: 120 seconds (2 minutes)
  - More aggressive: 60 seconds
  - Less aggressive: 180-300 seconds
  - **Tradeoff**: Shorter = faster dead peer detection, longer = less overhead

- **Ping Timeout**:
  - Default: 1200 seconds (20 minutes)
  - More aggressive: 300-600 seconds
  - **Note**: Should be >> PING_INTERVAL

- **Inactivity Timeout**:
  - Default: 1200 seconds (20 minutes, matches Bitcoin Core)
  - Keep at default unless specific needs

---

### Ban & Discouragement
**Location**: `include/sync/banman.hpp:132`

```cpp
static constexpr int64_t DISCOURAGEMENT_DURATION = 24 * 60 * 60;  // 24 hours
```

**Parameters**:

| Parameter | Value | Description |
|-----------|-------|-------------|
| `DISCOURAGEMENT_DURATION` | 86400s (24h) | How long to discourage misbehaving peers |

**Ban Types**:

1. **Manual Bans** (Persistent):
   - Stored in `banlist.json`
   - Can be permanent or timed
   - Use for known malicious nodes

2. **Automatic Discouragement** (Temporary):
   - In-memory only (lost on restart)
   - 24-hour duration
   - Use for misbehaving peers
   - Probabilistic acceptance (simulates bloom filter)

**Tuning**:
- **Discouragement Duration**:
  - Default: 24 hours
  - Stricter: 48-72 hours
  - Lenient: 6-12 hours
  - **Purpose**: Prevent reconnection spam from bad peers

---

## Address Management

### Address Staleness
**Location**: `src/network/addr_manager.cpp:13-15`

```cpp
static constexpr uint32_t STALE_AFTER_DAYS = 30;   // Addresses stale after 30 days
static constexpr uint32_t MAX_FAILURES = 10;        // Max connection failures
static constexpr uint32_t SECONDS_PER_DAY = 86400;
```

#### Parameters:

| Parameter | Value | Description |
|-----------|-------|-------------|
| `STALE_AFTER_DAYS` | 30 days | Consider address stale after this time |
| `MAX_FAILURES` | 10 | Remove address after this many failures |

**Tuning**:
- **Staleness Period**:
  - Default: 30 days
  - Shorter: 7-14 days (more aggressive cleanup)
  - Longer: 60-90 days (keep addresses longer)
  - **Purpose**: Remove offline/dead nodes from address book

- **Max Failures**:
  - Default: 10 attempts
  - Stricter: 5 attempts
  - Lenient: 15-20 attempts
  - **Purpose**: Don't keep trying permanently unreachable addresses

---

### Time Samples
**Location**: `src/util/timedata.cpp:15`

```cpp
static constexpr size_t MAX_TIME_SAMPLES = 200;
```

**Description**: Maximum number of peer time offsets to track for network time adjustment.

**Purpose**:
- Adjust local time based on peer timestamps
- Use median offset to resist manipulation
- Prevent time-based attacks

**Tuning**:
- **Default**: 200 samples (matches Bitcoin Core)
- **Recommendation**: Keep at 200

---

## Timing & Intervals

### Network Manager Intervals
**Location**: `include/network/network_manager.hpp:58-59`

```cpp
std::chrono::seconds connect_interval{5};      // Default: 5 seconds
std::chrono::seconds maintenance_interval{30}; // Default: 30 seconds
```

**Summary Table**:

| Task | Default Interval | Purpose | Tuning Range |
|------|-----------------|---------|--------------|
| Connection attempts | 5s | Try connecting to new peers | 2-30s |
| Maintenance | 30s | Cleanup, ban sweep, tip announce | 10-120s |
| Ping | 120s | Keep-alive check | 60-300s |
| Tip announcement | 30s | Re-announce chain tip | 15-90s |
| Sync timeout check | 60s | Reset stalled sync | 30-120s |

---

## Resource Limits

### Memory Allocations

| Component | Per-Instance | Typical Count | Total Memory |
|-----------|-------------|---------------|--------------|
| Peer connection | 1-2 MB | 133 (8+125) | 133-266 MB |
| RandomX VM (Light) | 256 MB | 2 cached | 512 MB |
| RandomX VM (Fast) | 2.5 GB | 2 cached | 5 GB |
| Receive buffer | 32 MB max | 133 | Up to 4.2 GB |
| Block headers | 80 bytes | ~250K/year | ~20 MB/year |

**Total Memory Estimates**:

| Configuration | Minimum RAM | Recommended RAM |
|--------------|-------------|-----------------|
| Light Node (Light RandomX, 8 peers) | 1 GB | 2 GB |
| Full Node (Light RandomX, 133 peers) | 2 GB | 4 GB |
| Mining Node (Fast RandomX, 133 peers) | 6 GB | 8 GB |

---

### Bandwidth Estimates

| Activity | Upload | Download | Description |
|----------|--------|----------|-------------|
| Idle connection | 1 KB/s | 1 KB/s | Keep-alive pings |
| Header sync | 50 KB/s | 200 KB/s | During IBD |
| Normal operation | 10 KB/s/peer | 10 KB/s/peer | Block announcements |

**Total Bandwidth**:
- **8 outbound peers**: ~80-160 KB/s typical
- **125 inbound peers**: Up to 1.2 MB/s during active sync
- **IBD**: Can sustain 1-2 MB/s download

---

## Chain-Specific Parameters

### MainNet
**Location**: `src/chain/chainparams.cpp` (implementation)

```cpp
// Typical mainnet values (example):
consensus.powLimit = uint256S("00000000ffff0000...");  // Maximum target
consensus.nPowTargetSpacing = 120;                     // 2 minutes
consensus.nRandomXEpochDuration = 7 * 24 * 60 * 60;   // 1 week
consensus.nASERTHalfLife = 2 * 24 * 60 * 60;          // 2 days
consensus.nASERTAnchorHeight = 1;
```

**Recommendations**:
- Set `nMinimumChainWork` to actual mainnet work after bootstrapping
- Use DNS seeds for peer discovery
- Enable listening (`listen_enabled = true`)
- Set appropriate `max_inbound_peers` based on bandwidth

---

### TestNet
**Typical testnet differences**:
- Higher `powLimit` (easier difficulty)
- Same block time (120s)
- Different magic bytes
- Different default port
- `nMinimumChainWork = 0` (no eclipse protection needed)

**Recommendations**:
- Use for testing protocol changes
- Reset periodically (testnet3, testnet4, etc.)
- Allow easier mining for testing

---

### RegTest (Regression Test)
**Typical regtest settings**:
- Maximum `powLimit` (difficulty = 1)
- Same block time (120s) but can be overridden
- Different magic bytes
- Local port only (18444)
- `nMinimumChainWork = 0`

**Recommendations**:
- Use for automated testing
- Can mine blocks instantly
- No network connectivity needed

---

## Configuration Checklist

### Production MainNet Deployment

#### Network Settings
- [ ] Set unique `network_magic` (avoid Bitcoin's values)
- [ ] Configure `listen_port` (ensure firewall allows)
- [ ] Set `listen_enabled = true` (if serving peers)
- [ ] Configure `datadir` for persistent bans
- [ ] Set `io_threads` based on server capacity

#### Peer Management
- [ ] `max_outbound_peers`: 8-15 (based on bandwidth)
- [ ] `max_inbound_peers`: 0-125+ (based on resources)
- [ ] `target_outbound_peers`: Match max_outbound

#### Resource Allocation
- [ ] Ensure minimum 2 GB RAM for light mode
- [ ] Ensure 8 GB+ RAM if using fast RandomX mode
- [ ] Verify sufficient bandwidth (1-2 MB/s for full node)
- [ ] Check disk space for blockchain growth

#### Security Settings
- [ ] Enable `banman` with persistent storage
- [ ] Set reasonable timeouts (defaults OK)
- [ ] Consider reducing `MAX_INV_SIZE` and `MAX_HEADERS_SIZE`
- [ ] Implement rate limiting (see security audit)

#### Consensus Parameters
- [ ] Verify genesis block hash
- [ ] Set `nMinimumChainWork` to known good value
- [ ] Confirm `nPowTargetSpacing` (block time)
- [ ] Validate `nRandomXEpochDuration` (1 week recommended)

---

### Low-Resource Node Configuration

```cpp
NetworkManager::Config config;
config.io_threads = 2;                           // Minimal threads
config.par_threads = 1;                          // Single verification thread
config.listen_enabled = false;                   // No inbound
config.connect_interval = std::chrono::seconds(10);  // Less aggressive

PeerManager::Config peer_config;
peer_config.max_outbound_peers = 4;              // Minimal peers
peer_config.max_inbound_peers = 0;               // No inbound
peer_config.target_outbound_peers = 4;

// RandomX: Use light mode (default)
InitRandomX(1, false);  // Cache 1 epoch, light mode
```

**Expected resource usage**:
- RAM: ~800 MB
- Bandwidth: ~40 KB/s average
- CPU: Minimal when synced

---

### High-Performance Server Configuration

```cpp
NetworkManager::Config config;
config.io_threads = 16;                          // Many threads
config.par_threads = 0;                          // Auto-detect (use all cores)
config.listen_enabled = true;                    // Accept inbound
config.connect_interval = std::chrono::seconds(3);   // Aggressive
config.maintenance_interval = std::chrono::seconds(15);  // Frequent cleanup

PeerManager::Config peer_config;
peer_config.max_outbound_peers = 15;             // Many outbound
peer_config.max_inbound_peers = 500;             // Serve network
peer_config.target_outbound_peers = 15;

// RandomX: Use fast mode after IBD
InitRandomX(3, true);  // Cache 3 epochs, fast mode
```

**Expected resource usage**:
- RAM: 8-10 GB
- Bandwidth: 5-10 MB/s during active periods
- CPU: High during verification

---

## Performance Tuning Guide

### Optimizing for Sync Speed

1. **Increase outbound peers**: 10-15 (more sources)
2. **Increase `io_threads`**: 8-16 (parallel processing)
3. **Decrease `connect_interval`**: 2-3s (faster peer discovery)
4. **Use fast RandomX mode**: After initial sync
5. **Increase `par_threads`**: Match CPU core count

### Optimizing for Bandwidth

1. **Reduce peers**: 4-6 outbound, 0 inbound
2. **Increase intervals**:
   - `connect_interval`: 15-30s
   - `maintenance_interval`: 60-120s
3. **Disable listening**: `listen_enabled = false`
4. **Increase timeouts**: More patient connections

### Optimizing for Memory

1. **Minimize peers**: 4 outbound, 0 inbound
2. **Reduce `io_threads`**: 1-2
3. **Single RandomX cache**: `InitRandomX(1, false)`
4. **Reduce `par_threads`**: 1-2

### Optimizing for Latency

1. **Increase outbound peers**: 10-15 (redundancy)
2. **Decrease intervals**:
   - `connect_interval`: 2-3s
   - `ping_interval`: 60s
3. **Decrease timeouts**: 30s handshake, 300s ping timeout
4. **Geographic diversity**: Connect to peers worldwide

---

## Monitoring & Metrics

### Key Metrics to Track

1. **Peer Count**:
   - Outbound: Should stay near `target_outbound_peers`
   - Inbound: Should not exceed `max_inbound_peers`
   - Churn rate: How often peers disconnect

2. **Synchronization**:
   - Current height vs network height
   - Sync speed (blocks/second or headers/second)
   - Stuck sync detection (> 60s without progress)

3. **Resource Usage**:
   - Memory: RSS, heap size
   - CPU: Per-thread usage
   - Bandwidth: Upload/download rates
   - Disk I/O: Read/write rates

4. **Ban Statistics**:
   - Number of banned addresses
   - Number of discouraged addresses
   - Ban reasons (if logging)

5. **RandomX Performance**:
   - Hash rate (if mining)
   - VM cache hit rate
   - Epoch transitions

---

## Debugging & Troubleshooting

### Common Issues

**Issue**: Not finding peers
- Check: `max_outbound_peers` > 0
- Check: DNS seeds configured
- Check: Firewall not blocking outbound
- Increase: `connect_interval` if too slow

**Issue**: Sync stuck
- Check: At least 1 peer connected
- Check: `sync_timeout` not too short
- Check: `par_threads` > 0 or auto
- Verify: Chain not under attack (check known peers)

**Issue**: High memory usage
- Check: RandomX mode (fast vs light)
- Check: `max_inbound_peers` not too high
- Check: Receive buffers (possible DoS)
- Reduce: `io_threads` if excessive

**Issue**: High bandwidth usage
- Check: `max_inbound_peers` serving many peers
- Reduce: Peer counts
- Increase: Intervals (slower updates)

---

## Security Best Practices

1. **Never expose RPC publicly** without authentication
2. **Use firewall** to limit P2P to necessary ports
3. **Enable bans** with persistent storage
4. **Monitor** for unusual peer behavior
5. **Update** protocol version as needed
6. **Validate** genesis hash on startup
7. **Use anchors** for eclipse resistance
8. **Set minimum chain work** for mainnet
9. **Rate limit** external inputs (see security audit)
10. **Keep software updated** with security patches

---

## Reference Implementation Values

### Bitcoin Core Comparison

| Parameter | CoinbaseChain | Bitcoin Core | Notes |
|-----------|---------------|--------------|-------|
| Protocol Version | 70016 | 70016 | Match |
| Max Outbound | 8 | 10 | Slightly lower |
| Max Inbound | 125 | 125 | Match |
| Max Message Size | 32 MB | 32 MB | Match |
| Ping Interval | 120s | 120s | Match |
| Inactivity Timeout | 1200s | 1200s | Match |
| Max Headers | 2000 | 2000 | Match |
| Block Time | 120s | 600s | 5× faster |

---

## Glossary

- **IBD**: Initial Block Download - Synchronizing blockchain from scratch
- **ASERT**: Absolutely Scheduled Exponentially Rising Targets - Difficulty adjustment algorithm
- **RandomX**: ASIC-resistant Proof-of-Work algorithm
- **Epoch**: Time period for RandomX key (1 week default)
- **Anchor**: Reliable peer saved for reconnection
- **DoS**: Denial of Service attack
- **Eclipse Attack**: Isolating node by controlling all its peers
- **Discouragement**: Temporary soft-ban for misbehaving peers

---

## Changelog

### Version 1.0 (2025-10-17)
- Initial comprehensive parameter documentation
- Extracted all configurable constants from codebase
- Added tuning recommendations
- Included performance and security guidance

---

## See Also

- [NETWORK_SECURITY_AUDIT.md](NETWORK_SECURITY_AUDIT.md) - Security vulnerabilities and fixes
- Bitcoin Core documentation: https://github.com/bitcoin/bitcoin/tree/master/doc
- RandomX specification: https://github.com/tevador/RandomX
- ASERT algorithm: https://gitlab.com/bitcoin-cash-node/bchn-sw/bitcoincash-upgrade-specifications

---

**Document Maintained By**: System Architecture Team
**Last Review**: 2025-10-17
**Next Review**: Quarterly or on major version change
