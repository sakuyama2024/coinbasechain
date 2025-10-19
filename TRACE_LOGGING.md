# TRACE Logging Guide

## Overview

The codebase now has comprehensive TRACE-level logging throughout the networking and chain subsystems. This logging is disabled by default for performance, but can be enabled via environment variables for debugging.

## Running Tests with TRACE Logging

### Basic Usage

```bash
# Enable TRACE logging for all components
COINBASE_TEST_LOG_LEVEL=trace ./build/coinbasechain_tests

# Run specific test with TRACE
COINBASE_TEST_LOG_LEVEL=trace ./build/coinbasechain_tests "[InvalidateBlock]"

# Run with other log levels
COINBASE_TEST_LOG_LEVEL=debug ./build/coinbasechain_tests
COINBASE_TEST_LOG_LEVEL=info ./build/coinbasechain_tests
```

### Filtering TRACE Output

Since TRACE logging is very verbose, you can filter for specific components:

```bash
# Show only chain-related TRACE logs
COINBASE_TEST_LOG_LEVEL=trace ./build/coinbasechain_tests 2>&1 | grep "AddToBlockIndex\|SetTip\|ActivateBestChain"

# Show only network-related TRACE logs
COINBASE_TEST_LOG_LEVEL=trace ./build/coinbasechain_tests 2>&1 | grep "Peer\|Message\|send\|recv"

# Show only PoW-related TRACE logs
COINBASE_TEST_LOG_LEVEL=trace ./build/coinbasechain_tests 2>&1 | grep "CheckProofOfWork\|GetNextWorkRequired\|ASERT"
```

## Running the Node with TRACE Logging

### Using Command-Line Flags

```bash
# Enable TRACE for all components
./coinbasechain --debug=all

# Enable TRACE for specific components
./coinbasechain --debug=chain
./coinbasechain --debug=network
./coinbasechain --debug=chain,network

# Set global log level to TRACE
./coinbasechain --loglevel=trace
```

### Log Output Location

- **Tests**: Console output only (stderr)
- **Node**: Both console and `~/.coinbasechain/debug.log` (or custom `--datadir`)

## TRACE Logging Coverage

### Chain Subsystem (`--debug=chain`)

#### chainstate_manager.cpp
- **AcceptBlockHeader**: Header validation entry point with hash/prev/peer
- **ActivateBestChain**: Chain selection, fork detection, reorg process
- **ConnectTip/DisconnectTip**: Block connection/disconnection tracking
- **ProcessOrphanHeaders**: Orphan pool processing
- **TryAddOrphanHeader**: Orphan addition with DoS limits
- **InvalidateBlock**: Manual block invalidation
- **CheckHeadersPoW**: PoW validation for header batches

#### pow.cpp
- **GetNextWorkRequired**: Difficulty adjustment (ASERT algorithm details)
  - Genesis/regtest/pre-anchor cases
  - ASERT calculation parameters (anchor height, time/height diff, target spacing, half-life)
  - Final difficulty result
- **CheckProofOfWork**: PoW verification
  - Verification mode (FULL/COMMITMENT/MINING)
  - Invalid nBits detection
  - Commitment verification (pass/fail with values)
  - RandomX hash computation (epoch, result)
  - Success/failure with details

#### block_manager.cpp
- **Initialize**: Genesis block setup
- **AddToBlockIndex**: Block addition to index
  - Duplicate detection
  - Height and chainwork calculation
  - Genesis vs. normal block handling

#### chain.cpp
- **SetTip**: Active chain tip changes
- **FindFork**: Fork point detection (critical for reorgs!)
- **LocatorEntries**: Block locator creation for P2P sync

#### timedata.cpp
- **AddTimeData**: Network time sample tracking
  - Peer and offset details
  - Median calculation
  - Time offset adjustment

### Network Subsystem (`--debug=network`)

#### peer.cpp
- Peer lifecycle (start, disconnect)
- Message sending
- VERSION/VERACK handshake

#### peer_manager.cpp
- Misbehavior scoring
- Peer tracking
- Periodic cleanup

#### network_manager.cpp
- Message handling
- Duplicate header detection
- DoS checks

## Examples

### Debug a Reorg Issue

```bash
# Run InvalidateBlock test with full chain TRACE
COINBASE_TEST_LOG_LEVEL=trace ./build/coinbasechain_tests "[InvalidateBlock]" 2>&1 | \
  grep -E "ActivateBestChain|FindFork|SetTip|InvalidateBlock" | \
  head -100
```

### Debug PoW Validation

```bash
# Run PoW test with full TRACE
COINBASE_TEST_LOG_LEVEL=trace ./build/coinbasechain_tests "[pow]" 2>&1 | \
  grep -E "CheckProofOfWork|GetNextWorkRequired|ASERT"
```

### Debug Network Sync

```bash
# Run network tests with TRACE
COINBASE_TEST_LOG_LEVEL=trace ./build/coinbasechain_tests "[network]" 2>&1 | \
  grep -E "handle_message|send_message|Peer::" | \
  less
```

### Monitor Live Node

```bash
# Start node with chain TRACE logging
./coinbasechain --debug=chain --regtest

# In another terminal, tail the log
tail -f ~/.coinbasechain/debug.log | grep TRACE
```

## Performance Impact

- **Disabled (default)**: Zero overhead - logging checks are inlined and optimized away
- **Enabled**: ~1-3% overhead for TRACE level (mostly I/O bound)

TRACE logging should only be used for debugging, not in production.

## Test Considerations

**Important**: Some unit tests use `TestChainstateManager` which bypasses actual chain validation for speed. These tests won't exercise the TRACE logging in:
- `pow.cpp::CheckProofOfWork` (PoW bypassed)
- `chainstate_manager.cpp::AcceptBlockHeader` (validation bypassed)

To see full TRACE output, run **integration tests** that use real validation:
- `[InvalidateBlock]` - Exercises reorg logic
- `[network]` - Exercises P2P and chain sync
- `[dos]` - Exercises DoS protection and validation

Example:
```bash
# This shows TRACE from test helpers, not real validation
COINBASE_TEST_LOG_LEVEL=trace ./build/coinbasechain_tests "Reorg - Simple"

# This shows TRACE from actual chain/network code
COINBASE_TEST_LOG_LEVEL=trace ./build/coinbasechain_tests "[InvalidateBlock]"
```

## Implementation Details

### Test Logging Initialization

The test framework (`test/catch_amalgamated.cpp`) automatically:
1. Reads `COINBASE_TEST_LOG_LEVEL` environment variable (default: "info")
2. Initializes LogManager with specified level
3. If level="trace", enables TRACE for all components (chain, network, sync, crypto, app)
4. Shuts down logging after tests complete

### Component Loggers

The logging system uses component-based loggers:
- `LOG_CHAIN_TRACE()` - Chain operations
- `LOG_NET_TRACE()` - Network operations
- `LOG_SYNC_TRACE()` - Sync operations
- `LOG_CRYPTO_TRACE()` - Cryptographic operations
- `LOG_APP_TRACE()` - Application-level operations

Each can be independently controlled via `--debug=<component>`.
