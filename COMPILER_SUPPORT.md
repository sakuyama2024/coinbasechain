# Compiler Support Strategy

## Overview

CoinbaseChain officially supports **LLVM/Clang** and **GCC** on macOS and Linux.

## Supported Compilers

| Compiler | Minimum Version | Recommended Version | Primary Use Case |
|----------|----------------|---------------------|------------------|
| **LLVM/Clang** | 15.0 | Latest (21.0+) | **Development, fuzzing, CI** |
| **GCC** | 11.0 | Latest (13.0+) | Production builds, Linux distros |
| Apple Clang | ❌ Not supported | Use Homebrew LLVM | Missing C++20 features |

## Recommended: LLVM/Clang for Development

### Why LLVM/Clang is Primary

**For security-critical blockchain code, LLVM provides superior tooling:**

1. **Fuzzing** - libFuzzer built-in (required for `ENABLE_FUZZING=ON`)
2. **Sanitizers** - Better ASan, TSan, MSan, UBSan implementations
3. **Static Analysis** - clang-tidy with blockchain-specific checks
4. **Consistent Behavior** - Same compiler across macOS and Linux
5. **Better Diagnostics** - Clearer error messages
6. **C++20 Support** - Ahead of GCC in many areas

### Installation

#### macOS (Homebrew LLVM)
```bash
brew install llvm boost cmake

# Add to PATH (or specify full path in cmake)
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
```

#### Linux (Ubuntu/Debian)
```bash
# LLVM 18 (recommended)
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 18

# Or install from packages
sudo apt install clang-18 clang++-18 lldb-18 lld-18
```

#### Linux (Fedora/RHEL)
```bash
sudo dnf install clang llvm lld compiler-rt
```

### Building with LLVM
```bash
# Standard build
cmake -B build -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build build -j$(nproc)

# Development build (with sanitizers)
cmake -B build-asan \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_COMPILER=clang \
    -DSANITIZE=address
cmake --build build-asan -j$(nproc)

# Fuzzing build (LLVM required)
cmake -B build-fuzz \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DENABLE_FUZZING=ON
cmake --build build-fuzz -j$(nproc)
```

## Secondary: GCC for Production

GCC is supported for users who prefer it or for distribution packaging.

### When to Use GCC
- Linux distribution packaging (Debian, Ubuntu, Fedora)
- Systems where LLVM is not available
- Production deployments (both work equally well)
- Validating code is not compiler-specific

### Installation

#### Linux (Ubuntu/Debian)
```bash
sudo apt install g++-11 gcc-11  # Minimum
sudo apt install g++-13 gcc-13  # Recommended
```

#### Linux (Fedora/RHEL)
```bash
sudo dnf install gcc-c++
```

#### macOS (Not Recommended)
```bash
brew install gcc
# Note: Still need Homebrew LLVM for fuzzing
```

### Building with GCC
```bash
# Standard build
cmake -B build -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc
cmake --build build -j$(nproc)

# With sanitizers (works but LLVM is better)
cmake -B build-asan \
    -DCMAKE_CXX_COMPILER=g++ \
    -DSANITIZE=address
cmake --build build-asan -j$(nproc)
```

**Note:** Fuzzing requires LLVM. GCC does not have libFuzzer support.

## Not Supported: Apple Clang

Apple's bundled Clang (from Xcode) **lags behind** LLVM releases:
- Missing C++20 features
- Missing libFuzzer runtime
- Older sanitizer implementations
- Inconsistent with Linux builds

**Solution:** Use Homebrew LLVM on macOS instead of Apple Clang.

## CI/CD Strategy

### Recommended CI Matrix

Test both compilers to ensure portability:

```yaml
# .github/workflows/build.yml
strategy:
  matrix:
    os: [ubuntu-latest, macos-latest]
    compiler: [clang, gcc]
    exclude:
      - os: macos-latest
        compiler: gcc  # Skip GCC on macOS (Homebrew LLVM only)
```

### What to Test
1. **LLVM/Clang** (primary):
   - Build with all sanitizers (ASan, UBSan, TSan)
   - Run fuzz targets
   - Run unit tests
   - Run functional tests

