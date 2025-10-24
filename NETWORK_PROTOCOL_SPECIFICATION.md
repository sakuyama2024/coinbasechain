# CoinbaseChain Network Protocol Specification

**Version:** 1.0.0  
**Date:** 2025-10-24  
**Protocol Version:** 1   

---

## Table of Contents

1. [Protocol Overview](#1-protocol-overview)
2. [Wire Format Fundamentals](#2-wire-format-fundamentals)
3. [Message Framing](#3-message-framing)
4. [Connection Handshake](#4-connection-handshake)
5. [Message Types](#5-message-types)
6. [Block Headers](#6-block-headers)
7. [Network Constants](#7-network-constants)
8. [Security Limits](#8-security-limits)
9. [Bitcoin Deviations](#9-bitcoin-deviations)

---

## 1. Protocol Overview

### 1.1 Design Philosophy

CoinbaseChain implements a **headers-only blockchain** with a **Bitcoin-compatible P2P protocol**. The protocol maintains 98% compatibility with Bitcoin's wire format while diverging in three key areas:

1. **Block Structure:** 100-byte headers (vs 80-byte in Bitcoin) to accommodate RandomX PoW
2. **No Transactions:** Eliminates transaction, mempool, and UTXO messages
3. **Network Identity:** Unique magic bytes and ports for network separation

### 1.2 Protocol Goals

| Goal | Implementation |
|------|----------------|
| **Interoperability** | Bitcoin-compatible message framing and serialization |
| **Efficiency** | Headers-only sync (200KB per 2000 blocks vs 2MB+ in Bitcoin) |
| **Security** | Multi-layer DoS protection with proven Bitcoin limits |
| **Simplicity** | No transaction complexity (11 message types vs 27+ in Bitcoin) |

### 1.3 Network Identifiers

| Network | Magic Bytes | Port | Purpose |
|---------|-------------|------|---------|
| **Mainnet** | `0x554E4943` ("UNIC") | 9590 | Production network |
| **Testnet** | `0xA3F8D412` | 19590 | Public testing |
| **Regtest** | `0x4B7C2E91` | 29590 | Local development |

**Magic Bytes Encoding:**
```
Mainnet: 0x554E4943
  U    N    I    C
 0x55 0x4E 0x49 0x43
```

---

## 2. Wire Format Fundamentals

### 2.1 Endianness

| Field Type | Endianness | Example |
|------------|------------|---------|
| **Scalar integers** | Little-endian | `uint32_t`, `int64_t` |
| **Hash blobs** | As-stored (no swap) | `uint256`, `uint160` |
| **Port numbers** | Big-endian | Network byte order |
| **IPv6 addresses** | As-stored | Network byte order |

**Rationale:** Matches Bitcoin Core exactly for wire compatibility.

### 2.2 VarInt Encoding

Variable-length integer format (Bitcoin-compatible):

| Value Range | Encoding | Size |
|-------------|----------|------|
| `0 - 0xFC` | `value` | 1 byte |
| `0xFD - 0xFFFF` | `0xFD` + `uint16_le` | 3 bytes |
| `0x10000 - 0xFFFFFFFF` | `0xFE` + `uint32_le` | 5 bytes |
| `0x100000000+` | `0xFF` + `uint64_le` | 9 bytes |

**Examples:**

```
Value       Wire Bytes (hex)
------      ----------------
0           00
252         FC
253         FD FD 00
65535       FD FF FF
65536       FE 00 00 01 00
4294967296  FF 00 00 00 00 01 00 00 00
```

**Implementation:**
```cpp
size_t VarInt::encoded_size() const {
  if (value < 0xfd) return 1;
  if (value <= 0xffff) return 3;
  if (value <= 0xffffffff) return 5;
  return 9;
}
```

### 2.3 String Encoding

Strings are encoded as:
```
<VarInt: length> <bytes: data>
```

**Example:**
```
String: "/CoinbaseChain:1.0.0/"
Wire:   15 2F 43 6F 69 6E 62 61 73 65 43 68 61 69 6E 3A 31 2E 30 2E 30 2F
        ↑  ↑────────────────────────────────────────────────────────────
        │  String data (21 bytes)
        VarInt(21)
```

### 2.4 Network Address Format

**Without Timestamp (30 bytes):**
```
┌──────────────┬──────────────────────┬──────────┐
│  services    │      ip (IPv6)       │   port   │
│  (8 bytes)   │     (16 bytes)       │ (2 bytes)│
└──────────────┴──────────────────────┴──────────┘
```

**With Timestamp (34 bytes):**
```
┌──────────────┬──────────────┬──────────────────────┬──────────┐
│  timestamp   │  services    │      ip (IPv6)       │   port   │
│  (4 bytes)   │  (8 bytes)   │     (16 bytes)       │ (2 bytes)│
└──────────────┴──────────────┴──────────────────────┴──────────┘
```

**IPv4 Mapping:**
IPv4 addresses use IPv4-mapped IPv6 format (`::ffff:a.b.c.d`):
```cpp
// IPv4: 192.168.1.100 -> IPv6: ::ffff:192.168.1.100
NetworkAddress addr;
addr.ip = {0,0,0,0,0,0,0,0,0,0,0xff,0xff, 192,168,1,100};
```

**Service Flags:**
```cpp
enum ServiceFlags : uint64_t {
  NODE_NONE    = 0,
  NODE_NETWORK = (1 << 0),  // Can serve block headers
};
```

**DEVIATION:** In CoinbaseChain, `NODE_NETWORK` means "can serve headers" (not full blocks).

---

## 3. Message Framing

### 3.1 Message Structure

Every P2P message consists of:
```
┌─────────────────────┬──────────────────────┐
│   Message Header    │   Message Payload    │
│     (24 bytes)      │    (0-4MB bytes)     │
└─────────────────────┴──────────────────────┘
```

### 3.2 Message Header (24 bytes)

```
┌────────────┬─────────────┬─────────────┬─────────────┐
│   magic    │   command   │   length    │  checksum   │
│  (4 bytes) │ (12 bytes)  │  (4 bytes)  │  (4 bytes)  │
└────────────┴─────────────┴─────────────┴─────────────┘

Field      Type       Endian        Description
-----      ----       ------        -----------
magic      uint32_t   little        Network identifier
command    char[12]   -             Command name (null-padded)
length     uint32_t   little        Payload size in bytes
checksum   uint8_t[4] -             First 4 bytes of SHA256(SHA256(payload))
```

**Example (VERSION message):**
```
Hex: 43 49 4E 55  76 65 72 73 69 6F 6E 00 00 00 00 00  65 00 00 00  A2 F3 C1 D4
     ↑─magic────  ↑─command (version\0\0\0\0\0)────────  ↑─length───  ↑─checksum─
     0x554E4943   "version" null-padded to 12 bytes       101 bytes    SHA256²[:4]
```

### 3.3 Checksum Calculation

```cpp
std::array<uint8_t, 4> compute_checksum(const std::vector<uint8_t>& payload) {
    // Step 1: SHA-256 of payload
    auto hash1 = SHA256(payload);
    
    // Step 2: SHA-256 of hash1 (double-SHA256)
    auto hash2 = SHA256(hash1);
    
    // Step 3: Take first 4 bytes
    return {hash2[0], hash2[1], hash2[2], hash2[3]};
}
```

**Example:**
```
Payload:   01 00 00 00 (VERSION = 1)
SHA256:    6e340b9c... (32 bytes)
SHA256²:   d4c1f3a2... (32 bytes)
Checksum:  d4 c1 f3 a2 (first 4 bytes)
```

### 3.4 Command Names

Commands are **case-sensitive**, **null-padded** to 12 bytes:

| Command | Null-Padded Hex | Purpose |
|---------|-----------------|---------|
| `version` | `76 65 72 73 69 6F 6E 00 00 00 00 00` | Handshake |
| `verack` | `76 65 72 61 63 6B 00 00 00 00 00 00` | Acknowledge |
| `ping` | `70 69 6E 67 00 00 00 00 00 00 00 00` | Keepalive |
| `pong` | `70 6F 6E 67 00 00 00 00 00 00 00 00` | Keepalive response |
| `addr` | `61 64 64 72 00 00 00 00 00 00 00 00` | Peer addresses |
| `getaddr` | `67 65 74 61 64 64 72 00 00 00 00 00` | Request addresses |
| `inv` | `69 6E 76 00 00 00 00 00 00 00 00 00` | Announce blocks |
| `getdata` | `67 65 74 64 61 74 61 00 00 00 00 00` | Request data |
| `notfound` | `6E 6F 74 66 6F 75 6E 64 00 00 00 00` | Data unavailable |
| `getheaders` | `67 65 74 68 65 61 64 65 72 73 00 00` | Request headers |
| `headers` | `68 65 61 64 65 72 73 00 00 00 00 00` | Deliver headers |

---

## 4. Connection Handshake

### 4.1 Handshake Sequence

**Outbound Connection (Node A → Node B):**

```
Node A (Outbound)                    Node B (Inbound)
─────────────────                    ────────────────
 
[TCP CONNECT] ──────────────────────►
                                     [ACCEPT]
                                     [Wait for VERSION]
 
[Send VERSION] ─────────────────────►
                                     [Validate VERSION]
                                     [Send VERACK]
◄────────────────────────────────── [VERACK]
                                     [Send own VERSION]
◄────────────────────────────────── [VERSION]
 
[Send VERACK] ─────────────────────►
                                     [Connection READY]
[Connection READY]
 
[Begin normal message flow]
```

### 4.2 Timing Requirements

| Timeout | Value | Consequence |
|---------|-------|-------------|
| **Handshake timeout** | 60 seconds | Disconnect if VERSION/VERACK incomplete |
| **Ping interval** | 120 seconds | Send PING every 2 minutes |
| **Ping timeout** | 1200 seconds | Disconnect if no PONG in 20 minutes |
| **Inactivity timeout** | 1200 seconds | Disconnect if no messages in 20 minutes |

**Bitcoin-compatible:** All timeouts match Bitcoin Core exactly.

### 4.3 Self-Connection Detection

Nodes prevent connecting to themselves using random nonces:

```cpp
// Node A sends VERSION
uint64_t local_nonce = generate_random_nonce();  // e.g., 0x123456789ABCDEF0
version.nonce = local_nonce;

// Node A receives VERSION from Node B
if (remote_version.nonce == local_nonce) {
    // Self-connection detected! Disconnect.
    disconnect();
}
```


---

## 5. Message Types

### 5.1 VERSION Message

**Command:** `version`  
**Typical Size:** ~100 bytes  

**Payload Structure:**
```
┌─────────────┬─────────────┬─────────────┬─────────────┬─────────────┬─────────────┬─────────────┬─────────────┐
│  version    │  services   │ timestamp   │  addr_recv  │  addr_from  │    nonce    │ user_agent  │start_height │
│  (4 bytes)  │  (8 bytes)  │  (8 bytes)  │  (30 bytes) │  (30 bytes) │  (8 bytes)  │  (varint+)  │  (4 bytes)  │
└─────────────┴─────────────┴─────────────┴─────────────┴─────────────┴─────────────┴─────────────┴─────────────┘
```

**Field Descriptions:**

| Field | Type | Description |
|-------|------|-------------|
| `version` | int32 | Protocol version (1 for CoinbaseChain) |
| `services` | uint64 | Service flags (NODE_NETWORK = 0x01) |
| `timestamp` | int64 | Current Unix timestamp |
| `addr_recv` | NetAddr | Receiving node's address (no timestamp) |
| `addr_from` | NetAddr | Sending node's address (no timestamp) |
| `nonce` | uint64 | Random nonce for self-connection detection |
| `user_agent` | string | Node software identifier |
| `start_height` | int32 | Sender's blockchain height |

**Wire Format Example:**
```
01 00 00 00                          // version = 1
01 00 00 00 00 00 00 00              // services = NODE_NETWORK
3F 2A 5E 65 00 00 00 00              // timestamp = 1700000000
00 00 00 00 00 00 00 00              // addr_recv.services = 0
00 00 00 00 00 00 00 00 00 00 FF FF  // addr_recv.ip (IPv4-mapped)
C0 A8 01 64                          // addr_recv.ip = 192.168.1.100
25 76                                // addr_recv.port = 9590 (big-endian)
00 00 00 00 00 00 00 00              // addr_from.services = 0
00 00 00 00 00 00 00 00 00 00 FF FF  // addr_from.ip (empty)
00 00 00 00                          // addr_from.ip = 0.0.0.0
00 00                                // addr_from.port = 0
F0 DE BC 9A 78 56 34 12              // nonce = 0x123456789ABCDEF0
15                                   // user_agent length = 21
2F 43 6F 69 6E 62 61 73 65 43 68 61  // "/CoinbaseChain:1.0.0/"
69 6E 3A 31 2E 30 2E 30 2F
64 00 00 00                          // start_height = 100
```

**Security Constraints:**
- `user_agent` length ≤ 256 bytes 
- `version` must be ≥ 1 (MIN_PROTOCOL_VERSION)
- `nonce` must be non-zero for outbound connections

### 5.2 VERACK Message

**Command:** `verack`  
**Payload:** Empty (0 bytes)  

Acknowledges receipt of VERSION message. No payload.

**Complete Message:**
```
43 49 4E 55                          // magic = 0x554E4943
76 65 72 61 63 6B 00 00 00 00 00 00  // command = "verack"
00 00 00 00                          // length = 0
5D F6 E0 E2                          // checksum = SHA256²(empty)[:4]
```

### 5.3 PING Message

**Command:** `ping`  
**Payload Size:** 8 bytes  

**Structure:**
```
┌─────────────┐
│    nonce    │
│  (8 bytes)  │
└─────────────┘
```

**Example:**
```
F0 DE BC 9A 78 56 34 12              // nonce = 0x123456789ABCDEF0
```

### 5.4 PONG Message

**Command:** `pong`  
**Payload Size:** 8 bytes  

**Structure:** Identical to PING (echoes nonce from received PING).

```
┌─────────────┐
│    nonce    │
│  (8 bytes)  │
└─────────────┘
```

### 5.5 ADDR Message

**Command:** `addr`  
**Max Size:** ~34KB (1000 addresses)  

**Payload Structure:**
```
┌─────────────┬───────────────────┬───────────────────┬─────┐
│   count     │   addr[0]         │   addr[1]         │ ... │
│  (varint)   │ (34 bytes each)   │ (34 bytes each)   │     │
└─────────────┴───────────────────┴───────────────────┴─────┘
```

**Per-Address Structure (34 bytes):**
```
┌─────────────┬─────────────┬──────────────────────┬──────────┐
│  timestamp  │  services   │      ip (IPv6)       │   port   │
│  (4 bytes)  │  (8 bytes)  │     (16 bytes)       │ (2 bytes)│
└─────────────┴─────────────┴──────────────────────┴──────────┘
```

**Constraints:**
- Maximum addresses per message: 1000 (MAX_ADDR_SIZE)
- Timestamp: Unix time when address was last seen
- Incremental allocation (5MB batches) to prevent DoS

### 5.6 GETADDR Message

**Command:** `getaddr`  
**Payload:** Empty (0 bytes)  

Requests peer addresses. Peer responds with ADDR message containing up to 1000 addresses.

**Rate Limiting:**
- Nodes send ADDR response **once per connection**
- Duplicate GETADDR requests are ignored (DoS protection)

### 5.7 INV Message

**Command:** `inv`  
**Max Size:** ~1.6MB (50,000 inventory items)  

**Payload Structure:**
```
┌─────────────┬───────────────────┬───────────────────┬─────┐
│   count     │   inv[0]          │   inv[1]          │ ... │
│  (varint)   │ (36 bytes each)   │ (36 bytes each)   │     │
└─────────────┴───────────────────┴───────────────────┴─────┘
```

**Inventory Vector (36 bytes):**
```
┌─────────────┬─────────────────────────────────────┐
│    type     │              hash                   │
│  (4 bytes)  │           (32 bytes)                │
└─────────────┴─────────────────────────────────────┘
```

**Inventory Types:**
```cpp
enum InventoryType : uint32_t {
  ERROR     = 0,  // Not used
  MSG_BLOCK = 2,  // Block header announcement
};
```

**DEVIATION:** Only `MSG_BLOCK` is used (no transactions in headers-only chain).

**Constraints:**
- Maximum inventory items: 50,000 (MAX_INV_SIZE)
- Type must be `MSG_BLOCK` (type=2)

### 5.8 GETDATA Message

**Command:** `getdata`  
**Structure:** Identical to INV  

Requests inventory items announced via INV. In CoinbaseChain, triggers peer to send HEADERS message.

**Bitcoin Difference:**
- Bitcoin: GETDATA → BLOCK message (full block with transactions)
- CoinbaseChain: GETDATA → GETHEADERS → HEADERS (headers only)

### 5.9 NOTFOUND Message

**Command:** `notfound`  
**Structure:** Identical to INV  

Indicates requested inventory items are not available.

### 5.10 GETHEADERS Message

**Command:** `getheaders`  
**Typical Size:** ~1KB  

**Payload Structure:**
```
┌─────────────┬─────────────┬───────────────────┬─────┬─────────────┐
│  version    │   count     │   locator[0]      │ ... │  hash_stop  │
│  (4 bytes)  │  (varint)   │ (32 bytes each)   │     │  (32 bytes) │
└─────────────┴─────────────┴───────────────────┴─────┴─────────────┘
```

**Field Descriptions:**

| Field | Type | Description |
|-------|------|-------------|
| `version` | uint32 | Protocol version (1) |
| `count` | varint | Number of locator hashes (max 101) |
| `locator[]` | hash[count] | Block locator hashes (see below) |
| `hash_stop` | hash | Stop after this hash (0x00...00 = don't stop) |

**Block Locator Algorithm:**

The locator is an exponentially-spaced list of block hashes to help peers find the last common block:

```
Height Sequence (for tip at height 1000):
[1000, 999, 998, 996, 992, 984, 968, 936, 872, 744, 488, 0]
  ↑     ↑    ↑    ↑    ↑    ↑    ↑    ↑    ↑    ↑    ↑    ↑
 tip   -1   -2   -4   -8   -16  -32  -64  -128 -256 -512  genesis

Step size doubles after first 10 entries: 1,1,1,2,4,8,16,32,64,128,256,512
```

**Implementation:**
```cpp
std::vector<uint256> GetLocator(const CBlockIndex* pindex) {
    std::vector<uint256> locator;
    int step = 1;
    
    while (pindex) {
        locator.push_back(pindex->GetBlockHash());
        
        // Go back exponentially
        for (int i = 0; pindex && i < step; i++) {
            pindex = pindex->pprev;
        }
        
        // Double step after first 10 entries
        if (locator.size() > 10) {
            step *= 2;
        }
    }
    
    // Limit to 101 entries
    if (locator.size() > 101) {
        locator.resize(101);
    }
    
    return locator;
}
```

**Constraints:**
- Maximum locator size: 101 hashes (MAX_LOCATOR_SZ)
- Enforced to prevent CPU exhaustion from expensive FindFork() operations

### 5.11 HEADERS Message

**Command:** `headers`  
**Max Size:** ~200KB (2000 headers)  

**Payload Structure:**
```
┌─────────────┬───────────────────┬───────────────────┬─────┐
│   count     │   header[0]       │   header[1]       │ ... │
│  (varint)   │ (100 bytes each)  │ (100 bytes each)  │     │
└─────────────┴───────────────────┴───────────────────┴─────┘
```

**Per-Header Structure:** See [Section 6: Block Headers](#6-block-headers)

**Constraints:**
- Maximum headers per message: 2000 (MAX_HEADERS_SIZE)
- Headers must be continuous (`header[i].hashPrevBlock == header[i-1].GetHash()`)
- Incremental allocation (5MB batches) to prevent DoS

**Sync Behavior:**
- Full batch (2000 headers) → Peer likely has more → Send another GETHEADERS
- Partial batch (<2000 headers) → Likely synced → Stop requesting

---

## 6. Block Headers

### 6.1 Header Structure

CoinbaseChain headers are **100 bytes** (vs 80 bytes in Bitcoin).

```
┌─────────────┬─────────────┬─────────────┬─────────────┬─────────────┬─────────────┬─────────────┐
│  nVersion   │hashPrevBlock│minerAddress │    nTime    │    nBits    │   nNonce    │hashRandomX  │
│  (4 bytes)  │ (32 bytes)  │ (20 bytes)  │  (4 bytes)  │  (4 bytes)  │  (4 bytes)  │ (32 bytes)  │
└─────────────┴─────────────┴─────────────┴─────────────┴─────────────┴─────────────┴─────────────┘
  Offset: 0       4            36            56            60            64            68
```

**Field Descriptions:**

| Field | Type | Endian | Description |
|-------|------|--------|-------------|
| `nVersion` | int32 | little | Block version (currently 1) |
| `hashPrevBlock` | uint256 | as-stored | Hash of previous block header |
| `minerAddress` | uint160 | as-stored | Miner's address (replaces merkleRoot) |
| `nTime` | uint32 | little | Block timestamp (Unix time) |
| `nBits` | uint32 | little | Difficulty target (compact format) |
| `nNonce` | uint32 | little | Nonce for PoW |
| `hashRandomX` | uint256 | as-stored | RandomX hash commitment |



### 6.2 Wire Format Example

```
Genesis Block Header (100 bytes):
01 00 00 00                          // nVersion = 1
00 00 00 00 00 00 00 00 00 00 00 00  // hashPrevBlock = 0 (genesis)
00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00  // minerAddress = 0
00 00 00 00 00 00 00 00
00 00 00 00                          // nTime = 0
FF FF 00 1D                          // nBits (difficulty)
00 00 00 00                          // nNonce = 0
00 00 00 00 00 00 00 00 00 00 00 00  // hashRandomX = 0
00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
```

### 6.3 Header Serialization

**C++ Implementation:**
```cpp
std::array<uint8_t, 100> CBlockHeader::SerializeFixed() const noexcept {
    std::array<uint8_t, 100> buffer;
    size_t pos = 0;
    
    // Scalar fields (little-endian)
    WriteLE32(buffer.data() + pos, nVersion); pos += 4;
    
    // Hash blobs (as-stored, no endian swap)
    std::memcpy(buffer.data() + pos, hashPrevBlock.begin(), 32); pos += 32;
    std::memcpy(buffer.data() + pos, minerAddress.begin(), 20); pos += 20;
    
    // Scalar fields (little-endian)
    WriteLE32(buffer.data() + pos, nTime); pos += 4;
    WriteLE32(buffer.data() + pos, nBits); pos += 4;
    WriteLE32(buffer.data() + pos, nNonce); pos += 4;
    
    // Hash blob (as-stored, no endian swap)
    std::memcpy(buffer.data() + pos, hashRandomX.begin(), 32);
    
    return buffer;
}
```

### 6.4 Header Hashing

Block hash is computed as **SHA-256 of the 100-byte serialized header**:

```cpp
uint256 CBlockHeader::GetHash() const noexcept {
    auto bytes = SerializeFixed();
    return SHA256(bytes.data(), bytes.size());
}
```

**Critical Constraint:**
```cpp
static_assert(sizeof(CBlockHeader) == 100,
              "CBlockHeader must be tightly packed (no padding)");
```


---

## 7. Network Constants

### 7.1 Protocol Versions

```cpp
constexpr uint32_t PROTOCOL_VERSION     = 1;  // Current version
constexpr uint32_t MIN_PROTOCOL_VERSION = 1;  // Minimum supported
```


### 7.2 Service Flags

```cpp
enum ServiceFlags : uint64_t {
  NODE_NONE    = 0,
  NODE_NETWORK = (1 << 0),  // Can serve block headers
};
```

**DEVIATION:** `NODE_NETWORK` means "headers-only service" (not full blocks).

### 7.3 Timeouts (seconds)

| Constant | Value | Purpose |
|----------|-------|---------|
| `VERSION_HANDSHAKE_TIMEOUT_SEC` | 60 | Complete VERSION/VERACK |
| `PING_INTERVAL_SEC` | 120 | Send PING every 2 min |
| `PING_TIMEOUT_SEC` | 1200 | Disconnect after 20 min |
| `INACTIVITY_TIMEOUT_SEC` | 1200 | Disconnect if no messages |

**Bitcoin-compatible:** All values match Bitcoin Core.

### 7.4 Connection Limits

```cpp
constexpr unsigned int DEFAULT_MAX_OUTBOUND_CONNECTIONS = 8;
constexpr unsigned int DEFAULT_MAX_INBOUND_CONNECTIONS  = 125;
```

**Bitcoin-compatible:** Standard P2P connection limits.

---

## 8. Security Limits

### 8.1 Message Size Limits

| Limit | Value | Purpose |
|-------|-------|---------|
| `MAX_PROTOCOL_MESSAGE_LENGTH` | 4 MB | Single message max |
| `DEFAULT_RECV_FLOOD_SIZE` | 5 MB | Per-peer receive buffer |
| `MAX_SIZE` | 32 MB | Serialized object max |
| `MAX_VECTOR_ALLOCATE` | 5 MB | Incremental allocation batch |

**Bitcoin-compatible:** All limits match Bitcoin Core.

**Enforcement:**

```cpp
// Reject oversized messages
if (header.length > MAX_PROTOCOL_MESSAGE_LENGTH) {
    disconnect_peer("oversized message");
    return false;
}

// Flood protection
if (recv_buffer.size() > DEFAULT_RECV_FLOOD_SIZE) {
    disconnect_peer("receive flood");
    return false;
}
```

### 8.2 Message Count Limits

| Limit | Value | Message Types |
|-------|-------|---------------|
| `MAX_HEADERS_SIZE` | 2000 | HEADERS |
| `MAX_INV_SIZE` | 50000 | INV, GETDATA, NOTFOUND |
| `MAX_ADDR_SIZE` | 1000 | ADDR |
| `MAX_LOCATOR_SZ` | 101 | GETHEADERS |

**Bitcoin-compatible:** All limits match Bitcoin Core.

### 8.3 Orphan Header Limits

```cpp
constexpr size_t MAX_ORPHAN_HEADERS          = 1000;  // Global limit
constexpr size_t MAX_ORPHAN_HEADERS_PER_PEER = 50;    // Per-peer limit
constexpr int64_t ORPHAN_HEADER_EXPIRE_TIME  = 600;   // 10 minutes
```

**DoS Protection:**
- Global limit prevents memory exhaustion
- Per-peer limit prevents Sybil attacks
- Expiry prevents stale data accumulation

### 8.4 String Length Limits

```cpp
constexpr size_t MAX_SUBVERSION_LENGTH = 256;  // user_agent in VERSION
```

**Enforcement:**
```cpp
if (user_agent.length() > MAX_SUBVERSION_LENGTH) {
    return false;  // Reject VERSION message
}
```

### 8.5 Incremental Allocation

All vector deserializations use **incremental allocation** to prevent memory exhaustion:

```cpp
// BAD: Attacker sends count=1000000000, causing 8GB allocation
addresses.reserve(count);

// GOOD: Allocate in 5MB batches
constexpr size_t batch_size = MAX_VECTOR_ALLOCATE / sizeof(Address);
for (uint64_t i = 0; i < count; ++i) {
    if (addresses.size() >= allocated) {
        allocated = std::min(count, allocated + batch_size);
        addresses.reserve(allocated);
    }
    addresses.push_back(read_address());
}
```

---

## 9. Bitcoin Deviations

### 9.1 Network Identity

| Aspect | CoinbaseChain | Bitcoin | Reason |
|--------|---------------|---------|--------|
| **Magic bytes** | `0x554E4943` | `0xD9B4BEF9` | Prevent cross-chain connections |
| **Mainnet port** | 9590 | 8333 | Network separation |
| **Protocol version** | 1 | 70015+ | Independent versioning |



### 9.2 Block Structure

| Field | CoinbaseChain | Bitcoin | Reason |
|-------|---------------|---------|--------|
| **Header size** | 100 bytes | 80 bytes | RandomX PoW |
| **merkleRoot** | N/A (minerAddress) | 32 bytes | No transactions |
| **hashRandomX** | 32 bytes | N/A | RandomX commitment |

**Fundamental Difference:** Headers-only design eliminates transaction complexity.

### 9.3 Message Support

**Supported Messages:**
- VERSION, VERACK (handshake)
- PING, PONG (keepalive)
- ADDR, GETADDR (peer discovery)
- INV, GETDATA, NOTFOUND (block announcements)
- GETHEADERS, HEADERS (header sync)

**Unsupported Messages:**
- TX (no transactions)
- BLOCK (no full blocks)
- MEMPOOL (no mempool)
- GETBLOCKS (obsolete, use GETHEADERS)
- FILTERLOAD, FILTERADD, FILTERCLEAR (no bloom filters)
- CMPCTBLOCK, GETBLOCKTXN, BLOCKTXN (no compact blocks)
- SENDHEADERS (not implemented)


### 9.4 Service Flags

| Flag | CoinbaseChain Meaning | Bitcoin Meaning |
|------|----------------------|-----------------|
| `NODE_NETWORK` | Can serve block headers | Can serve full blocks |

**Semantic Difference:** Same flag, different capability.

### 9.5 Inventory Types

| Type | CoinbaseChain | Bitcoin |
|------|---------------|---------|
| `MSG_TX` | Not used | Transaction |
| `MSG_BLOCK` | Block hash only | Full block |
| `MSG_FILTERED_BLOCK` | Not used | Filtered block |
| `MSG_CMPCT_BLOCK` | Not used | Compact block |

**Difference:** Only `MSG_BLOCK` (type=2) is used for header announcements.


---

## Appendix A: Wire Format Examples

### A.1 Complete Handshake

**Step 1: Outbound peer sends VERSION**
```
Header (24 bytes):
43 49 4E 55 76 65 72 73 69 6F 6E 00 00 00 00 00  65 00 00 00  A2 F3 C1 D4

Payload (101 bytes):
01 00 00 00                          // version = 1
01 00 00 00 00 00 00 00              // services = NODE_NETWORK
3F 2A 5E 65 00 00 00 00              // timestamp
[30 bytes: addr_recv]
[30 bytes: addr_from]
F0 DE BC 9A 78 56 34 12              // nonce
15 2F 43 6F 69 6E 62 61 73 65 43 68  // user_agent = "/CoinbaseChain:1.0.0/"
61 69 6E 3A 31 2E 30 2E 30 2F
64 00 00 00                          // start_height = 100
```

**Step 2: Inbound peer sends VERACK**
```
43 49 4E 55 76 65 72 61 63 6B 00 00 00 00 00 00  00 00 00 00  5D F6 E0 E2
```

**Step 3: Inbound peer sends VERSION** (similar to Step 1)

**Step 4: Outbound peer sends VERACK** (similar to Step 2)

### A.2 Header Synchronization

**Peer A sends GETHEADERS**
```
Payload:
01 00 00 00                          // version = 1
05                                   // count = 5 locator hashes
[32 bytes: hash at height 1000]
[32 bytes: hash at height 999]
[32 bytes: hash at height 998]
[32 bytes: hash at height 996]
[32 bytes: hash at height 992]
00 00 00 00 00 00 00 00 00 00 00 00  // hash_stop = 0 (don't stop)
00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00
```

**Peer B responds with HEADERS**
```
Payload:
FD D0 07                             // count = 2000 (varint)
[100 bytes: header 1001]
[100 bytes: header 1002]
...
[100 bytes: header 3000]
Total: ~200KB
```

---

## Appendix B: Test Vectors

### B.1 VarInt Encoding

| Value | Expected Encoding |
|-------|-------------------|
| 0 | `00` |
| 252 | `FC` |
| 253 | `FD FD 00` |
| 65535 | `FD FF FF` |
| 65536 | `FE 00 00 01 00` |
| 2^32-1 | `FE FF FF FF FF` |
| 2^32 | `FF 00 00 00 00 01 00 00 00` |

### B.2 Network Address

**IPv4: 192.168.1.100:9590**
```
00 00 00 00 00 00 00 00              // services = 0
00 00 00 00 00 00 00 00 00 00 FF FF  // IPv4-mapped prefix
C0 A8 01 64                          // 192.168.1.100
25 76                                // port 9590 (big-endian)
```

### B.3 Checksum

**Empty Payload:**
```
SHA256(empty)  = e3b0c442...
SHA256²(empty) = 5df6e0e2...
Checksum       = 5D F6 E0 E2
```

**Payload: [0x01, 0x00, 0x00, 0x00]:**
```
SHA256         = 6e340b9c...
SHA256²        = d4c1f3a2...
Checksum       = D4 C1 F3 A2
```

---

## Appendix C: Implementation Notes

### C.1 Endianness Handling

```cpp
// Little-endian write
void WriteLE32(uint8_t* buf, uint32_t value) {
    buf[0] = value & 0xff;
    buf[1] = (value >> 8) & 0xff;
    buf[2] = (value >> 16) & 0xff;
    buf[3] = (value >> 24) & 0xff;
}

// Big-endian write (for ports)
void WriteBE16(uint8_t* buf, uint16_t value) {
    buf[0] = (value >> 8) & 0xff;
    buf[1] = value & 0xff;
}
```

### C.2 Message Parsing

```cpp
bool parse_message(const uint8_t* data, size_t size) {
    // Step 1: Parse header (24 bytes)
    if (size < 24) return false;
    MessageHeader header;
    deserialize_header(data, 24, header);
    
    // Step 2: Validate magic
    if (header.magic != MAINNET_MAGIC) {
        disconnect("wrong network");
        return false;
    }
    
    // Step 3: Validate length
    if (header.length > MAX_PROTOCOL_MESSAGE_LENGTH) {
        disconnect("oversized message");
        return false;
    }
    
    // Step 4: Extract payload
    const uint8_t* payload = data + 24;
    size_t payload_size = header.length;
    
    // Step 5: Verify checksum
    auto expected_checksum = compute_checksum(payload, payload_size);
    if (memcmp(expected_checksum.data(), header.checksum.data(), 4) != 0) {
        disconnect("checksum mismatch");
        return false;
    }
    
    // Step 6: Dispatch to handler
    std::string command = header.get_command();
    return handle_message(command, payload, payload_size);
}
```

### C.3 Security Best Practices

1. **Always validate message length** before parsing payload
2. **Use incremental allocation** for vectors to prevent memory exhaustion
3. **Enforce string length limits** (MAX_SUBVERSION_LENGTH = 256)
4. **Check receive buffer size** before appending (DEFAULT_RECV_FLOOD_SIZE = 5MB)
5. **Validate nonce uniqueness** for self-connection detection
6. **Verify checksum** before processing message

---

## Revision History

| Version | Date | Changes |
|---------|------|---------|
| 1.0.0 | 2025-10-24 | Initial specification |

---

**END OF SPECIFICATION**
