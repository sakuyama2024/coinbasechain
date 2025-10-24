#!/bin/bash
# Run tests with macOS Instruments Leaks tool
# This is the native macOS leak detector

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

echo "=================================================="
echo "Running tests with macOS Leaks Tool"
echo "=================================================="
echo ""

if [ ! -f "$BUILD_DIR/coinbasechain_tests" ]; then
    echo "ERROR: Test binary not found. Build first:"
    echo "  cd build && cmake --build . --parallel"
    exit 1
fi

cd "$BUILD_DIR"

# Check if leaks command is available
if ! command -v leaks &> /dev/null; then
    echo "ERROR: 'leaks' command not found."
    echo "Install Xcode Command Line Tools: xcode-select --install"
    exit 1
fi

# Run tests
if [ $# -eq 0 ]; then
    echo "Running all tests with leak detection..."
    TEST_ARGS=""
else
    echo "Running tests: $@"
    TEST_ARGS="$@"
fi

# Run the test binary and capture its PID
echo "Starting tests..."
./coinbasechain_tests $TEST_ARGS &
TEST_PID=$!

# Wait a bit for tests to start
sleep 2

# Monitor for leaks while tests are running
echo "Monitoring for leaks (PID: $TEST_PID)..."
leaks --atExit -- $TEST_PID

# Wait for tests to complete
wait $TEST_PID
TEST_EXIT_CODE=$?

echo ""
echo "=================================================="
echo "Leak check complete!"
echo "Exit code: $TEST_EXIT_CODE"
echo "=================================================="

exit $TEST_EXIT_CODE
