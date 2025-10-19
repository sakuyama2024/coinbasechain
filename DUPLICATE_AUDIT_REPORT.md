# Codebase Duplicate Audit Report
**Date:** October 18, 2025
**Scope:** src/ and include/ directories

## Executive Summary

This audit identified several categories of duplicate code that should be consolidated:

1. **Enum Definitions**: POWVerifyMode defined 3 times (2 forward declarations + 1 actual enum)
2. **Constants**: MAX_HEADERS_RESULTS defined 3 times with value 2000
3. **Functions**: Startup banner defined 2 times (FIXED - removed from logging.cpp)

---

## 1. DUPLICATE ENUM: POWVerifyMode

### Current State (DUPLICATED)

**Location 1:** `include/consensus/pow.hpp:20`
```cpp
enum class POWVerifyMode;  // Forward declaration
```

**Location 2:** `include/crypto/randomx_pow.hpp:27`
```cpp
enum class POWVerifyMode {
  FULL = 0,        // Verify both RandomX hash and commitment
  COMMITMENT_ONLY, // Only verify commitment (faster, for header sync)
  MINING           // Calculate hash and commitment (for miners)
};
```

**Location 3:** `include/validation/chainstate_manager.hpp:25`
```cpp
enum class POWVerifyMode;  // Forward declaration
```

### Problem
- The actual enum definition is in `crypto/randomx_pow.hpp`
- Two other files have forward declarations of the same enum
- This creates ambiguity about which namespace owns this type
- Forward declarations are in different namespaces (consensus vs validation)

### Recommended Fix
1. **Move** the canonical enum definition to `include/consensus/pow.hpp` (most logical home)
2. **Remove** forward declarations from other headers
3. **Update** all files to use `consensus::POWVerifyMode`

---

## 2. DUPLICATE CONSTANT: MAX_HEADERS_RESULTS

### Status: ✅ FIXED

**Previously duplicated in 3 locations:**
- `include/network/protocol.hpp:101` - `MAX_HEADERS_SIZE` (CANONICAL - KEPT)
- `include/sync/header_sync.hpp:93` - `MAX_HEADERS_RESULTS` (REMOVED)
- `include/validation/validation.hpp:124-125` - `MAX_HEADERS_RESULTS` (REMOVED)

**Actions Taken:**
1. ✅ Removed duplicate from `sync/header_sync.hpp`
2. ✅ Removed duplicate from `validation/validation.hpp`
3. ✅ Updated `src/sync/header_sync.cpp` to use `protocol::MAX_HEADERS_SIZE` (3 locations)
4. ✅ Added `#include "network/protocol.hpp"` to header_sync.cpp
5. ✅ Updated test files:
   - `test/validation_tests.cpp` - Added include, updated constant reference
   - `test/orphan_security_tests.cpp` - Added include, updated constant reference

**Consolidation Result:**
- All code now references single canonical constant: `protocol::MAX_HEADERS_SIZE`
- Consistent type: `uint32_t` (matches Bitcoin Core protocol requirements)
- No more type mismatches or naming inconsistencies

---

## 3. DUPLICATE FUNCTION: Startup Banner

### Status: ✅ FIXED

**Previously duplicated:**
- `include/version.hpp:53` - `GetStartupBanner()` (CANONICAL - KEPT)
- `src/util/logging.cpp:71` - Hardcoded banner (REMOVED)

**Action Taken:** Removed duplicate banner from logging.cpp

---

## 4. Additional Duplicates Found

### PROTOCOL_VERSION References

### Status: ✅ FIXED

**Previously duplicated:**
- `include/version.hpp:25` - `constexpr int PROTOCOL_VERSION = 1;` (REMOVED)
- `include/network/protocol.hpp:13` - Forwarded from version.hpp (CHANGED TO CANONICAL)

**Actions Taken:**
1. ✅ Moved canonical definition to `protocol.hpp:13` (where it logically belongs with other protocol constants)
2. ✅ Removed duplicate from `version.hpp` (protocol version is not a client version constant)
3. ✅ Changed type to `uint32_t` for consistency with protocol requirements

**Consolidation Result:**
- Single definition: `protocol::PROTOCOL_VERSION` (uint32_t = 1)
- All usage via `protocol::PROTOCOL_VERSION` (verified 4 usage sites)
- No type mismatches

---

## 5. Code Quality Issues

### Different Types for Same Constant

The `MAX_HEADERS_RESULTS` constant uses three different integer types:
- `uint32_t` (protocol.hpp)
- `size_t` (header_sync.hpp)
- `unsigned int` (validation.hpp)

**Recommendation:** Standardize on `uint32_t` to match Bitcoin Core conventions and network protocol requirements.

---

## Implementation Plan

### Priority 1 (High Impact - Do First)
1. ✅ Remove duplicate startup banner (DONE)
2. ✅ Consolidate MAX_HEADERS constants (DONE)
3. Consolidate POWVerifyMode enum

### Priority 2 (Medium Impact)
4. ✅ Fix PROTOCOL_VERSION type mismatch (DONE)
5. Audit for other duplicate constants

### Priority 3 (Code Quality)
6. Document canonical locations for shared constants
7. Add static_assert checks to prevent future divergence

---

## Methodology for Future Audits

### Automated Search Patterns
```bash
# Find duplicate enum definitions
grep -rn "^[[:space:]]*enum class" include/ src/

# Find duplicate constant definitions
grep -rn "constexpr.*=" include/ | sort -t: -k3

# Find duplicate function names
grep -rn "^[[:space:]]*inline.*(" include/
```

### Manual Review Checklist
- [ ] Search for magic numbers (0x... constants)
- [ ] Search for timeout/interval constants
- [ ] Search for MAX_/MIN_ prefix constants
- [ ] Search for enum definitions
- [ ] Search for typedef/using declarations
- [ ] Check for duplicate implementations in .cpp files

