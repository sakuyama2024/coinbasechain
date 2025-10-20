# Constants and User Inputs Audit

**Purpose**: Catalog all constants and user-controllable inputs in the codebase, categorized by networking vs chain functionality.

**Date**: 2025-10-20

---

## NETWORKING CONSTANTS

### Protocol Constants (`include/network/protocol.hpp`)

#### Network Identification
```cpp
constexpr uint32_t PROTOCOL_VERSION = 1;
constexpr uint32_t MAINNET_MAGIC = 0x554E4943;  // "UNIC"
constexpr uint32_t TESTNET_MAGIC = 0xA3F8D412;
constexpr uint32_t REGTEST_MAGIC = 0x4B7C2E91;
```

#### Default Ports
```cpp
constexpr uint16_t MAINNET_PORT = 9590;
constexpr uint16_t TESTNET_PORT = 19590;  // MAINNET + 10000
constexpr uint16_t REGTEST_PORT = 29590;  // MAINNET + 20000
```

#### Message Size Limits (DoS Protection)
```cpp
constexpr uint64_t MAX_SIZE = 0x02000000;                  // 32 MB - Max serialized object size
constexpr size_t MAX_VECTOR_ALLOCATE = 5 * 1000 * 1000;    // 5 MB - Incremental allocation limit
constexpr size_t MAX_PROTOCOL_MESSAGE_LENGTH = 4 * 1000 * 1000;  // 4 MB - Single message limit
constexpr size_t DEFAULT_RECV_FLOOD_SIZE = 5 * 1000 * 1000;      // 5 MB - Flood protection
```

#### Protocol Message Limits
```cpp
constexpr unsigned int MAX_LOCATOR_SZ = 101;      // GETHEADERS/GETBLOCKS locator limit
constexpr uint32_t MAX_INV_SIZE = 50000;          // Inventory items per message
constexpr uint32_t MAX_HEADERS_SIZE = 2000;       // Headers per HEADERS message
constexpr uint32_t MAX_ADDR_SIZE = 1000;          // Addresses per ADDR message
```

#### Timeout Constants
```cpp
constexpr int VERSION_HANDSHAKE_TIMEOUT_SEC = 60;     // 1 minute for handshake
constexpr int PING_INTERVAL_SEC = 120;                // 2 minutes between pings
constexpr int PING_TIMEOUT_SEC = 20 * 60;             // 20 minutes - peer must respond
constexpr int INACTIVITY_TIMEOUT_SEC = 20 * 60;       // 20 minutes - idle disconnect
```

#### Network Address Constants
```cpp
constexpr size_t MAX_SUBVERSION_LENGTH = 256;
constexpr int64_t TIMESTAMP_ALLOWANCE_SEC = 2 * 60 * 60;  // 2 hours
```

#### Message Header Structure
```cpp
constexpr size_t MESSAGE_HEADER_SIZE = 24;
constexpr size_t COMMAND_SIZE = 12;
constexpr size_t CHECKSUM_SIZE = 4;
```

### Peer Manager Constants (`include/network/peer_manager.hpp`)

#### Connection Limits
```cpp
// PeerManager::Config defaults
max_outbound_peers = 8;
max_inbound_peers = 125;
target_outbound_peers = 8;
```

#### DoS Protection
```cpp
static constexpr int DISCOURAGEMENT_THRESHOLD = 100;
static constexpr int MAX_UNCONNECTING_HEADERS = 10;
```

#### Misbehavior Penalties
```cpp
namespace MisbehaviorPenalty {
  static constexpr int INVALID_POW = 100;
  static constexpr int OVERSIZED_MESSAGE = 20;
  static constexpr int NON_CONTINUOUS_HEADERS = 20;
  static constexpr int LOW_WORK_HEADERS = 10;
  static constexpr int INVALID_HEADER = 100;
  static constexpr int TOO_MANY_UNCONNECTING = 100;  // Instant disconnect
  static constexpr int TOO_MANY_ORPHANS = 100;       // Instant disconnect
}
```

### Ban Manager Constants (`include/network/banman.hpp`)

```cpp
static constexpr int64_t DISCOURAGEMENT_DURATION = 24 * 60 * 60;  // 24 hours
```

### Network Manager Configuration (`include/network/network_manager.hpp`)

```cpp
struct Config {
  uint32_t network_magic = protocol::magic::MAINNET;
  uint16_t listen_port = protocol::ports::MAINNET;
  bool listen_enabled = false;
  bool enable_nat = true;
  size_t io_threads = 4;
  std::string datadir = "";
  std::chrono::seconds connect_interval = std::chrono::seconds(5);
  std::chrono::seconds maintenance_interval = std::chrono::seconds(30);
};
```

