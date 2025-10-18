# Fuzzing CoinbaseChain

This directory contains fuzz targets for testing CoinbaseChain's parsing and deserialization code.

## Fuzz Targets

### Shallow Parsing Targets (Fast, byte-level fuzzing)

- **fuzz_block_header** - Tests block header deserialization (100-byte headers)
- **fuzz_varint** - Tests variable-length integer encoding/decoding
- **fuzz_messages** - Tests all network message types (VERSION, HEADERS, INV, etc.)
- **fuzz_message_header** - Tests message header parsing (magic, command, checksum)

### Deep Logic Targets (State machine fuzzing)

- **fuzz_chain_reorg** - Tests chain reorganization, orphan processing, and InvalidateBlock cascades
  - Exercises 316+ conditional branches in chainstate_manager.cpp
  - Tests competing forks, reorg depth limits, orphan resolution
  - Validates chain selection and suspicious reorg detection
  - Runs at ~270k exec/s (bypasses expensive PoW for speed)

## Local Fuzzing with libFuzzer

### Prerequisites

- Clang compiler (for libFuzzer support)
- CMake 3.20+
- Boost libraries

### Building Fuzz Targets

```bash
# Create build directory
mkdir -p build-fuzz
cd build-fuzz

# Configure with fuzzing enabled
cmake .. \
    -DENABLE_FUZZING=ON \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo

# Build
make -j$(nproc)
```

### Running Fuzz Targets

```bash
# Run for 60 seconds with 4 parallel jobs
./fuzz/fuzz_block_header -max_total_time=60 -jobs=4

# Run with corpus directory (saves interesting inputs)
mkdir -p corpus/block_header
./fuzz/fuzz_block_header corpus/block_header -max_total_time=300

# Run chain reorganization fuzzer (with seed corpus)
python3 ../fuzz/generate_chain_seeds.py  # Creates fuzz_chain_reorg_corpus/
./fuzz/fuzz_chain_reorg fuzz_chain_reorg_corpus/ -max_total_time=300

# Run with AddressSanitizer for better bug detection
cmake .. -DENABLE_FUZZING=ON -DCMAKE_CXX_COMPILER=clang++ -DSANITIZE=address
make -j$(nproc)
./fuzz/fuzz_messages corpus/messages -max_total_time=600
```

### Useful libFuzzer Options

- `-max_total_time=N` - Run for N seconds
- `-jobs=N` - Run N parallel fuzzing jobs
- `-max_len=N` - Maximum input length
- `-dict=file.dict` - Use dictionary file for structured inputs
- `-print_final_stats=1` - Show coverage statistics
- `-help=1` - Show all options

## OSS-Fuzz Integration

### What is OSS-Fuzz?

OSS-Fuzz is Google's continuous fuzzing service for open source projects. It provides:
- 24/7 fuzzing infrastructure
- Automatic bug reporting
- Multiple sanitizers (ASan, UBSan, MSan)
- Multiple fuzzing engines (libFuzzer, AFL++, Hongfuzz)
- Coverage tracking

### Integration Files

The `../oss-fuzz/` directory contains:
- **project.yaml** - Project configuration and contacts
- **Dockerfile** - Build environment setup
- **build.sh** - Build script for OSS-Fuzz infrastructure

### Testing OSS-Fuzz Integration Locally

You can test the OSS-Fuzz build locally before submission:

```bash
# Clone OSS-Fuzz
git clone https://github.com/google/oss-fuzz.git
cd oss-fuzz

# Create project directory
mkdir -p projects/coinbasechain
cp ../coinbasechain/oss-fuzz/* projects/coinbasechain/

# Build with OSS-Fuzz
python infra/helper.py build_image coinbasechain
python infra/helper.py build_fuzzers coinbasechain

# Run a fuzzer
python infra/helper.py run_fuzzer coinbasechain fuzz_block_header
```

### Submitting to OSS-Fuzz

To get continuous fuzzing from Google:

1. Fork the OSS-Fuzz repository
2. Add your project files to `projects/coinbasechain/`
3. Test locally with the commands above
4. Submit a pull request to OSS-Fuzz
5. Google will review and approve

See: https://google.github.io/oss-fuzz/getting-started/new-project-guide/

## Best Practices

1. **Start small** - Run fuzzers for a few minutes locally first
2. **Use corpora** - Save and reuse interesting inputs
3. **Run with sanitizers** - ASan, UBSan catch memory bugs
4. **Parallel fuzzing** - Use multiple jobs for faster coverage
5. **Check regularly** - Even 5 minutes of fuzzing can find bugs
6. **Minimize crashes** - Use `libFuzzer -minimize_crash=1` to reduce crash cases

## Interpreting Results

### Crash Found

If a fuzzer finds a crash:
```
==12345==ERROR: AddressSanitizer: heap-buffer-overflow
```

The crashing input is saved to `crash-*` file. To reproduce:
```bash
./fuzz/fuzz_block_header crash-abc123
```

### Coverage Statistics

LibFuzzer reports coverage:
```
#12345 NEW    cov: 456 ft: 789 corp: 23/1234b
```
- **cov**: Code coverage (number of edges)
- **ft**: Feature coverage (interesting behaviors)
- **corp**: Corpus size

## Security Considerations

These fuzz targets test:
- Buffer overflow vulnerabilities
- Integer overflows in VarInt parsing
- DoS via excessive allocations
- Malformed message handling
- Round-trip serialization consistency

All parsing code should handle arbitrary untrusted input without crashing.
