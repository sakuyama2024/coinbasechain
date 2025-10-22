# CoinbaseChain Architecture

**Version:** 1.0.0
**Last Updated:** 2025-10-21
**Status:** Production Ready

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [System Overview](#2-system-overview)
3. [Network Protocol Layer](#3-network-protocol-layer)
4. [Consensus Protocol Layer](#4-consensus-protocol-layer)
5. [Data Flow](#5-data-flow)
6. [Component Architecture](#6-component-architecture)
7. [Security Architecture](#7-security-architecture)
8. [Performance Characteristics](#8-performance-characteristics)
9. [Deployment Architecture](#9-deployment-architecture)

---

## 1. Executive Summary

CoinbaseChain is a **headers-only blockchain** implementation that combines:
- **Bitcoin-inspired P2P networking** with strategic compatibility
- **RandomX proof-of-work** for ASIC resistance
- **ASERT difficulty adjustment** for responsive block times
- **No transaction layer** - simplified consensus for specific use cases

### Key Design Principles

1. **Simplicity** - Headers-only design removes transaction complexity
2. **Security** - RandomX PoW + comprehensive DoS protection
3. **Responsiveness** - Per-block difficulty adjustment with ASERT
4. **Compatibility** - Strategic reuse of proven Bitcoin patterns

---

## 2. System Overview

### 2.1 High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Application Layer                        │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
│  │   RPC    │  │   CLI    │  │   API    │  │   GUI    │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │
└─────────────────────────────────────────────────────────────┘
                              │
┌─────────────────────────────────────────────────────────────┐
│                    Blockchain Layer                          │
│  ┌───────────────────────────────────────────────────────┐  │
│  │              ChainstateManager                        │  │
│  │  ┌─────────────┐  ┌─────────────┐  ┌──────────────┐  │  │
│  │  │  Validation │  │   Chain     │  │   Block      │  │  │
│  │  │    Rules    │  │  Selector   │  │   Manager    │  │  │
│  │  └─────────────┘  └─────────────┘  └──────────────┘  │  │
│  └───────────────────────────────────────────────────────┘  │
│                                                              │
│  ┌───────────────────────────────────────────────────────┐  │
│  │                  Consensus Layer                      │  │
│  │  ┌─────────────┐  ┌─────────────┐  ┌──────────────┐  │  │
│  │  │   RandomX   │  │    ASERT    │  │   Header     │  │  │
│  │  │     PoW     │  │ Difficulty  │  │  Validation  │  │  │
│  │  └─────────────┘  └─────────────┘  └──────────────┘  │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                              │
┌─────────────────────────────────────────────────────────────┐
│                     Network Layer                            │
│  ┌───────────────────────────────────────────────────────┐  │
│  │              NetworkManager                           │  │
│  │  ┌─────────────┐  ┌─────────────┐  ┌──────────────┐  │  │
│  │  │PeerManager  │  │AddrManager  │  │HeaderSync    │  │  │
│  │  │            │  │             │  │   Manager    │  │  │
│  │  └─────────────┘  └─────────────┘  └──────────────┘  │  │
│  └───────────────────────────────────────────────────────┘  │
│                                                              │
│  ┌───────────────────────────────────────────────────────┐  │
│  │                P2P Protocol Layer                     │  │
│  │  ┌─────────────┐  ┌─────────────┐  ┌──────────────┐  │  │
│  │  │    Peer     │  │  Transport  │  │   Message    │  │  │
│  │  │ Connection  │  │   (TCP)     │  │   Protocol   │  │  │
│  │  └─────────────┘  └─────────────┘  └──────────────┘  │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 Core Components

| Component | Purpose | Key Classes |
|-----------|---------|-------------|
| **Network Layer** | P2P communication | NetworkManager, PeerManager, Peer |
| **Consensus Layer** | Block validation | ChainstateManager, Validation |
| **Storage Layer** | Data persistence | BlockManager, LevelDB |
| **Crypto Layer** | PoW & hashing | RandomX, SHA256 |
| **RPC Layer** | External interface | RPCServer |

---

## 3. Network Protocol Layer

### 3.1 P2P Network Stack

```
Application Messages (headers, inv, getdata)
                ↓
┌────────────────────────────────────────┐
│          Message Protocol              │
│  • Framing (24-byte header)           │
│  • Checksum validation                 │
│  • Command dispatch                    │
└────────────────────────────────────────┘
                ↓
┌────────────────────────────────────────┐
│          Peer Management               │
│  • Connection lifecycle                │
│  • Handshake (VERSION/VERACK)         │
│  • Ping/Pong keepalive                │
│  • Misbehavior tracking               │
└────────────────────────────────────────┘
                ↓
┌────────────────────────────────────────┐
│         Transport Layer                │
│  • TCP connections                     │
│  • Async I/O (Boost.Asio)             │
│  • Connection pooling                  │
└────────────────────────────────────────┘
```

### 3.2 Message Protocol

#### Message Header Format (24 bytes)
```
┌─────────────┬──────────────┬─────────────┬──────────────┐
│ Magic       │ Command      │ Length      │ Checksum     │
│ (4 bytes)   │ (12 bytes)   │ (4 bytes)   │ (4 bytes)    │
├─────────────┼──────────────┼─────────────┼──────────────┤
│ 0x554E4943  │ "headers\0"  │ 0x00002710  │ SHA256[:4]   │
│ ("UNIC")    │ (null-pad)   │ (10,000)    │ of payload   │
└─────────────┴──────────────┴─────────────┴──────────────┘
```

#### Supported Messages

| Message | Direction | Purpose | Size Limit |
|---------|-----------|---------|------------|
| version | Both | Handshake initiation | ~200 bytes |
| verack | Both | Handshake acknowledgment | 0 bytes |
| ping | Both | Keepalive request | 8 bytes |
| pong | Both | Keepalive response | 8 bytes |
| addr | Both | Peer address exchange | 30KB |
| getaddr | Both | Request peer addresses | 0 bytes |
| inv | Both | Announce headers | 50KB |
| getdata | Both | Request headers | 50KB |
| headers | Both | Header relay | 200KB |
| getheaders | Both | Request headers | ~1KB |
| notfound | Both | Data not available | 50KB |

### 3.3 Connection Management

#### Connection Types
```cpp
enum class ConnectionType {
    INBOUND,              // Incoming connection
    OUTBOUND_FULL_RELAY,  // Full outbound peer
    FEELER                // Test connection (disconnects after handshake)
};
```

#### Connection Lifecycle
```
   NEW → CONNECTING → CONNECTED → VERSION_SENT → READY
                                       ↓
                                   DISCONNECTING → DISCONNECTED
```

#### Connection Limits
- **Max Inbound:** 125 connections
- **Max Outbound:** 8 full relay connections
- **Feeler Connections:** 1 every 120 seconds
- **Connection Timeout:** 60 seconds for handshake

### 3.4 Peer Discovery

```
┌────────────────┐     ┌────────────────┐     ┌────────────────┐
│   DNS Seeds    │────▶│  AddrManager   │────▶│ Peer Selection │
└────────────────┘     └────────────────┘     └────────────────┘
                              ▲                        │
                              │                        ▼
┌────────────────┐            │              ┌────────────────┐
│  Anchor Nodes  │────────────┘              │   Connect to   │
└────────────────┘                           │     Peers      │
                                             └────────────────┘
```

---

## 4. Consensus Protocol Layer

### 4.1 Block Validation Pipeline

```
           Incoming Header
                 │
                 ▼
┌─────────────────────────────┐
│   Layer 1: Pre-validation    │
│  • PoW commitment check      │ ← ~1ms (50x faster)
│  • Anti-DoS work threshold   │
└─────────────────────────────┘
                 │
                 ▼
┌─────────────────────────────┐
│  Layer 2: Context-free       │
│  • Full RandomX PoW verify   │ ← ~50ms
│  • Header structure check    │
└─────────────────────────────┘
                 │
                 ▼
┌─────────────────────────────┐
│   Layer 3: Contextual        │
│  • Difficulty (ASERT)        │ ← ~5ms
│  • Timestamp validation      │
│  • Chain work calculation    │
└─────────────────────────────┘
                 │
                 ▼
┌─────────────────────────────┐
│    Chain State Update        │
│  • Best chain activation     │
│  • Orphan processing         │
│  • Notification dispatch     │
└─────────────────────────────┘
```

### 4.2 Proof of Work (RandomX)

#### RandomX Architecture
```
┌──────────────────────────────────────────────┐
│           RandomX PoW System                  │
├──────────────────────────────────────────────┤
│                                              │
│  ┌────────────┐    ┌────────────────────┐   │
│  │   Header   │───▶│  RandomX Hash      │   │
│  │  (80 bytes)│    │  • 2GB dataset     │   │
│  └────────────┘    │  • CPU-optimized   │   │
│                    └────────────────────┘   │
│                             │                │
│                             ▼                │
│                    ┌────────────────────┐   │
│                    │  20-byte hash     │   │
│                    │  (commitment)      │   │
│                    └────────────────────┘   │
│                             │                │
│                             ▼                │
│  ┌────────────────────────────────────────┐ │
│  │  Stored in header.hashRandomX field    │ │
│  └────────────────────────────────────────┘ │
└──────────────────────────────────────────────┘
```

#### Verification Modes
1. **MINING** - Compute hash, return result
2. **FULL** - Verify hash matches commitment
3. **COMMITMENT_ONLY** - Quick difficulty check

### 4.3 Difficulty Adjustment (ASERT)

#### ASERT Algorithm
```
                     Actual Time - Expected Time
    exponent = ────────────────────────────────── × 2^16
                        Half-life

    New Target = Anchor Target × 2^(exponent/2^16)
```

#### Parameters
- **Target Block Time:** 120 seconds
- **Half-life:** 172,800 seconds (2 days)
- **Anchor Block:** Height 1
- **Adjustment:** Every block

### 4.4 Block Header Structure

```
CoinbaseChain Block Header (100 bytes)
┌──────────────┬────────┬──────────────────────────────┐
│ Field        │ Bytes  │ Description                  │
├──────────────┼────────┼──────────────────────────────┤
│ nVersion     │ 4      │ Block version                │
│ hashPrevBlock│ 32     │ Previous block hash          │
│ minerAddress │ 20     │ Miner reward address         │
│ nTime        │ 4      │ Unix timestamp               │
│ nBits        │ 4      │ Difficulty target            │
│ nNonce       │ 4      │ PoW nonce                    │
│ hashRandomX  │ 20     │ RandomX PoW commitment       │
└──────────────┴────────┴──────────────────────────────┘
Total: 100 bytes
```

### 4.5 Consensus Rules

| Rule | Specification | Enforcement |
|------|--------------|-------------|
| **PoW Valid** | RandomX hash < target | CheckBlockHeader() |
| **Difficulty Correct** | ASERT calculation | ContextualCheckBlockHeader() |
| **Time > MTP** | timestamp > median(last 11) | ContextualCheckBlockHeader() |
| **Future Time Limit** | timestamp < now + 2 hours | ContextualCheckBlockHeader() |
| **Version >= 1** | nVersion >= 1 | CheckBlockHeader() |
| **Previous Block Exists** | hashPrevBlock in index | AcceptBlockHeader() |

---

## 5. Data Flow

### 5.1 Header Reception Flow

```
Network Peer
     │
     ▼ [headers message]
NetworkManager::OnMessage()
     │
     ▼
HeaderSyncManager::ProcessHeaders()
     │
     ▼ [batch validation]
ChainstateManager::AcceptBlockHeader()
     │
     ├─► CheckHeadersPoW() [fast pre-check]
     ├─► CheckBlockHeader() [full PoW]
     └─► ContextualCheckBlockHeader() [consensus]
           │
           ▼
     BlockManager::AddToIndex()
           │
           ▼
     ActivateBestChain()
           │
           ▼ [if new tip]
     NotificationInterface::BlockTip()
```

### 5.2 Header Request Flow

```
Need Headers at Height X
     │
     ▼
BuildBlockLocator()
     │
     ▼
Send "getheaders" message
     │
     ▼
Peer processes request
     │
     ▼
Receive "headers" response
     │
     ▼
Process headers (see above)
```

### 5.3 Orphan Header Processing

```
Header with Unknown Parent
     │
     ▼
AddToOrphanPool()
     │
     ├─► Check per-peer limit (50)
     └─► Check global limit (1000)
           │
           ▼
     Wait for parent...
           │
     Parent Arrives
           │
           ▼
     ProcessOrphanHeaders()
           │
           ▼
     Recursive validation
```

---

## 6. Component Architecture

### 6.1 Core Classes

#### Network Components
```cpp
NetworkManager
├── PeerManager
│   ├── Peer[] (connection objects)
│   └── BanManager
├── AddrManager
│   ├── tried_addresses
│   └── new_addresses
├── HeaderSyncManager
└── MessageRouter
```

#### Consensus Components
```cpp
ChainstateManager
├── BlockManager
│   ├── block_index (map)
│   └── best_chain
├── ChainSelector
├── ValidationInterface
└── OrphanManager
```

### 6.2 Thread Model

```
Main Thread
├── RPC Server
└── Application Logic

Network I/O Thread Pool (4 threads)
├── Accept connections
├── Read/Write messages
└── Async operations

Validation Thread
├── Header validation
├── Chain activation
└── Orphan processing

Mining Thread (optional)
└── RandomX hash computation
```

### 6.3 Storage Architecture

```
Data Directory (~/.coinbasechain/)
├── blocks/
│   └── headers.dat         # Serialized headers
├── chainstate/
│   └── CURRENT            # LevelDB chain state
├── peers.dat              # Known peer addresses
├── anchors.dat           # Trusted anchor nodes
├── banlist.dat           # Banned peers
└── coinbasechain.conf    # Configuration
```

---

## 7. Security Architecture

### 7.1 Attack Mitigation

| Attack Vector | Mitigation | Implementation |
|---------------|------------|----------------|
| **Sybil Attack** | Connection limits | Max 125 inbound |
| **DoS via Headers** | Work threshold | GetAntiDoSWorkThreshold() |
| **Orphan Spam** | Orphan limits | 1000 global, 50 per peer |
| **Time Manipulation** | MTP enforcement | 11-block median |
| **Memory Exhaustion** | Receive buffer limit | 5MB max |
| **CPU Exhaustion** | Commitment-only PoW | 50x faster pre-check |
| **Connection Flood** | Rate limiting | 1 conn/second |
| **Misbehavior** | Ban scoring | 100 points = ban |

### 7.2 Misbehavior Scoring

```cpp
enum MisbehaviorPenalty {
    INVALID_HEADER = 100,      // Instant ban
    INVALID_POW = 100,         // Instant ban
    TOO_MANY_ORPHANS = 100,    // Instant ban
    UNREQUESTED_HEADERS = 20,  // 5 strikes
    TIMEOUT = 10               // 10 strikes
};
```

### 7.3 Cryptographic Security

- **PoW:** RandomX (memory-hard, ASIC-resistant)
- **Hashing:** SHA256 for checksums and merkle trees
- **Addresses:** Base58Check encoding with checksums
- **Randomness:** Cryptographically secure RNG

---

## 8. Performance Characteristics

### 8.1 Benchmarks

| Operation | Performance | Notes |
|-----------|-------------|-------|
| **Header Validation (commitment)** | ~1ms | Fast pre-check |
| **Header Validation (full)** | ~50ms | RandomX verification |
| **Chain Activation** | ~10ms | Update best chain |
| **Message Processing** | <1ms | Parse and dispatch |
| **Peer Handshake** | ~100ms | VERSION/VERACK |
| **Block Time** | 120 seconds | Target |
| **Sync Speed** | ~10K headers/sec | With fast validation |

### 8.2 Resource Usage

| Resource | Usage | Notes |
|----------|-------|-------|
| **Memory (base)** | ~200MB | Without RandomX |
| **Memory (with RandomX)** | ~2.2GB | Dataset cached |
| **Disk Space** | ~100MB/year | Headers only |
| **Network Bandwidth** | ~10KB/s average | Header relay |
| **CPU (validation)** | ~5% single core | Normal operation |
| **CPU (mining)** | 100% all cores | RandomX mining |

### 8.3 Scalability

- **Headers/Year:** ~263,000 (at 2-minute blocks)
- **Growth Rate:** ~26MB/year (100 bytes/header)
- **10-Year Projection:** ~260MB storage
- **Max Throughput:** ~100 headers/second validated

---

## 9. Deployment Architecture

### 9.1 Node Types

#### Full Node
```yaml
Components:
  - Full header chain
  - Network connectivity
  - RPC server
  - Validation engine
Resources:
  - 2GB RAM (with RandomX)
  - 1GB disk
  - 10Mbps network
```

#### Light Client (Future)
```yaml
Components:
  - Header SPV
  - Checkpoint sync
  - Minimal validation
Resources:
  - 100MB RAM
  - 10MB disk
  - 1Mbps network
```

### 9.2 Network Topology

```
         ┌─────────────┐
         │  DNS Seeds  │
         └──────┬──────┘
                │
     ┌──────────┴──────────┐
     ▼                     ▼
┌─────────┐           ┌─────────┐
│Full Node│◄─────────►│Full Node│
└────┬────┘           └────┬────┘
     │                     │
     ▼                     ▼
┌─────────┐           ┌─────────┐
│Full Node│◄─────────►│Full Node│
└─────────┘           └─────────┘

Connections: 8 outbound, up to 125 inbound
Topology: Random mesh with preferential attachment
```

### 9.3 Configuration

#### Minimal Configuration
```ini
# coinbasechain.conf
listen=1
port=9590
rpcport=9591
maxconnections=125
```

#### Production Configuration
```ini
# coinbasechain.conf
# Network
listen=1
port=9590
maxconnections=125
timeout=60

# RPC
rpcport=9591
rpcuser=user
rpcpassword=pass
rpcallowip=127.0.0.1

# Performance
dbcache=100
maxorphanheaders=1000
maxheadersperpeer=2000

# Security
bantime=86400
maxuploadtarget=5000
```

---

## 10. Summary

CoinbaseChain implements a **robust, headers-only blockchain** with:

### Strengths
- ✅ **Simple consensus** - No transaction complexity
- ✅ **ASIC resistant** - RandomX memory-hard PoW
- ✅ **Responsive difficulty** - ASERT per-block adjustment
- ✅ **DoS protected** - Comprehensive attack mitigation
- ✅ **Bitcoin compatible** - Reuses proven patterns

### Trade-offs
- ❌ **No transactions** - Limited to specific use cases
- ❌ **Higher memory use** - RandomX requires 2GB
- ❌ **No smart contracts** - Simplified functionality

### Use Cases
- Timestamping services
- Proof of existence
- Decentralized ordering
- Simplified blockchain applications

The architecture achieves its design goals of simplicity, security, and efficiency while maintaining strategic compatibility with Bitcoin's proven design patterns.

---

## Related Documentation

- [PROTOCOL_SPECIFICATION.md](PROTOCOL_SPECIFICATION.md) - Detailed protocol specs
- [CHAINSTATE_AUDIT.md](CHAINSTATE_AUDIT.md) - Consensus implementation audit
- [PROTOCOL_DEVIATIONS.md](PROTOCOL_DEVIATIONS.md) - Differences from Bitcoin
- [PROTOCOL_COMPARISON.md](PROTOCOL_COMPARISON.md) - Side-by-side comparison

---

*Architecture Document v1.0.0*
*Last Updated: 2025-10-21*
*Generated by: Claude Code*