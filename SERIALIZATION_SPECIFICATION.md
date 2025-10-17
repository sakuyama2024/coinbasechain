# Block Header Serialization Specification

## Wire Format

All block headers are serialized to exactly **100 bytes** in the following format:

| Field          | Size (bytes) | Offset | Endianness     | Description                          |
|----------------|--------------|--------|----------------|--------------------------------------|
| nVersion       | 4            | 0      | Little-endian  | Block version number                 |
| hashPrevBlock  | 32           | 4      | Little-endian  | Hash of previous block header        |
| minerAddress   | 20           | 36     | Little-endian  | Miner's reward address (uint160)     |
| nTime          | 4            | 56     | Little-endian  | Unix timestamp                       |
| nBits          | 4            | 60     | Little-endian  | Difficulty target (compact format)   |
| nNonce         | 4            | 64     | Little-endian  | Nonce for proof-of-work              |
| hashRandomX    | 32           | 68     | Little-endian  | RandomX hash                         |
| **TOTAL**      | **100**      |        |                |                                      |

## Endianness Details

### Scalar Fields (nVersion, nTime, nBits, nNonce)
These 32-bit integers are serialized using **little-endian byte order**:
- Example: `nVersion = 1` → bytes: `01 00 00 00`
- Uses: `endian::WriteLE32()` / `endian::ReadLE32()`

### Hash Fields (hashPrevBlock, hashRandomX, minerAddress)
These multi-byte fields are serialized as **raw bytes in little-endian order**:
- `uint256` and `uint160` internally store bytes in little-endian format
- Serialization copies bytes directly (via `std::copy` from `begin()` to `end()`)
- No byte reversal on wire (Bitcoin Core compatible behavior)

**Important**: When displaying hashes via `GetHex()`, the bytes are **reversed for display**. This is a display-only convention - the internal storage and wire format remain little-endian.

### Example: hashPrevBlock
```
Internal storage (little-endian): [0x01, 0x00, ..., 0x00] (32 bytes)
Wire format (same):               [0x01, 0x00, ..., 0x00]
GetHex() display (reversed):      "0000...0001"
```

This matches Bitcoin Core's uint256 behavior exactly.

## Consensus-Critical Properties

### 1. Exact Size Enforcement
```cpp
// Serialize - runtime check
assert(data.size() == HEADER_SIZE);  // Must be exactly 100 bytes

// Deserialize - reject wrong sizes
if (size != HEADER_SIZE) return false;  // Must be exactly 100 bytes
assert(pos == HEADER_SIZE);  // Must consume exactly 100 bytes
```

### 2. Field Order
The field order is **consensus-critical** and must never change:
1. nVersion (4 bytes)
2. hashPrevBlock (32 bytes)
3. minerAddress (20 bytes) ← **Different from Bitcoin (uses hashMerkleRoot)**
4. nTime (4 bytes)
5. nBits (4 bytes)
6. nNonce (4 bytes)
7. hashRandomX (32 bytes) ← **Additional field not in Bitcoin**

### 3. Hash Calculation
The block hash is computed as:
```
hash = SHA256(SHA256(Serialize()))
```

With proper endianness handling:
1. Serialize header to 100 bytes (all fields little-endian)
2. Double SHA256 produces big-endian output
3. **Reverse bytes** before storing in uint256 (for correct little-endian display)

## Differences from Bitcoin Core

### Field Changes
1. **minerAddress** replaces Bitcoin's `hashMerkleRoot`
   - Bitcoin: 32-byte merkle root of transactions
   - Coinbase Chain: 20-byte miner address (headers-only chain)

2. **hashRandomX** is an additional field
   - Bitcoin: Does not have this field (80-byte headers)
   - Coinbase Chain: 32-byte RandomX hash (100-byte headers)

### Size
- Bitcoin Core block headers: **80 bytes**
- Coinbase Chain block headers: **100 bytes**

## Test Vectors

### Genesis Block (Example)
```
nVersion:      1
hashPrevBlock: 0000000000000000000000000000000000000000000000000000000000000000
minerAddress:  0000000000000000000000000000000000000000
nTime:         1234567890
nBits:         0x1d00ffff
nNonce:        0
hashRandomX:   0000000000000000000000000000000000000000000000000000000000000000

Serialized (hex):
01000000  (nVersion = 1, little-endian)
00000000000000000000000000000000000000000000000000000000000000000000  (hashPrevBlock)
00000000000000000000000000000000000000000000  (minerAddress)
d2029649  (nTime = 1234567890, little-endian)
ffff001d  (nBits = 0x1d00ffff, little-endian)
00000000  (nNonce = 0, little-endian)
00000000000000000000000000000000000000000000000000000000000000000000  (hashRandomX)
```

## Implementation Notes

### Safe Serialization Pattern
```cpp
std::vector<uint8_t> CBlockHeader::Serialize() const
{
    std::vector<uint8_t> data;
    data.reserve(HEADER_SIZE);
    
    // Scalars: use endian helpers
    data.resize(4);
    endian::WriteLE32(data.data(), static_cast<uint32_t>(nVersion));
    
    // Hashes: copy raw bytes (already little-endian internally)
    data.insert(data.end(), hashPrevBlock.begin(), hashPrevBlock.end());
    data.insert(data.end(), minerAddress.begin(), minerAddress.end());
    
    // ... more fields ...
    
    // Consensus-critical: verify exact size
    assert(data.size() == HEADER_SIZE);
    return data;
}
```

### Safe Deserialization Pattern
```cpp
bool CBlockHeader::Deserialize(const uint8_t* data, size_t size)
{
    // Consensus-critical: reject if size doesn't match exactly
    if (size != HEADER_SIZE) return false;
    
    size_t pos = 0;
    
    // Scalars: use endian helpers
    nVersion = static_cast<int32_t>(endian::ReadLE32(data + pos));
    pos += 4;
    
    // Hashes: copy raw bytes (already little-endian on wire)
    std::copy(data + pos, data + pos + 32, hashPrevBlock.begin());
    pos += 32;
    
    // ... more fields ...
    
    // Sanity check: verify we consumed exactly HEADER_SIZE
    assert(pos == HEADER_SIZE);
    return true;
}
```

## Version History

- **v1.0** (2025-10-15): Initial specification
  - 100-byte headers
  - Little-endian wire format
  - minerAddress (20 bytes) + hashRandomX (32 bytes)
  - Exact size enforcement

## References

- Bitcoin Core block header: https://developer.bitcoin.org/reference/block_chain.html#block-headers
- Coinbase Chain differs in: header size (100 vs 80 bytes), field layout (minerAddress vs hashMerkleRoot), RandomX PoW
