---
description: Refactor code following component extraction patterns
---

# Refactoring Guidelines

When refactoring code, follow these principles from our recent NetworkManager refactoring:

## Component Extraction Pattern
1. **Single Responsibility**: Each extracted component should handle ONE domain
2. **Delegation Pattern**: Keep public wrapper methods in original class for API compatibility
3. **Zero Test Breakage**: All tests must pass without modifications
4. **Clean Interfaces**: Use forward declarations and proper dependency injection

## Architecture Standards
- Components should be ~200-500 lines each
- Header files define clear public interfaces
- Implementation files contain all business logic
- Use std::unique_ptr for component ownership

## Bitcoin Core Compatibility
- Follow Bitcoin Core naming conventions (CamelCase for classes)
- Match Bitcoin Core patterns (e.g., CConnman, SendMessages, ProcessMessage)
- Maintain thread-safety with proper locking

## Testing Requirements
- Run all tests after refactoring
- Verify zero regressions
- Check that delegation maintains exact behavior

---

**Task**: {{prompt}}

**Your Actions**:
1. Analyze the target code for extraction opportunities
2. Design the component interface
3. Extract implementation with proper separation
4. Update original class to delegate
5. Build and run all tests
6. Report results with line count changes
