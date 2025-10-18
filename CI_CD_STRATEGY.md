# CI/CD Strategy for CoinbaseChain

## Philosophy

For a **security-critical blockchain**, our CI/CD strategy prioritizes:
1. **Security** - Multiple sanitizers, fuzzing, static analysis
2. **Correctness** - Extensive test coverage, multiple compilers
3. **Performance** - Benchmarking, profiling, optimization tracking
4. **Speed** - Fast feedback for developers (~5-10 min CI)
5. **Cost** - Run expensive tests nightly, not on every commit

## Overview

```
Developer Push → PR CI (fast) → Merge → Post-Merge CI → Nightly Tests → Release
     ↓                                        ↓              ↓
  Local tests                          Docker build    Fuzzing (24/7)
                                       Benchmarks      Security scan
```

## Stage 1: Developer Workflow (Local)

**Before pushing code:**

```bash
# Quick local checks (< 2 minutes)
cmake --build build -j
./build/coinbasechain_tests

# If touching critical code, run sanitizers locally
cmake -B build-asan -DSANITIZE=address
cmake --build build-asan -j
./build-asan/coinbasechain_tests

# Format check
clang-format -i src/**/*.cpp include/**/*.hpp
```

**Pre-commit hooks (optional but recommended):**
- Run clang-format
- Build succeeds
- Tests pass

## Stage 2: Pull Request CI (Fast Feedback)

**Trigger:** Every PR push
**Goal:** Fast feedback (5-10 minutes)
**Cost:** Optimize for speed, not exhaustive coverage

### 2.1 Quick Build & Test Matrix

Run in parallel:

| Job | Platform | Compiler | Sanitizer | Duration |
|-----|----------|----------|-----------|----------|
| **build-test-linux-clang** | Ubuntu 22.04 | Clang 18 | None | ~3 min |
| **build-test-linux-gcc** | Ubuntu 22.04 | GCC 13 | None | ~3 min |
| **build-test-macos** | macOS 14 | Homebrew LLVM | None | ~4 min |
| **build-asan** | Ubuntu 22.04 | Clang 18 | ASan+UBSan | ~5 min |

**What we test:**
- ✅ Build succeeds
- ✅ All unit tests pass
- ✅ No memory leaks (ASan)
- ✅ No undefined behavior (UBSan)
- ✅ Works on Linux + macOS
- ✅ Works with Clang + GCC

**What we skip** (save for nightly):
- ❌ ThreadSanitizer (slow)
- ❌ Extensive fuzzing (expensive)
- ❌ Performance benchmarks
- ❌ Integration tests (if slow)

### 2.2 Code Quality Checks

Run in parallel:

| Job | What | Duration |
|-----|------|----------|
| **formatting** | clang-format check | ~30 sec |
| **static-analysis** | clang-tidy on changed files | ~2 min |
| **fuzz-smoke-test** | 10k runs per target | ~1 min |

### 2.3 PR Requirements

To merge, PR must pass:
- ✅ All build jobs pass
- ✅ All tests pass
- ✅ ASan/UBSan clean
- ✅ Formatting correct
- ✅ No new clang-tidy warnings
- ✅ At least 1 approval

## Stage 3: Post-Merge CI (Main Branch)

**Trigger:** Push to `main` branch
**Goal:** Final verification before deploy
**Duration:** ~15-20 minutes

### 3.1 Extended Test Suite

Everything from PR CI, plus:

| Job | What | Why |
|-----|------|-----|
| **build-tsan** | ThreadSanitizer | Catch race conditions |
| **fuzz-extended** | 1M runs per target | More coverage |
| **integration-tests** | End-to-end scenarios | System behavior |
| **benchmark** | Performance tests | Track regressions |

### 3.2 Artifacts

Build and upload:

1. **Binaries** (Linux, macOS)
   - `coinbasechain` (node)
   - `coinbasechain-cli` (RPC client)
   - `coinbasechain_tests` (test suite)

2. **Docker Image**
   - Multi-stage build (small image)
   - Push to GitHub Container Registry
   - Tag: `ghcr.io/coinbasechain/node:main-{sha}`

3. **Coverage Report**
   - Generate with `--coverage` flag
   - Upload to Codecov
   - Track coverage trends

