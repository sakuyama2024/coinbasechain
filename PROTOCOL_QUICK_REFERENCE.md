# Protocol Quick Reference

## At a Glance: Bitcoin vs CoinbaseChain

### 🔑 Key Identifiers
```
                Bitcoin              CoinbaseChain
Magic Bytes:    0xD9B4BEF9          0x554E4943 ("UNIC")
Port:           8333                9590
Version:        70015+              1
Network:        Bitcoin             CoinbaseChain
```

### ⏱️ Timing & Performance
```
                Bitcoin              CoinbaseChain
Block Time:     600 sec (10 min)    120 sec (2 min)
Difficulty:     Every 2016 blocks   Every block (ASERT)
Adjustment:     ~2 weeks            Immediate
Halving:        Every 210k blocks   None (no rewards)
```

### 📦 Data Structures
```
                Bitcoin              CoinbaseChain
Header Size:    80 bytes            100 bytes
Block Size:     1-4 MB              Headers only
Transaction:    Yes                 No
UTXO Set:       Yes                 No
```

### 🔧 Technical Details
```
                Bitcoin              CoinbaseChain
PoW:            SHA256d             RandomX
Memory Hard:    No                  Yes (2GB)
ASIC:           Yes                 Resistant
Checkpoints:    Yes                 No
BIP9:           Yes                 No
```

### 📨 P2P Messages

| Message | Bitcoin | CoinbaseChain | Notes |
|---------|:-------:|:-------------:|-------|
| version | ✅ | ✅ | Different values |
| verack | ✅ | ✅ | Identical |
| ping/pong | ✅ | ✅ | Identical |
| addr | ✅ | ✅ | Identical |
| inv | ✅ | ✅ | Headers only for us |
| getdata | ✅ | ✅ | Headers only for us |
| headers | ✅ | ✅ | 100 vs 80 bytes |
| getheaders | ✅ | ✅ | Identical |
| block | ✅ | ❌ | We don't support |
| tx | ✅ | ❌ | We don't support |
| mempool | ✅ | ❌ | We don't support |
| sendheaders | ✅ | ❌ | Not implemented |

### 🏗️ Block Header Layout

**Bitcoin (80 bytes):**
```
[version:4][prevhash:32][merkleroot:32][time:4][bits:4][nonce:4]
```

**CoinbaseChain (100 bytes):**
```
[version:4][prevhash:32][mineraddr:20][time:4][bits:4][nonce:4][randomx:20]
```

### ✅ What's the Same?

- Message header format (24 bytes)
- Serialization (little-endian, CompactSize)
- Checksum algorithm (double SHA256)
- Network address format (26 bytes)
- Time validation (MTP + 2hr future)
- Connection handshake (VERSION/VERACK)

### ❌ What's Different?

- Network identifiers (magic, ports)
- Block structure (headers-only)
- Proof of work algorithm
- Block timing (5x faster)
- Difficulty adjustment (per-block)
- No transaction support

### 📊 Compatibility Score

```
Protocol Structure:     75% compatible
Network Behavior:       50% compatible
Consensus Rules:        25% compatible
Overall:               50% compatible

Status: Intentionally separate networks
        with shared design patterns
```

### 🎯 Quick Checks

**Can they connect?** No (different magic/ports)
**Can they share code?** Yes (similar patterns)
**Can they validate each other?** No (different PoW/headers)
**Should they interact?** No (separate networks)

---

*Last Updated: 2025-10-21*