---

## CHAIN CONSTANTS

### Consensus Parameters (`include/chain/chainparams.hpp`)

```cpp
struct ConsensusParams {
  uint256 powLimit;                          // Maximum difficulty (easiest target)
  int64_t nPowTargetSpacing = 120;           // 2 minutes between blocks
  int64_t nRandomXEpochDuration = 7 * 24 * 60 * 60;  // 1 week
  int64_t nASERTHalfLife = 2 * 24 * 60 * 60;         // 2 days (in seconds)
  int32_t nASERTAnchorHeight = 1;            // ASERT anchor block height
  uint256 hashGenesisBlock;
  uint256 nMinimumChainWork;
};
```

### Block Header Constants (`include/chain/block.hpp`)

```cpp
static constexpr size_t UINT256_BYTES = 32;
static constexpr size_t UINT160_BYTES = 20;
static constexpr size_t HEADER_SIZE = 100;   // Total block header size in bytes
```

### Validation Constants (`include/chain/validation.hpp`)

```cpp
static constexpr int64_t MAX_FUTURE_BLOCK_TIME = 2 * 60 * 60;  // 2 hours
```

### Orphan Header Limits (`include/chain/chainstate_manager.hpp`)

```cpp
static constexpr size_t MAX_ORPHAN_HEADERS = 1000;         // Total orphans across all peers
static constexpr size_t MAX_ORPHAN_HEADERS_PER_PEER = 50;  // Max orphans per peer
static constexpr int64_t ORPHAN_HEADER_EXPIRE_TIME = 600;  // 10 minutes in seconds
```

### Time Validation Constants (`include/chain/timedata.hpp`)

```cpp
static constexpr int64_t DEFAULT_MAX_TIME_ADJUSTMENT = 70 * 60;  // 70 minutes
```

### RandomX Constants (`include/chain/randomx_pow.hpp`)

```cpp
static constexpr int DEFAULT_RANDOMX_VM_CACHE_SIZE = 2;
```

---

## USER INPUTS - NETWORKING

### Command-Line Arguments (main.cpp)

#### Network Configuration
```
--port=<port>         Listen port (default: 9590 mainnet, 19590 testnet, 29590 regtest)
--listen              Enable inbound connections
--nolisten            Disable inbound connections (default)
--threads=<n>         Number of IO threads (default: 4)
--regtest             Use regression test chain
--testnet             Use test network
```

#### Data Directory
```
--datadir=<path>      Data directory (default: ~/.coinbasechain)
```

### RPC Commands (Network-Related)

**From `src/network/rpc_server.cpp` and `src/cli.cpp`:**

```
getpeerinfo           Get connected peer information
addnode <address>     Add a node connection
```

---

## USER INPUTS - CHAIN

### Command-Line Arguments (main.cpp)

#### Chain Configuration
```
--suspiciousreorgdepth=<n>  Max reorg depth before halt (default: 100, 0 = unlimited)
--par=<n>                   Number of parallel RandomX verification threads (default: 0 = auto)
```

#### Application Configuration (`include/application.hpp`)
```cpp
struct AppConfig {
  int suspicious_reorg_depth = 100;  // Default suspicious reorg depth
};
```

### RPC Commands (Chain-Related)

**From `src/network/rpc_server.cpp` and `src/cli.cpp`:**

#### Blockchain Queries
```
getinfo                   Get general node information
getblockchaininfo         Get blockchain state information
getblockcount             Get current block height
getblockhash <height>     Get block hash at height (INPUT: integer height)
getblockheader <hash>     Get block header by hash (INPUT: hex string 64 chars)
getbestblockhash          Get hash of best (tip) block
getdifficulty             Get proof-of-work difficulty
```

#### Mining Commands
```
getmininginfo             Get mining-related information
getnetworkhashps [nblocks]  Get network hashes per second (INPUT: optional integer)
startmining               Start CPU mining (regtest only)
stopmining                Stop CPU mining
generate <nblocks>        Mine N blocks (INPUT: 1-100, regtest only)
```

#### Testing Commands (Regtest/Testnet Only)
```
setmocktime <timestamp>   Set mock time for testing (INPUT: unix timestamp 0 to 4294967295)
invalidateblock <hash>    Mark block as invalid (INPUT: hex string 64 chars)
```

#### Control
```
stop                      Stop the node
```

---

## SECURITY-CRITICAL INPUT VALIDATION

### RPC Server Input Validation (`src/network/rpc_server.cpp`)

**All RPC inputs go through validation helpers:**

```cpp
std::optional<int> SafeParseInt(const std::string& str, int min, int max);
std::optional<uint256> SafeParseHash(const std::string& str);
std::optional<uint16_t> SafeParsePort(const std::string& str);
std::string EscapeJSONString(const std::string& str);
```