2. **GCC** (secondary):
   - Build without sanitizers (baseline)
   - Run unit tests
   - Verify no compiler-specific issues

## Compiler Feature Requirements

CoinbaseChain requires these **C++20 features**:

| Feature | LLVM 15+ | GCC 11+ | Apple Clang 14 |
|---------|----------|---------|----------------|
| Concepts | ✅ | ✅ | ⚠️ Partial |
| Ranges | ✅ | ✅ | ❌ |
| `std::format` | ✅ (18+) | ⚠️ (13+) | ❌ |
| Three-way comparison | ✅ | ✅ | ⚠️ Partial |
| Coroutines | ✅ | ✅ | ⚠️ Partial |
| consteval | ✅ | ✅ | ⚠️ Buggy |

**Why spdlog uses `std::format`:**
- Avoids fmt library dependency issues
- Works around consteval bugs in older compilers
- See `CMakeLists.txt:67` - `SPDLOG_USE_STD_FORMAT=ON`

## Compiler-Specific Flags

### LLVM/Clang Flags
```cmake
# Warnings as errors (strict)
-Werror -Wall -Wextra -Wpedantic

# Security hardening
-fstack-protector-strong
-D_FORTIFY_SOURCE=2

# Performance
-O3 -march=native (release)
-O1 -g (fuzzing - faster than -O0)
```

### GCC Flags
```cmake
# Same as Clang, but may need:
-Wno-maybe-uninitialized  # GCC false positives

# GCC-specific optimizations
-flto=auto  # Link-time optimization
```

## Cross-Compiler Compatibility Guidelines

### Write Portable Code
1. ✅ Use standard C++20 features
2. ✅ Avoid compiler intrinsics (use `<bit>` instead)
3. ✅ Test with both compilers in CI
4. ✅ Use `-Werror` to catch warnings early

### Platform-Specific Code
```cpp
#ifdef __clang__
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wformat-nonliteral"
#elif defined(__GNUC__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif

// Code with warnings...

#ifdef __clang__
  #pragma clang diagnostic pop
#elif defined(__GNUC__)
  #pragma GCC diagnostic pop
#endif
```

## Troubleshooting

### "libfuzzer runtime not found"
- **Cause:** Using Apple Clang instead of Homebrew LLVM
- **Solution:** Install Homebrew LLVM: `brew install llvm`

### "std::format not found"
- **Cause:** GCC < 13 or LLVM < 18
- **Solution:** Upgrade compiler or use spdlog's fmt backend

### "consteval errors with spdlog"
- **Cause:** spdlog + fmt + LLVM 21+ consteval strictness
- **Solution:** Already fixed via `SPDLOG_USE_STD_FORMAT=ON`

### Different behavior between compilers
- **Test:** Run full test suite with both compilers
- **Fix:** Likely undefined behavior - use sanitizers to find it

## Summary: Quick Reference

| Task | Compiler | Why |
|------|----------|-----|
| **Daily development** | LLVM/Clang | Best tooling, sanitizers, fuzzing |
| **Security testing** | LLVM/Clang | Superior sanitizers, libFuzzer |
| **Production builds** | LLVM or GCC | Both work equally well |
| **CI testing** | Both | Ensures portability |
| **Fuzzing** | LLVM only | libFuzzer not in GCC |
| **Linux packaging** | GCC | Distribution preference |
| **macOS development** | Homebrew LLVM | Apple Clang too old |

## Decision: LLVM Primary, GCC Secondary

**Official stance:**
1. **LLVM/Clang is the primary development compiler**
   - Required for fuzzing
   - Best sanitizer support
   - Consistent across platforms
   - Maintained by CoinbaseChain developers

2. **GCC is supported for production use**
   - Validates code portability
   - Preferred by some Linux distributions
   - Equally robust for production builds
   - Not required for development

3. **Apple Clang is not supported**
   - Use Homebrew LLVM on macOS instead

This strategy provides the best balance of:
- **Security** (LLVM's superior tooling)
- **Portability** (GCC compatibility)
- **Consistency** (same compiler across platforms)
- **Flexibility** (users can choose)
