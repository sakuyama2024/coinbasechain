#!/bin/bash
# Run tests with LeakSanitizer to detect memory leaks in production code
# Test infrastructure leaks are suppressed via lsan.supp

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

# LeakSanitizer options
export LSAN_OPTIONS="suppressions=$SCRIPT_DIR/lsan.supp:print_suppressions=0:detect_leaks=1:verbosity=1"

# AddressSanitizer options
export ASAN_OPTIONS="detect_leaks=1:halt_on_error=0:log_path=asan.log"

echo "=================================================="
echo "Running tests with LeakSanitizer"
echo "Suppression file: $SCRIPT_DIR/lsan.supp"
echo "=================================================="
echo ""

# Check if built with sanitizers
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "ERROR: Build directory not found. Run this first:"
    echo "  rm -rf build && mkdir build && cd build"
    echo "  cmake -DSANITIZE=address .."
    echo "  cmake --build . --parallel"
    exit 1
fi

if ! grep -q "SANITIZE.*=address" "$BUILD_DIR/CMakeCache.txt"; then
    echo "WARNING: Build not configured with AddressSanitizer!"
    echo "Rebuild with: cd build && cmake -DSANITIZE=address .. && cmake --build . --parallel"
    echo ""
    read -p "Continue anyway? (y/n) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

cd "$BUILD_DIR"

# Run tests
if [ $# -eq 0 ]; then
    echo "Running all tests..."
    ./coinbasechain_tests
else
    echo "Running tests: $@"
    ./coinbasechain_tests "$@"
fi

echo ""
echo "=================================================="
echo "Leak check complete!"
echo "  - Any leaks reported are in PRODUCTION CODE"
echo "  - Test infrastructure leaks are suppressed"
echo "=================================================="