**Specific validations:**
- `getblockhash`: Height validated as integer in range
- `getblockheader`: Hash validated as 64-character hex string
- `generate`: Block count limited to 1-100, regtest-only
- `setmocktime`: Timestamp limited to 0-4294967295, non-mainnet only
- `invalidateblock`: Hash validated as 64-character hex string

### Network Message Validation

**All incoming network messages validated against:**
- Message size limits (MAX_PROTOCOL_MESSAGE_LENGTH = 4 MB)
- Headers batch size limits (MAX_HEADERS_SIZE = 2000)
- Inventory size limits (MAX_INV_SIZE = 50000)
- Address message limits (MAX_ADDR_SIZE = 1000)
- Flood protection (DEFAULT_RECV_FLOOD_SIZE = 5 MB)

---

## FIXED SEED NODES

**Source**: `include/chain/chainparams.hpp`

```cpp
std::vector<std::string> vFixedSeeds;  // Hardcoded seed node addresses (IP:port)
```

**Implementation**: See `src/chain/chainparams.cpp` for actual seed addresses per network.

---

## CONFIGURATION FILES

### Persistent Ban List
- **Path**: `<datadir>/banlist.json`
- **Format**: JSON serialization of CBanEntry objects
- **Contents**: Banned IP addresses with creation time and expiry time

### Peer Anchors
- **Path**: `<datadir>/anchors.dat`
- **Format**: JSON array of NetworkAddress objects
- **Purpose**: Eclipse attack resistance (reconnect to known-good peers)

### Block Headers Database
- **Path**: `<datadir>/headers.dat`
- **Format**: Custom binary serialization
- **Contents**: Full block header chain

---

## ATTACK SURFACE ANALYSIS

### High-Risk User Inputs (Requires Validation)

1. **Network Messages** (External, Untrusted)
   - All P2P protocol messages
   - Validated against size limits and protocol constraints
   - Misbehavior penalties for violations

2. **RPC Commands** (Local, Semi-Trusted)
   - Block hashes (must be valid 64-char hex)
   - Block heights (must be valid integers)
   - Timestamps (must be in valid range)
   - Generate block counts (limited to 1-100)
   - All inputs validated by SafeParse* functions

3. **Command-Line Arguments** (Local, Trusted)
   - Port numbers
   - Thread counts
   - Reorg depth limits
   - Minimal validation (std::stoi with exception handling)

4. **Configuration Files** (Local, Semi-Trusted)
   - Ban list JSON (loaded with error handling)
   - Anchors JSON (loaded with error handling)
   - Headers database (checksums and validation)

### Low-Risk Inputs

1. **Fixed Seed Nodes** (Hardcoded, Trusted)
   - Part of source code, not user-controllable

2. **Genesis Block** (Hardcoded, Trusted)
   - Part of consensus rules, immutable

---

## RECOMMENDATIONS

### High Priority

1. **Add range validation for command-line arguments**
   - Port numbers should be validated (1-65535)
   - Thread counts should have reasonable max (e.g., 1-128)
   - Reorg depth should have reasonable max (e.g., 0-10000)

2. **Add configuration file validation**
   - Validate JSON schema before processing
   - Limit file sizes to prevent DoS
   - Sanitize all string inputs from files

### Medium Priority

3. **Add rate limiting to RPC commands**
   - Prevent abuse of expensive commands (getblockchaininfo, getmininginfo)
   - Limit frequency of generate/invalidateblock commands

4. **Add IP address validation**
   - Validate all peer addresses before connection attempts
   - Check for private/reserved ranges appropriately
   - Validate port numbers from network messages

### Low Priority

5. **Document all constants in code**
   - Add comments explaining why each limit was chosen
   - Reference Bitcoin Core or other sources where applicable

6. **Create constants header file**
   - Centralize all magic numbers
   - Make constants easily discoverable and modifiable

---

## SUMMARY

### Networking Constants: 25+
- Protocol limits, timeouts, connection limits, DoS thresholds

### Chain Constants: 15+
- Consensus parameters, block timing, orphan limits, validation rules

### User Inputs - Networking: 5
- Command-line args (port, listen, threads, network type)
- RPC commands (getpeerinfo, addnode)

### User Inputs - Chain: 12
- Command-line args (suspiciousreorgdepth, par)
- RPC commands (blockchain queries, mining commands, test commands)

**Total Attack Surface**: 17 user-controllable input points
- 5 networking inputs (mostly configuration)
- 12 chain inputs (mostly RPC queries and test commands)
- All RPC inputs validated through SafeParse* helpers
- Network message inputs validated against protocol limits

