# CoinbaseChain Build Instructions

**Version:** 1.0.0  
**Last Updated:** 2025-10-24  
**Supported Platforms:** Linux, macOS, Docker  

---

## Table of Contents

1. [Quick Start](#1-quick-start)
2. [System Requirements](#2-system-requirements)
3. [Dependencies](#3-dependencies)
4. [Building from Source](#4-building-from-source)
5. [Build Configurations](#5-build-configurations)
6. [Docker Build](#6-docker-build)
7. [Testing](#7-testing)
8. [Troubleshooting](#8-troubleshooting)
9. [Advanced Options](#9-advanced-options)

---

## 1. Quick Start

### Linux/macOS
```bash
# Clone the repository
git clone https://github.com/unicitynetwork/coinbasechain-docker.git
cd coinbasechain-docker

# Create build directory
mkdir build && cd build

# Configure and build
cmake ..
make -j$(nproc)

# Binaries are in build/bin/
./bin/coinbasechain --version
```

### Docker
```bash
# Build Docker image
docker build -f Dockerfile -t coinbasechain:latest .

# Run container
docker run -d --name coinbasechain \
  -p 9590:9590 \
  -v coinbasechain-data:/home/coinbasechain/.coinbasechain \
  coinbasechain:latest
```

---

## 2. System Requirements

### Minimum Requirements

| Resource | Minimum | Recommended | Notes |
|----------|---------|-------------|-------|
| **CPU** | 2 cores | 4+ cores | RandomX requires significant CPU |
| **RAM** | 3 GB | 4+ GB | 2GB for RandomX + 1GB overhead |
| **Disk** | 2 GB | 10+ GB | Blockchain + dependencies |
| **Network** | 1 Mbps | 10+ Mbps | P2P synchronization |

### Operating System Support

| Platform | Status | Notes |
|----------|--------|-------|
| **Ubuntu 20.04+** | ✅ Fully Supported | Primary development platform |
| **Ubuntu 22.04** | ✅ Fully Supported | Recommended for production |
| **Debian 11+** | ✅ Supported | Similar to Ubuntu |
| **Fedora 35+** | ✅ Supported | Use dnf package manager |
| **macOS 12+** | ✅ Supported | Requires Homebrew |
| **macOS 13+ (Apple Silicon)** | ✅ Supported | Native ARM64 build |
| **CentOS/RHEL 8+** | ⚠️ Experimental | May need newer toolchain |
| **Windows (WSL2)** | ⚠️ Experimental | Use Ubuntu 22.04 in WSL2 |
| **Windows (Native)** | ❌ Not Supported | Use Docker or WSL2 |

---

## 3. Dependencies

### Required Dependencies

| Dependency | Minimum Version | Purpose |
|------------|-----------------|---------|
| **CMake** | 3.20+ | Build system |
| **C++ Compiler** | C++20 support | Code compilation |
| **Boost** | 1.70+ | Async I/O (Asio) |
| **Git** | 2.0+ | Fetching external dependencies |
| **OpenSSL** | 1.1.1+ | Cryptographic functions |

### Automatically Fetched (via CMake FetchContent)

These dependencies are **automatically downloaded and built** by CMake:

| Dependency | Version | Purpose |
|------------|---------|---------|
| **RandomX** | master | ASIC-resistant PoW algorithm |
| **fmt** | 8.1.1 | Fast string formatting |
| **spdlog** | 1.12.0 | Modern logging library |
| **nlohmann/json** | 3.11.3 | JSON serialization |
| **miniupnpc** | 2.2.5 | UPnP NAT traversal |

**Note:** Internet connection required during first build to fetch dependencies.

---

## 4. Building from Source

### 4.1 Ubuntu/Debian

**Install System Dependencies:**
```bash
# Update package list
sudo apt-get update

# Install build tools and dependencies
sudo apt-get install -y \
  build-essential \
  cmake \
  git \
  libboost-system-dev \
  libssl-dev \
  pkg-config
```

**Build:**
```bash
# Clone repository
git clone https://github.com/unicitynetwork/coinbasechain-docker.git
cd coinbasechain-docker

# Create and enter build directory
mkdir build && cd build

# Configure CMake
cmake ..

# Build (use all CPU cores)
make -j$(nproc)

# Binaries are in build/bin/
ls -lh bin/
```

**Expected Output:**
```
bin/
├── coinbasechain       (main node executable)
└── coinbasechain-cli   (RPC client)
```

### 4.2 Fedora/RHEL/CentOS

**Install System Dependencies:**
```bash
# Install build tools and dependencies
sudo dnf install -y \
  gcc-c++ \
  cmake \
  git \
  boost-devel \
  openssl-devel \
  pkgconfig
```

**Build:** (same as Ubuntu)

### 4.3 macOS

**Install Homebrew (if not installed):**
```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

**Install Dependencies:**
```bash
# Install build tools
brew install cmake boost openssl pkg-config

# Ensure compiler is recent enough (macOS 12+ includes Clang 13+)
clang++ --version  # Should show Clang 13+ with C++20 support
```

**Build:**
```bash
# Clone repository
git clone https://github.com/unicitynetwork/coinbasechain-docker.git
cd coinbasechain-docker

# Create and enter build directory
mkdir build && cd build

# Configure CMake (may need to specify OpenSSL path)
cmake .. \
  -DOPENSSL_ROOT_DIR=$(brew --prefix openssl)

# Build (use all CPU cores)
make -j$(sysctl -n hw.ncpu)

# Binaries are in build/bin/
ls -lh bin/
```

### 4.4 macOS Apple Silicon (M1/M2/M3)

**Same as macOS x86_64, but ensure native ARM64 build:**

```bash
# Verify architecture
uname -m  # Should show "arm64"

# Build (CMake automatically detects ARM64)
cmake ..
make -j$(sysctl -n hw.ncpu)

# Verify binary architecture
file bin/coinbasechain  # Should show "arm64"
```

**Performance Note:** RandomX runs natively on ARM64 and performs well on Apple Silicon.

---

## 5. Build Configurations

### 5.1 Release Build (Default)

Optimized for production use:
```bash
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

**Characteristics:**
- Full optimizations enabled (`-O3`)
- No debug symbols
- Assertions disabled (`-DNDEBUG`)
- ~30% faster than Debug build
- **Recommended for production nodes**

### 5.2 Debug Build

Includes debugging symbols and assertions:
```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
```

**Characteristics:**
- Debug symbols included (`-g`)
- Assertions enabled
- Reduced optimizations (`-O1`)
- Larger binary size (~3x)
- **Recommended for development**

### 5.3 RelWithDebInfo Build

Optimized with debug symbols:
```bash
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
make -j$(nproc)
```

**Characteristics:**
- Full optimizations (`-O2`)
- Debug symbols included (`-g`)
- Assertions disabled
- **Recommended for profiling**

### 5.4 Custom Optimization

**High-Performance Build:**
```bash
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-O3 -march=native -mtune=native" ..
make -j$(nproc)
```

**Warning:** `-march=native` produces CPU-specific binaries (not portable).

---

## 6. Docker Build

### 6.1 Standard Docker Build

**Build Image:**
```bash
# From project root
docker build -f Dockerfile -t coinbasechain:latest .
```

**Build takes ~10-15 minutes on typical hardware.**

**Build Arguments:**
```bash
# Build with specific CMake flags
docker build \
  --build-arg CMAKE_BUILD_TYPE=Release \
  -f Dockerfile \
  -t coinbasechain:latest .
```

### 6.2 Multi-Stage Build Details

The Dockerfile uses **multi-stage build** for minimal runtime image:

**Stage 1: Builder (Ubuntu 22.04)**
- Installs build dependencies
- Compiles CoinbaseChain
- ~1.5 GB image size

**Stage 2: Runtime (Ubuntu 22.04)**
- Only runtime dependencies
- Copies binaries from builder
- ~200 MB final image size

**Image Size Comparison:**
```
coinbasechain:latest     200 MB  (multi-stage)
coinbasechain:builder    1.5 GB  (build stage only)
```

### 6.3 Docker Compose

**Create `docker-compose.yml`:**
```yaml
version: '3.8'

services:
  coinbasechain:
    image: coinbasechain:latest
    container_name: coinbasechain-node
    ports:
      - "9590:9590"  # P2P port
    volumes:
      - coinbasechain-data:/home/coinbasechain/.coinbasechain
    environment:
      COINBASECHAIN_NETWORK: mainnet
      COINBASECHAIN_THREADS: 4
      COINBASECHAIN_LOGLEVEL: info
    restart: unless-stopped

volumes:
  coinbasechain-data:
```

**Start:**
```bash
docker-compose up -d
```

**View Logs:**
```bash
docker-compose logs -f
```

**Execute RPC Commands:**
```bash
docker exec coinbasechain-node coinbasechain-cli getblockcount
```

---

## 7. Testing

### 7.1 Build Tests

Tests are **built by default** during compilation:

```bash
# Tests are automatically built
make -j$(nproc)

# Test executable: build/coinbasechain_tests
ls -lh coinbasechain_tests
```

### 7.2 Running Tests

**Run All Tests:**
```bash
./coinbasechain_tests
```

**Expected Output:**
```
All tests passed (357 test cases, 4,806 assertions)
```

**Run Specific Test Categories:**
```bash
# Unit tests only
./coinbasechain_tests "[unit]"

# Integration tests only
./coinbasechain_tests "[integration]"

# Network tests only
./coinbasechain_tests "[network]"

# Security tests only
./coinbasechain_tests "[security]"

# RandomX tests only
./coinbasechain_tests "[randomx]"
```

**Run with Verbose Output:**
```bash
./coinbasechain_tests -v
```

**Run Specific Test by Name:**
```bash
./coinbasechain_tests "MessageRouter"
```

**Show Test Timings:**
```bash
./coinbasechain_tests --durations yes
```

**List All Tests:**
```bash
./coinbasechain_tests --list-tests
```

### 7.3 Test Categories

| Category | Test Cases | Coverage | Purpose |
|----------|------------|----------|---------|
| **Unit** | 112 | 92% | Individual component testing |
| **Integration** | 63 | 89% | End-to-end system testing |
| **Network** | 89 | 98% | P2P protocol testing |
| **Security** | 31 | 95% | DoS and attack resistance |
| **RandomX** | 24 | 88% | PoW algorithm testing |
| **Performance** | 38 | 94% | Benchmarking and profiling |

**Total:** 357 test cases, 4,806 assertions

### 7.4 CI/CD Testing

**JUnit Output (for CI):**
```bash
./coinbasechain_tests -r junit -o test-results.xml
```

**Exit Code:**
- `0` = All tests passed
- `1` = One or more tests failed

---

## 8. Troubleshooting

### 8.1 Common Build Errors

#### Error: "CMake version too old"
```
CMake Error: CMake 3.20 or higher is required.
```

**Solution (Ubuntu/Debian):**
```bash
# Add Kitware APT repository for latest CMake
sudo apt-get install -y software-properties-common
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc | sudo apt-key add -
sudo apt-add-repository 'deb https://apt.kitware.com/ubuntu/ focal main'
sudo apt-get update
sudo apt-get install cmake
```

#### Error: "C++20 not supported"
```
error: 'concept' in namespace 'std' does not name a type
```

**Solution:** Upgrade compiler:
```bash
# Ubuntu/Debian
sudo apt-get install -y g++-11 gcc-11
export CXX=g++-11
export CC=gcc-11

# Fedora
sudo dnf install -y gcc-c++

# macOS
brew install llvm
export CXX=/usr/local/opt/llvm/bin/clang++
```

#### Error: "Boost not found"
```
CMake Error: Could NOT find Boost
```

**Solution:**
```bash
# Ubuntu/Debian
sudo apt-get install -y libboost-system-dev

# Fedora
sudo dnf install -y boost-devel

# macOS
brew install boost
```

#### Error: "OpenSSL not found" (macOS)
```
CMake Error: Could NOT find OpenSSL
```

**Solution:**
```bash
cmake .. -DOPENSSL_ROOT_DIR=$(brew --prefix openssl)
```

### 8.2 Runtime Errors

#### Error: "Cannot allocate memory" (RandomX)
```
Failed to initialize RandomX dataset: out of memory
```

**Solution:** Increase available RAM or swap space:
```bash
# Check available memory
free -h

# Add swap (if needed)
sudo fallocate -l 2G /swapfile
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile
```

#### Error: "Port already in use"
```
Failed to bind to port 9590: Address already in use
```

**Solution:**
```bash
# Check what's using the port
sudo lsof -i :9590

# Kill the process or use different port
./bin/coinbasechain --port=9591
```

### 8.3 Test Failures

#### Intermittent Network Test Failures

**Symptom:** Tests pass when run individually but fail in full suite.

**Solution:** Run tests with more timeout:
```bash
./coinbasechain_tests --timeout 10  # 10 seconds per test
```

#### RandomX Test Failures (Low Memory)

**Symptom:**
```
TEST FAILED: RandomX dataset allocation failed
```

**Solution:** Close memory-intensive applications or increase RAM.

---

## 9. Advanced Options

### 9.1 Sanitizer Builds

For finding memory errors and race conditions:

**Thread Sanitizer (TSan):**
```bash
mkdir build-tsan && cd build-tsan
cmake -DSANITIZE=thread ..
make -j$(nproc)
./coinbasechain_tests
```

**Address Sanitizer (ASan):**
```bash
mkdir build-asan && cd build-asan
cmake -DSANITIZE=address ..
make -j$(nproc)
./coinbasechain_tests
```

**Undefined Behavior Sanitizer (UBSan):**
```bash
mkdir build-ubsan && cd build-ubsan
cmake -DSANITIZE=undefined ..
make -j$(nproc)
./coinbasechain_tests
```

**Performance Impact:**
- TSan: ~5-15x slower
- ASan: ~2x slower
- UBSan: ~20% slower

### 9.2 Fuzzing Builds

Requires Clang with libFuzzer support:

```bash
# Install Clang (if not already installed)
sudo apt-get install -y clang-14

# Build with fuzzing enabled
mkdir build-fuzz && cd build-fuzz
cmake -DENABLE_FUZZING=ON \
      -DCMAKE_CXX_COMPILER=clang++-14 \
      -DCMAKE_C_COMPILER=clang-14 ..
make -j$(nproc)

# Run fuzz targets
./fuzz/fuzz_block_header
./fuzz/fuzz_messages
./fuzz/fuzz_chain_reorg
```

**Available Fuzz Targets:**
- `fuzz_block_header` - Block header parsing
- `fuzz_varint` - VarInt encoding/decoding
- `fuzz_messages` - P2P message deserialization
- `fuzz_message_header` - Message header parsing
- `fuzz_chain_reorg` - Chain reorganization logic

### 9.3 Cross-Compilation

**ARM64 from x86_64 (Linux):**
```bash
# Install cross-compiler
sudo apt-get install -y g++-aarch64-linux-gnu

# Configure for ARM64
cmake .. \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++

make -j$(nproc)
```

**Verify binary:**
```bash
file bin/coinbasechain
# Output: bin/coinbasechain: ELF 64-bit LSB executable, ARM aarch64, ...
```

### 9.4 Static Linking

For portable binaries:

```bash
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++"

make -j$(nproc)
```

**Binary Size:**
- Dynamic: ~5 MB
- Static: ~15 MB

### 9.5 Custom Installation Prefix

```bash
cmake .. -DCMAKE_INSTALL_PREFIX=/opt/coinbasechain
make -j$(nproc)
sudo make install
```

**Installed Files:**
```
/opt/coinbasechain/
├── bin/
│   ├── coinbasechain
│   └── coinbasechain-cli
└── share/
    └── coinbasechain/
        └── doc/
```

### 9.6 Parallel Build Configuration

**Limit Build Parallelism:**
```bash
# Use only 2 cores (conserve memory)
make -j2
```

**Ninja Build System (faster than Make):**
```bash
sudo apt-get install -y ninja-build

cmake -G Ninja ..
ninja -j$(nproc)
```

**Build Time Comparison (8-core CPU):**
- Make: ~8 minutes
- Ninja: ~6 minutes

---

## Appendix A: Dependency Versions

### Tested Compiler Versions

| Compiler | Version | Status |
|----------|---------|--------|
| **GCC** | 10.x | ✅ Tested |
| **GCC** | 11.x | ✅ Recommended |
| **GCC** | 12.x | ✅ Tested |
| **GCC** | 13.x | ✅ Tested |
| **Clang** | 11.x | ✅ Tested |
| **Clang** | 12.x | ✅ Tested |
| **Clang** | 13.x | ✅ Recommended |
| **Clang** | 14.x+ | ✅ Tested |
| **Apple Clang** | 13.x+ | ✅ Tested (macOS 12+) |
| **MSVC** | - | ❌ Not Supported |

### CMake Versions

| Version | Status | Notes |
|---------|--------|-------|
| 3.20 | ✅ Minimum | Required for C++20 support |
| 3.22+ | ✅ Recommended | Better FetchContent support |
| 3.25+ | ✅ Latest | Fastest builds |

### Boost Versions

| Version | Status | Notes |
|---------|--------|-------|
| 1.70 | ✅ Minimum | Basic Asio support |
| 1.74+ | ✅ Recommended | Improved Asio performance |
| 1.80+ | ✅ Latest | Best compatibility |

---

## Appendix B: Build Time Estimates

**First Build (includes dependency download):**

| Platform | Cores | Time | Notes |
|----------|-------|------|-------|
| Linux (AMD64) | 4 | ~12 min | Intel i5 |
| Linux (AMD64) | 8 | ~8 min | Intel i7 |
| Linux (AMD64) | 16 | ~6 min | AMD Ryzen 9 |
| macOS (Intel) | 8 | ~10 min | Intel i9 |
| macOS (M1) | 8 | ~7 min | Apple Silicon |
| Docker | 4 | ~15 min | Includes image build |

**Incremental Build (after code change):**

| Change Type | Time | Notes |
|-------------|------|-------|
| Single file | ~10s | Recompile + relink |
| Header change | ~30s | Recompile affected files |
| Full rebuild | ~3 min | No dependency download |

---

## Appendix C: Binary Sizes

**Stripped Release Build:**

| Binary | Size | Notes |
|--------|------|-------|
| `coinbasechain` | ~8 MB | Main node executable |
| `coinbasechain-cli` | ~2 MB | RPC client |
| `coinbasechain_tests` | ~15 MB | Test suite |

**Debug Build:**

| Binary | Size | Notes |
|--------|------|-------|
| `coinbasechain` | ~25 MB | With debug symbols |
| `coinbasechain-cli` | ~6 MB | With debug symbols |
| `coinbasechain_tests` | ~50 MB | With debug symbols |

---

## Appendix D: Verification

**Verify Build Success:**

```bash
# Check binary exists and is executable
ls -lh bin/coinbasechain
file bin/coinbasechain
ldd bin/coinbasechain  # Linux only

# Run version check
./bin/coinbasechain --version

# Run basic test
./coinbasechain_tests "[unit]"

# Start node (will create datadir)
./bin/coinbasechain -regtest -printtoconsole
# Press Ctrl+C to stop after seeing "Node initialized"
```

**Expected Output:**
```
CoinbaseChain version v1.0.0
Protocol version: 1
RandomX: initialized
ASERT difficulty: enabled
```

---

## Support

**Documentation:**
- Architecture: [ARCHITECTURE.md](ARCHITECTURE.md)
- Protocol: [PROTOCOL_SPECIFICATION.md](PROTOCOL_SPECIFICATION.md)
- Network: [NETWORK_PROTOCOL_SPECIFICATION.md](NETWORK_PROTOCOL_SPECIFICATION.md)

**Community:**
- Issues: [GitHub Issues](https://github.com/unicitynetwork/coinbasechain-docker/issues)
- Discussions: [GitHub Discussions](https://github.com/unicitynetwork/coinbasechain-docker/discussions)

---

**END OF BUILD INSTRUCTIONS**
