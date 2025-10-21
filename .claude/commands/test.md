---
description: Build and run tests with failure analysis
---

# Test Execution and Analysis

Run tests and analyze results systematically.

## Build Process
1. Clean build if needed
2. Build test target with parallel compilation
3. Report compilation errors clearly

## Test Execution
1. Run specified test suite or all tests
2. Capture full output including failures
3. Report test statistics

## Failure Analysis
For each failure:
- Identify the failing test case
- Extract the assertion that failed
- Show relevant code context
- Suggest potential root causes
- Recommend fixes

## Test Organization
- `[network]` - All network tests
- `[validation]` - Validation tests
- `[rpc]` - RPC tests
- `[nat]` - NAT traversal tests
- `[network_header_sync]` - Header sync tests

---

**Test Request**: {{prompt}}

**Your Actions**:
1. Build the test target (`cmake --build build --target coinbasechain_tests`)
2. Run the specified tests (or all if not specified)
3. Parse and report test results
4. If failures occur:
   - Extract failure details
   - Read relevant source code
   - Analyze the failure cause
   - Suggest specific fixes
5. Report final statistics (passed/failed/total)
