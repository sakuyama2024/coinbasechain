# Architecture Summary

## 🏗️ System at a Glance

```
┌──────────────────────────────────────┐
│         CoinbaseChain Node           │
├──────────────────────────────────────┤
│                                      │
│  Apps  ──► RPC ──► Blockchain ──┐   │
│                                  ▼   │
│         Consensus ◄─► Network P2P    │
│                                      │
└──────────────────────────────────────┘
```

## 📊 Key Metrics

| Metric | Value | Notes |
|--------|-------|-------|
| **Block Time** | 2 minutes | 5x faster than Bitcoin |
| **Header Size** | 100 bytes | 20 bytes larger than Bitcoin |
| **Memory Usage** | ~2.2 GB | RandomX dataset |
| **Disk Growth** | ~26 MB/year | Headers only |
| **Connections** | 8 out, 125 in | Standard P2P limits |
| **Sync Speed** | ~10K headers/sec | With fast validation |

## 🔧 Core Components

### Network Layer
```
PeerManager (manages connections)
    ├── Peer[] (individual connections)
    ├── AddrManager (address book)
    └── HeaderSyncManager (synchronization)
```

### Consensus Layer
```
ChainstateManager (coordinates validation)
    ├── Validation Rules (CheckBlockHeader)
    ├── ASERT Difficulty (per-block adjustment)
    └── RandomX PoW (memory-hard proof)
```

## 🔄 Data Flow

```
1. RECEIVE Header ──► 2. VALIDATE
                           │
                     ┌─────┴─────┐
                     │           │
                 3. ACCEPT   4. REJECT
                     │           │
                5. UPDATE    6. BAN
                   Chain      Peer
```

## 🛡️ Security Features

| Feature | Protection Against | Implementation |
|---------|-------------------|----------------|
| **Work Threshold** | DoS attacks | Min chainwork required |
| **Orphan Limits** | Memory exhaustion | 1000 total, 50/peer |
| **Connection Limits** | Sybil attacks | Max 125 inbound |
| **Ban Scoring** | Misbehavior | 100 points = ban |
| **RandomX PoW** | ASIC mining | Memory-hard algorithm |
| **MTP Enforcement** | Time attacks | 11-block median |

## 📦 Message Types

### Essential Messages
- `version/verack` - Handshake
- `headers` - Block headers (100 bytes each)
- `getheaders` - Request headers
- `inv/getdata` - Inventory management
- `ping/pong` - Keepalive

### Not Supported (Headers-Only)
- ❌ `block` - No full blocks
- ❌ `tx` - No transactions
- ❌ `mempool` - No transaction pool

## 🎯 Design Decisions

### What We Keep from Bitcoin
✅ Message framing (24-byte headers)
✅ Serialization format
✅ P2P connection patterns
✅ Time validation rules
✅ Network address format

### What We Changed
❌ Block structure (headers-only)
❌ PoW algorithm (RandomX vs SHA256)
❌ Difficulty adjustment (ASERT vs fixed)
❌ Block time (2 min vs 10 min)
❌ Network identifiers (magic/ports)

## 🚀 Performance Profile

```
Operation               Time        CPU
─────────────────────────────────────────
Header Pre-check        ~1ms        Low
Full Validation        ~50ms       Medium
Chain Update           ~10ms        Low
Network I/O            <1ms         Low
RandomX Mining         N/A         100%
```

## 📈 Scalability

```
Time Period    Headers    Storage
──────────────────────────────────
1 Day            720       72 KB
1 Month       21,600      2.1 MB
1 Year       262,800       26 MB
10 Years   2,628,000      260 MB
```

## 🔌 Integration Points

### RPC Interface
```javascript
// Example RPC calls
getblockcount()      // Current height
getbestblockhash()   // Tip hash
getblockheader(hash) // Header data
getpeerinfo()        // Connected peers
```

### Configuration
```ini
# Essential settings
port=9590            # P2P port
rpcport=9591        # RPC port
maxconnections=125   # Connection limit
datadir=~/.coinbase  # Data directory
```

## 📚 Key Files

### Implementation
- `src/network/` - P2P networking
- `src/chain/` - Consensus rules
- `include/` - Headers and interfaces

### Documentation
- `ARCHITECTURE.md` - Full details
- `PROTOCOL_SPECIFICATION.md` - Wire protocol
- `CHAINSTATE_AUDIT.md` - Consensus audit

## ✅ Production Readiness

| Component | Status | Tests | Audit |
|-----------|--------|-------|-------|
| Network Protocol | ✅ Ready | 357 passing | Complete |
| Consensus Rules | ✅ Ready | 4,806 assertions | Complete |
| DoS Protection | ✅ Ready | Stress tested | Complete |
| RandomX PoW | ✅ Ready | Validated | Complete |
| ASERT Difficulty | ✅ Ready | Tested | Complete |

## 🎨 Architecture Principles

1. **Simplicity** - Headers-only removes complexity
2. **Security** - Defense in depth against attacks
3. **Efficiency** - Optimized validation pipeline
4. **Compatibility** - Strategic Bitcoin pattern reuse
5. **Modularity** - Clean component separation

---

*Quick reference for CoinbaseChain architecture*
*Full details in [ARCHITECTURE.md](ARCHITECTURE.md)*