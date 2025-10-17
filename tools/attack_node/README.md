# Attack Node - DoS Protection Testing Tool

A C++ utility for testing DoS (Denial of Service) protection mechanisms in the CoinbaseChain P2P network.

**⚠️  WARNING: This tool sends malicious P2P messages. Only use on private test networks!**

## Purpose

This tool allows you to test the node's DoS protection by sending various types of malicious P2P messages:

1. **Invalid PoW Headers** - Headers with invalid proof-of-work (should trigger instant disconnect, score=100)
2. **Oversized Messages** - Headers messages exceeding MAX_HEADERS_COUNT (should trigger +20 misbehavior score)
3. **Non-Continuous Headers** - Headers that don't properly chain together (should trigger +20 misbehavior score)
4. **Spam Attacks** - Repeated violations to test score accumulation and disconnection

## Building

The tool is built automatically with the main project:

```bash
cmake -S . -B build
cmake --build build --target attack_node
```

Binary location: `build/bin/attack_node`

## Usage

```bash
# Show help
./build/bin/attack_node --help

# Test invalid PoW attack
./build/bin/attack_node --attack invalid-pow

# Test oversized headers attack
./build/bin/attack_node --attack oversized

# Test non-continuous headers
./build/bin/attack_node --attack non-continuous

# Test spam attack (5x non-continuous)
./build/bin/attack_node --attack spam-continuous

# Run all attacks
./build/bin/attack_node --attack all

# Target a specific host/port
./build/bin/attack_node --host 192.168.1.100 --port 18444 --attack all
```

## Options

- `--host <host>` - Target host (default: 127.0.0.1)
- `--port <port>` - Target port (default: 18444)
- `--attack <type>` - Attack type to perform
- `--help` - Show help message

## Testing DoS Protection

### Setup

1. Start a regtest node:
```bash
./build/bin/coinbasechain --regtest --datadir=/tmp/test-node --listen --port=18444
```

2. Run attack tool:
```bash
./build/bin/attack_node --attack non-continuous
```

3. Check node logs for misbehavior scoring:
```bash
tail -f /tmp/test-node/debug.log | grep -i misbehaving
```

### Expected Results

**Invalid PoW:**
- Misbehavior score: +100
- Result: Instant disconnect

**Oversized Headers:**
- Misbehavior score: +20
- Result: Disconnect after 5 violations (5×20=100)

**Non-Continuous Headers:**
- Misbehavior score: +20
- Result: Disconnect after 5 violations (5×20=100)

**Spam Attack:**
- Sends 5 non-continuous headers messages
- Accumulated score: 100
- Result: Peer disconnected and discouraged

### Checking Misbehavior Scores

Use the `getpeerinfo` RPC to check peer misbehavior scores:

```bash
./build/bin/coinbasechain-cli --datadir=/tmp/test-node getpeerinfo
```

Output includes:
- `misbehavior_score`: Current score for the peer
- `should_disconnect`: Whether peer should be disconnected

## Implementation Details

### P2P Handshake

The tool performs a proper P2P handshake:
1. Connects to target
2. Sends VERSION message
3. Waits for VERACK
4. Sends VERACK
5. Executes attack

### Message Construction

- Uses the same P2P protocol as the main node
- Creates properly formatted message headers
- Deliberately constructs invalid payloads for testing

### Attack Types

#### 1. Invalid PoW
```cpp
header.nBits = 0x00000001;  // Impossible difficulty
```

#### 2. Oversized Headers
```cpp
// Send 3000 headers (limit is 2000)
for (int i = 0; i < 3000; i++) {
    headers.push_back(header);
}
```

#### 3. Non-Continuous Headers
```cpp
header2.hashPrevBlock.SetNull();  // Wrong! Doesn't connect
```

## Files

- `attack_node.cpp` - Main implementation
- `CMakeLists.txt` - Build configuration
- `README.md` - This file

## Related Testing

This tool complements the unit tests in `test/dos_protection_tests.cpp`:
- **Unit tests**: Test DoS protection logic directly (misbehavior scoring, thresholds)
- **Attack tool**: Test end-to-end P2P behavior (real network messages)

## Safety

This tool is designed for testing only:
- ⚠️ Never use on production networks
- ⚠️ Only use on private/regtest networks
- ⚠️ The tool warns before execution
- ⚠️ No persistence - single-shot attacks

## Future Enhancements

Potential additions:
- Low-work header spam
- Future timestamp attacks
- Checkpoint violation attacks
- Ban evasion testing
- Multiple concurrent connections
- Fuzzing support
