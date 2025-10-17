# Command-Line Arguments: Unicity vs Our Implementation

## Unicity's Argument System

**Key Features:**
- **ArgsManager** - Sophisticated argument parsing with categories, validation, help text
- **Config file support** (`bitcoin.conf`)
- **Settings persistence** (`settings.json`)
- **Network-specific options** (mainnet/testnet/regtest)
- **Argument validation** (ALLOW_ANY, DISALLOW_NEGATION, DEBUG_ONLY, etc.)
- **Help categories** (OPTIONS, CONNECTION, WALLET, RPC, DEBUG, etc.)
- **Config includes** (`-includeconf`)
- **Sensitive argument handling** (passwords, keys)

## Our Current Implementation

**What we have:**
```cpp
--help               Show help
--datadir=<path>     Data directory
--port=<port>        Listen port
--listen             Enable inbound
--nolisten           Disable inbound
--threads=<n>        IO threads
--verbose            Verbose logging
```

**Implementation:** Simple string parsing in main.cpp

## Critical Missing Arguments for Headers-Only Chain

### 1. **Network Configuration**
```
-testnet                 Use testnet
-regtest                 Use regression test network
-chain=<chain>          Specify chain (main/test/regtest)
```
**Priority:** HIGH - Need this for testing

### 2. **Connection Management**
```
-addnode=<ip>           Add specific peer
-connect=<ip>           Connect ONLY to specific peer
-seednode=<ip>          Get addresses then disconnect
-maxconnections=<n>     Max total connections (default: 125)
-timeout=<n>            Connection timeout (ms)
-peertimeout=<n>        Peer inactivity timeout (s)
```
**Priority:** HIGH - Essential for p2p networking

### 3. **Network Options**
```
-dns                    Allow DNS lookups (default: 1)
-dnsseed                Query DNS seeds (default: 1)
-bind=<addr>:<port>     Bind to specific address
-proxy=<ip:port>        SOCKS5 proxy
-onlynet=<net>          Only connect to network (ipv4/ipv6)
-discover               Discover own IP (default: 1)
-externalip=<ip>        Advertise this IP
```
**Priority:** MEDIUM - Nice to have for flexibility

### 4. **Bandwidth**
```
-maxuploadtarget=<n>    Limit outbound bandwidth (MB/24h)
-maxreceivebuffer=<n>   Per-connection receive buffer
-maxsendbuffer=<n>      Per-connection send buffer
```
**Priority:** MEDIUM - DoS protection

### 5. **Blockchain/Storage**
```
-blocksdir=<dir>        Block storage directory
-prune=<n>              Prune old blocks (keep last N MB)
-reindex                Rebuild block index
```
**Priority:** LOW for now (no blocks yet)

### 6. **Daemon Options**
```
-daemon                 Run in background
-daemonwait             Wait for init before daemonizing
-pid=<file>             PID file location
-conf=<file>            Config file (default: coinbasechain.conf)
-includeconf=<file>     Include additional config
```
**Priority:** MEDIUM - Useful for production

### 7. **Logging** (we have --verbose)
```
-debug=<category>       Enable debug logging (net, addrman, etc.)
-debugexclude=<cat>     Exclude debug category
-logips                 Log peer IP addresses
-logtimestamps          Timestamp log entries
-shrinkdebugfile        Shrink debug.log on startup
-printtoconsole         Print to console instead of file
```
**Priority:** HIGH - Need better logging

### 8. **Admin/RPC** (not needed yet)
```
-rpcuser=<user>
-rpcpassword=<pass>
-rpcport=<port>
-server                 Accept RPC connections
```
**Priority:** LOW - Phase 10+

## Unicity-Specific We DON'T Need

```
-alertnotify            Command on alert
-blocknotify            Command on new block
-wallet options         (no transactions)
-txindex                (no transactions)
-coinstatsindex         (no UTXO)
-zmq options            (message queue)
-randomxfastmode        (PoW - will need eventually)
```

## Recommended Additions for Next Phase

### Tier 1 (Essential - Add Now):
```cpp
--testnet / --regtest   // Network selection
--addnode=<ip>          // Manual peer
--connect=<ip>          // Only connect to specific peer
--maxconnections=<n>    // Connection limit
--debug=<category>      // Debug logging
```

### Tier 2 (Important - Add Soon):
```cpp
--daemon                // Background mode
--conf=<file>           // Config file support
--bind=<addr>:<port>    // Bind address
--timeout=<n>           // Connection timeout
--dnsseed               // DNS seeding
--seednode=<ip>         // Bootstrap node
```

### Tier 3 (Nice to Have):
```cpp
--maxuploadtarget=<n>   // Bandwidth limit
--proxy=<ip:port>       // SOCKS5 proxy
--onlynet=<net>         // Network filter
--logips                // Log IPs
--printtoconsole        // Console logging
```

## Implementation Approach

### Option A: Simple (Current)
Keep our simple string parsing for now. Add arguments as needed.

**Pros:** Simple, no dependencies
**Cons:** Manual validation, no config file, no help generation

### Option B: Boost.Program_Options
Use Boost library for argument parsing.

**Pros:** Automatic help, validation, type safety
**Cons:** Another Boost dependency (but we already use Boost)

### Option C: Mini ArgsManager
Create simplified version of Unicity's ArgsManager.

**Pros:** Tailored to our needs, config file support
**Cons:** More code to maintain

## Recommendation

**For now (Phase 6-8):**
- Keep simple parsing
- Add Tier 1 arguments manually as needed
- Focus on blockchain sync

**After Phase 9:**
- Implement Option B or C
- Add config file support
- Add better logging framework
- Add Tier 2/3 arguments

## Config File Format (Future)

```ini
# coinbasechain.conf
datadir=/path/to/data
port=8333
listen=1

# Network
maxconnections=125
addnode=seed1.example.com
addnode=seed2.example.com

# Logging
debug=net
debug=addrman
printtoconsole=1
```

## Current vs Needed Arguments

| Feature | We Have | Unicity Has | Need? |
|---------|---------|-------------|-------|
| Data dir | ✅ | ✅ | ✅ |
| Port | ✅ | ✅ | ✅ |
| Listen | ✅ | ✅ | ✅ |
| Help | ✅ | ✅ | ✅ |
| Network (test/reg) | ❌ | ✅ | ✅ HIGH |
| Add node | ❌ | ✅ | ✅ HIGH |
| Connect | ❌ | ✅ | ✅ HIGH |
| Max connections | ❌ | ✅ | ✅ HIGH |
| Debug logging | Partial | ✅ | ✅ HIGH |
| Config file | ❌ | ✅ | ⏳ MEDIUM |
| Daemon mode | ❌ | ✅ | ⏳ MEDIUM |
| Bandwidth limits | ❌ | ✅ | ⏳ MEDIUM |
| Proxy/Tor | ❌ | ✅ | ⏳ LOW |
| RPC | ❌ | ✅ | ⏳ LOW |

## Conclusion

Our argument parsing is **sufficient for development** but needs expansion for production use.

**Immediate action:** Add `--testnet`, `--addnode`, `--connect` for Phase 6-8 testing.

**Future action:** Implement proper ArgsManager after Phase 9.
