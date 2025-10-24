#!/bin/bash
# Memory leak detection for macOS using native tools
# This script runs tests and checks for leaks afterward

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

echo "=================================================="
echo "macOS Memory Leak Detection"
echo "=================================================="
echo ""

if [ ! -f "$BUILD_DIR/coinbasechain_tests" ]; then
    echo "ERROR: Test binary not found. Build first:"
    echo "  cd build && cmake --build . --parallel"
    exit 1
fi

cd "$BUILD_DIR"

# Enable malloc stack logging for leak detection
export MallocStackLogging=1
export MallocStackLoggingNoCompact=1

# Run tests
if [ $# -eq 0 ]; then
    echo "Running all tests..."
    ./coinbasechain_tests 2>&1 | tee test_output.log
else
    echo "Running tests: $@"
    ./coinbasechain_tests "$@" 2>&1 | tee test_output.log
fi

TEST_EXIT_CODE=${PIPESTATUS[0]}

echo ""
echo "=================================================="
echo "Test run complete (exit code: $TEST_EXIT_CODE)"
echo ""
echo "To check for leaks in the last run, use:"
echo "  leaks coinbasechain_tests"
echo ""
echo "Or check a specific test run by finding its PID"
echo "and using: leaks <PID>"
echo "=================================================="

exit $TEST_EXIT_CODE