## Stage 4: Nightly Tests (Scheduled)

**Trigger:** Daily at 2 AM UTC
**Goal:** Expensive, exhaustive testing
**Duration:** ~2-4 hours

### 4.1 Extended Fuzzing

```yaml
- Run each fuzz target for 1 hour
- With corpus from previous runs
- Multiple sanitizers (ASan, MSan, UBSan)
- Save corpus for next run
- Report crashes as issues
```

**Targets:**
- `fuzz_block_header` (1 hour)
- `fuzz_varint` (1 hour)
- `fuzz_messages` (1 hour)
- `fuzz_message_header` (1 hour)
- `fuzz_chain_reorg` (1 hour)

### 4.2 Additional Sanitizers

| Sanitizer | What it finds | Notes |
|-----------|---------------|-------|
| **MemorySanitizer** | Uninitialized reads | Clang only, slow |
| **LeakSanitizer** | Memory leaks | Part of ASan |
| **ThreadSanitizer** | Data races | Very slow |

### 4.3 Static Analysis

```yaml
- CodeQL (GitHub security scanning)
- clang-tidy on entire codebase
- cppcheck
- Include-what-you-use
```

### 4.4 Performance Benchmarks

```yaml
- Run benchmark suite
- Compare vs baseline
- Alert if >5% regression
- Track metrics over time
```

### 4.5 Cross-Platform Builds

Test additional platforms:
- Ubuntu 20.04, 22.04, 24.04
- Debian 11, 12
- Fedora latest
- macOS 13, 14, 15
- Alpine Linux (musl libc)

### 4.6 Stress Tests

```yaml
- Long-running tests (4+ hours)
- High-concurrency scenarios
- Memory pressure tests
- Network partition simulations
```

## Stage 5: OSS-Fuzz (Continuous)

