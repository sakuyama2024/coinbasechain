---
description: Review code for Bitcoin Core compatibility and best practices
---

# Code Review Standards

Review code against these criteria:

## Memory Safety
- Check for proper RAII (Resource Acquisition Is Initialization)
- Verify no raw pointer ownership (use smart pointers)
- Look for potential memory leaks
- Ensure proper cleanup in destructors

## Thread Safety
- Verify proper mutex locking (no deadlocks)
- Check for race conditions
- Ensure atomic operations where needed
- Validate lock ordering

## Bitcoin Core Compatibility
- Follow Bitcoin Core naming conventions
- Match Bitcoin Core patterns and architecture
- Use similar validation/verification approaches
- Maintain compatibility with Bitcoin Core RPC interface

## Error Handling
- Check for proper error propagation
- Verify all error cases are handled
- Ensure no uncaught exceptions
- Validate input parameters

## Security
- Look for integer overflows/underflows
- Check for buffer overruns
- Verify proper input validation
- Ensure cryptographic operations are correct

## Testing
- Verify test coverage for new code
- Check that tests actually test the behavior
- Ensure edge cases are covered

---

**Code to Review**: {{prompt}}

**Your Actions**:
1. Read the specified code files
2. Analyze against all criteria above
3. Identify issues with severity (Critical/High/Medium/Low)
4. Provide specific line numbers and explanations
5. Suggest fixes with code examples
6. Check for missing tests