**Trigger:** Automatic (Google's infrastructure)
**Goal:** Find bugs with 24/7 fuzzing
**Cost:** Free (Google sponsors)

### Integration

1. **Submit to OSS-Fuzz** (one-time)
   - Fork https://github.com/google/oss-fuzz
   - Add `projects/coinbasechain/`
   - Submit PR

2. **Automatic Fuzzing** (ongoing)
   - Runs 24/7 on Google infrastructure
   - Multiple fuzzing engines (libFuzzer, AFL++, Hongfuzz)
   - Multiple sanitizers
   - Corpus minimization
   - Coverage tracking

3. **Bug Reports** (automatic)
   - Files GitHub issues when crash found
   - Includes reproducer
   - Private disclosure for security bugs

See: https://google.github.io/oss-fuzz/

## Stage 6: Release Pipeline

**Trigger:** Git tag `v*` (e.g., `v1.0.0`)
**Goal:** Production-ready binaries
**Duration:** ~30-45 minutes

### 6.1 Pre-Release Checks

```yaml
- All tests must pass
- No known security issues
- Changelog updated
- Version bumped
```

### 6.2 Build Release Artifacts

**Linux (x86_64):**
```bash
cmake -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON  # LTO

cmake --build build-release -j
strip build-release/bin/coinbasechain

# Create tarball
tar czf coinbasechain-v1.0.0-linux-x86_64.tar.gz \
  -C build-release/bin \
  coinbasechain coinbasechain-cli
```

**Linux (ARM64):**
```bash
# Cross-compile or use ARM runner
cmake -B build-release-arm64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64
```

**macOS (Universal Binary):**
```bash
# Build for both Intel and Apple Silicon
cmake -B build-release-mac \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"
```

**Docker Images:**
```bash
# Multi-arch build
docker buildx build \
  --platform linux/amd64,linux/arm64 \
  --tag coinbasechain/node:v1.0.0 \
  --tag coinbasechain/node:latest \
  --push .
```

### 6.3 Sign Artifacts

```bash
# GPG sign tarballs
gpg --detach-sign --armor coinbasechain-v1.0.0-linux-x86_64.tar.gz

# Generate SHA256 checksums
sha256sum *.tar.gz > SHA256SUMS
gpg --clearsign SHA256SUMS
```

### 6.4 Create GitHub Release

```yaml
- Upload binaries
- Upload checksums
- Upload signatures
- Generate release notes (from CHANGELOG)
- Mark as pre-release if beta
```

### 6.5 Update Package Repositories

```yaml
- Homebrew (macOS)
  - Update formula
  - Submit PR to homebrew-core

- Debian/Ubuntu (PPA)
  - Build .deb packages
  - Upload to Launchpad

- Docker Hub
  - Already pushed in 6.2

- Snap Store
  - Build snap package
  - Push to store
```

## Stage 7: Post-Release Monitoring

### 7.1 Deployment Verification

```yaml
- Deploy to testnet
- Run smoke tests
- Monitor for crashes
- Check performance metrics
```

### 7.2 Security Monitoring

```yaml
- GitHub Security Advisories
- Dependabot alerts
- OSS-Fuzz crash reports
- CVE database monitoring
```

## CI Configuration Files

### Recommended Structure

```
.github/
├── workflows/
│   ├── pr-ci.yml              # Fast PR checks (5-10 min)
│   ├── post-merge.yml         # Extended tests after merge (15-20 min)
│   ├── nightly.yml            # Exhaustive nightly tests (2-4 hours)
│   ├── release.yml            # Build and publish release
│   ├── security.yml           # CodeQL and dependency scanning
│   └── benchmarks.yml         # Performance tracking
├── dependabot.yml             # Automatic dependency updates
└── CODEOWNERS                 # Require reviews from owners
```

## Cost Optimization

### GitHub Actions Free Tier

- **2,000 minutes/month** for public repos (free)
- **20 concurrent jobs** (free)
- macOS uses **10x minutes** (1 min macOS = 10 min Linux)

### Strategy to Stay Within Limits

1. **PR CI:** ~10 min × 20 PRs/month = 200 min
2. **Post-merge:** ~20 min × 50 merges/month = 1,000 min
3. **Nightly:** ~3 hours × 30 days = 5,400 min (need paid plan)

**Optimization:**
- Run nightly on Linux only (cheap)
- Use self-hosted runners for fuzzing
- Run macOS tests only on PR, not nightly
- Cache dependencies aggressively

### Self-Hosted Runners (Optional)

For extensive fuzzing:
- Set up Linux machine
- Run GitHub Actions runner
- Unlimited minutes, but you pay for hardware

## Branch Protection Rules

### `main` Branch

```yaml
Require:
  - All status checks pass
  - At least 1 approval from maintainer
  - Up-to-date with main
  - Signed commits (recommended)

Prohibit:
  - Force pushes
  - Deletions
  - Direct pushes (use PRs)
```

### `develop` Branch (if using)

```yaml
Require:
  - All status checks pass
  - No approval required (faster iteration)

Allow:
  - Direct pushes by maintainers
```

## Testing Philosophy by Stage

| Stage | Focus | Tradeoff |
|-------|-------|----------|
| **Local** | Fast feedback | Limited scope |
| **PR CI** | Core functionality | Speed > coverage |
| **Post-merge** | Extended testing | Moderate coverage |
| **Nightly** | Exhaustive | Time > coverage |
| **OSS-Fuzz** | Continuous fuzzing | Always running |
| **Release** | Production quality | Full validation |

## Metrics to Track

### Code Quality
- Test coverage (target: >80%)
- Passing tests (target: 100%)
- Compiler warnings (target: 0)
- Static analysis issues (target: 0)

### Performance
- Build time (track regressions)
- Test execution time
- Binary size
- Runtime performance benchmarks

### Security
- Open security issues (target: 0)
- Days to fix CVEs (target: <7)
- Fuzzing coverage (edges reached)
- Crash-free rate

## Rollout Plan

### Phase 1: MVP (Week 1)
```yaml
✅ PR CI with basic tests
✅ clang-format check
✅ ASan builds
```

### Phase 2: Extended Testing (Week 2)
```yaml
✅ GCC builds
✅ macOS support
✅ Post-merge CI
✅ Integration tests
```

### Phase 3: Automation (Week 3)
```yaml
✅ Nightly tests
✅ Docker builds
✅ Benchmark tracking
```

### Phase 4: Security (Week 4)
```yaml
✅ OSS-Fuzz integration
✅ CodeQL scanning
✅ Dependabot
✅ Security policy
```

### Phase 5: Release (Week 5)
```yaml
✅ Release pipeline
✅ Binary signing
✅ Package repositories
```

## Example: PR CI Workflow

```yaml
name: Pull Request CI

on:
  pull_request:
    branches: [ main ]

jobs:
  # Fast checks (run first, fail fast)
  formatting:
    name: Code Formatting
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: DoozyX/clang-format-lint-action@v0.17
        with:
          source: 'src include'
          extensions: 'hpp,cpp'
          clangFormatVersion: 18

  # Build matrix (parallel)
  build-test:
    name: ${{ matrix.name }}
    runs-on: ${{ matrix.os }}
    strategy:
      fail-fast: false
      matrix:
        include:
          - name: Linux Clang
            os: ubuntu-22.04
            compiler: clang
            version: 18

          - name: Linux GCC
            os: ubuntu-22.04
            compiler: gcc
            version: 13

          - name: macOS LLVM
            os: macos-14
            compiler: clang

          - name: Linux ASan
            os: ubuntu-22.04
            compiler: clang
            version: 18
            sanitizer: address

    steps:
      - uses: actions/checkout@v4

      - name: Cache dependencies
        uses: actions/cache@v3
        with:
          path: |
            build/_deps
            ~/.hunter
          key: deps-${{ runner.os }}-${{ hashFiles('CMakeLists.txt') }}

      - name: Install dependencies
        run: |
          # ... install Boost, compiler, etc.

      - name: Configure
        run: |
          cmake -B build \
            -DCMAKE_CXX_COMPILER=${{ matrix.compiler }} \
            ${{ matrix.sanitizer && '-DSANITIZE=' + matrix.sanitizer || '' }}

      - name: Build
        run: cmake --build build -j$(nproc)

      - name: Test
        run: ./build/coinbasechain_tests

  # Fuzzing smoke test
  fuzz-smoke:
    name: Fuzz Smoke Test
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4

      - name: Build fuzz targets
        run: |
          cmake -B build-fuzz -DENABLE_FUZZING=ON
          cmake --build build-fuzz -j

      - name: Run fuzz targets (10k each)
        run: |
          ./build-fuzz/fuzz/fuzz_block_header -runs=10000
          ./build-fuzz/fuzz/fuzz_messages -runs=10000
          ./build-fuzz/fuzz/fuzz_chain_reorg -runs=10000

  # All checks must pass
  all-checks:
    name: All Checks Passed
    needs: [formatting, build-test, fuzz-smoke]
    runs-on: ubuntu-latest
    steps:
      - run: echo "All checks passed!"
```

## Summary: What Runs When

| Check | Local | PR | Post-Merge | Nightly | Release |
|-------|-------|----|-----------:|---------|---------|
| **Build (Clang)** | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Build (GCC)** | - | ✅ | ✅ | ✅ | ✅ |
| **Unit tests** | ✅ | ✅ | ✅ | ✅ | ✅ |
| **ASan** | Sometimes | ✅ | ✅ | ✅ | ✅ |
| **UBSan** | - | ✅ | ✅ | ✅ | ✅ |
| **TSan** | - | - | ✅ | ✅ | - |
| **MSan** | - | - | - | ✅ | - |
| **Fuzz (10k)** | - | ✅ | ✅ | - | ✅ |
| **Fuzz (1M)** | - | - | ✅ | - | - |
| **Fuzz (1hr)** | - | - | - | ✅ | - |
| **Fuzz (24/7)** | - | - | - | OSS-Fuzz | - |
| **Integration** | - | - | ✅ | ✅ | ✅ |
| **Benchmarks** | - | - | ✅ | ✅ | - |
| **Static analysis** | - | ✅ (changed) | - | ✅ (full) | - |
| **Coverage** | - | - | ✅ | ✅ | - |

## Next Steps

1. **Create `.github/workflows/pr-ci.yml`** (start here)
2. **Set up branch protection** on `main`
3. **Add post-merge CI** once PR CI stable
4. **Submit to OSS-Fuzz** for continuous fuzzing
5. **Add nightly tests** for exhaustive coverage
6. **Create release pipeline** when ready to ship

This gives you **world-class CI/CD** for a security-critical blockchain project! 🚀
