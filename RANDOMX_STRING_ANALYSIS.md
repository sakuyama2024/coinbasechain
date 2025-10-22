# RandomX Configuration Analysis: Unicity Compatibility

## Executive Summary

**CRITICAL FINDING:** CoinbaseChain uses a **different RandomX epoch seed string** than Unicity, which means:
- ❌ **Incompatible hashes** - Same block will have different RandomX hashes
- ❌ **Separate networks** - Cannot share mining pools or hashpower
- ❌ **Different PoW results** - Blocks valid on one chain are invalid on the other

**RECOMMENDATION:** Use Unicity's exact seed strings to maintain compatibility.

---

## Current Configuration

### What's Correct ✅

**1. RandomX Library Source**
```cmake
# CMakeLists.txt
FetchContent_Declare(
  randomx
  GIT_REPOSITORY https://github.com/unicitynetwork/RandomX.git
  GIT_TAG        origin/master
)
```
✅ Already using Unicity's RandomX fork

**2. Argon2 Salt (Internal RandomX Configuration)**
```cpp
// build/_deps/randomx-src/src/configuration.h
#define RANDOMX_ARGON_SALT "RandomX-alpha\x01"
```
✅ Correct - inherited from Unicity's fork (patch applied)
✅ Same as Unicity Consensus

---

### What's WRONG ❌

**Epoch Seed String (Application-Level Configuration)**

| Component | CoinbaseChain | Unicity | Compatible? |
|-----------|---------------|---------|-------------|
| **Argon2 Salt** | `"RandomX-alpha\x01"` | `"RandomX-alpha\x01"` | ✅ YES |
| **Normal Seed** | `"CoinbaseChain/RandomX/Epoch/%d"` | `"Scash/RandomX/Epoch/%d"` | ❌ NO |
| **Alpha Seed** | N/A | `"Alpha/RandomX/Epoch/%d"` | ❌ NO |

---

## Technical Details

### Unicity's Implementation

**File:** `unicity-consensus/src/pow.cpp`

```cpp
// Epoch seed strings
static const char *RANDOMX_EPOCH_SEED_STRING = "Scash/RandomX/Epoch/%d";
static const char *RANDOMX_EPOCH_SEED_STRING_ALPHA = "Alpha/RandomX/Epoch/%d";

// Seed hash generation (switches based on mode)
uint256 GetSeedHash(uint32_t nEpoch) {
    std::string s;
    if (g_isAlpha)
         s = strprintf(RANDOMX_EPOCH_SEED_STRING_ALPHA, nEpoch);  // Alpha mode
    else
         s = strprintf(RANDOMX_EPOCH_SEED_STRING, nEpoch);        // Normal mode

    uint256 h1, h2;
    CSHA256().Write((const unsigned char*)s.data(), s.size()).Finalize(h1.begin());
    CSHA256().Write(h1.begin(), 32).Finalize(h2.begin());
    return h2;  // This is the RandomX cache key
}
```

**Key Points:**
- Unicity has TWO seed strings (alpha and normal modes)
- Uses `g_isAlpha` flag to switch between them
- Seed string is hashed with SHA256d to create the RandomX key
- This key initializes the RandomX cache for each epoch

---

### CoinbaseChain's Current Implementation

**File:** `src/chain/randomx_pow.cpp`

```cpp
// Epoch seed string (line 17)
static const char *RANDOMX_EPOCH_SEED_STRING = "CoinbaseChain/RandomX/Epoch/%d";

uint256 GetSeedHash(uint32_t nEpoch) {
  char buffer[128];
  snprintf(buffer, sizeof(buffer), RANDOMX_EPOCH_SEED_STRING, nEpoch);
  std::string s(buffer);

  uint256 h1, h2;
  CSHA256()
      .Write((const unsigned char *)s.data(), s.size())
      .Finalize(h1.begin());
  CSHA256().Write(h1.begin(), 32).Finalize(h2.begin());
  return h2;
}
```

**Problems:**
1. ❌ Uses `"CoinbaseChain/RandomX/Epoch/%d"` instead of Unicity's strings
2. ❌ No alpha mode support
3. ❌ Produces different cache keys than Unicity for the same epoch

---

## Impact Analysis

### Hash Incompatibility Example

**Epoch 1000, Same Block:**

**Unicity (Alpha mode):**
```
Seed String: "Alpha/RandomX/Epoch/1000"
→ SHA256d → Key: 0xabc123...
→ RandomX Cache initialized with Key
→ Block Hash: 0x00001234...
```

**CoinbaseChain (Current):**
```
Seed String: "CoinbaseChain/RandomX/Epoch/1000"
→ SHA256d → Key: 0xdef456... (DIFFERENT!)
→ RandomX Cache initialized with Key (DIFFERENT CACHE!)
→ Block Hash: 0x00005678... (COMPLETELY DIFFERENT!)
```

**Result:** Same block header produces completely different RandomX hashes!

---

## Decision Tree

### Option 1: Share Unicity's Network (RECOMMENDED) ✅

**Goal:** CoinbaseChain and Unicity share the same RandomX configuration

**Changes Required:**
```cpp
// src/chain/randomx_pow.cpp
-static const char *RANDOMX_EPOCH_SEED_STRING = "CoinbaseChain/RandomX/Epoch/%d";
+static const char *RANDOMX_EPOCH_SEED_STRING = "Scash/RandomX/Epoch/%d";
+static const char *RANDOMX_EPOCH_SEED_STRING_ALPHA = "Alpha/RandomX/Epoch/%d";
```

**Benefits:**
- ✅ Compatible RandomX hashes with Unicity
- ✅ Can share mining pools
- ✅ Can share hashpower
- ✅ Blocks are cross-verifiable
- ✅ No need to fork RandomX library

**Drawbacks:**
- ⚠️ Must coordinate with Unicity on alpha mode flag (`g_isAlpha`)
- ⚠️ Tied to Unicity's epoch configuration

**Use Case:** CoinbaseChain is a layer on top of Unicity or shares infrastructure

---

### Option 2: Create Separate Network (NOT RECOMMENDED) ❌

**Goal:** CoinbaseChain has independent RandomX configuration

**Changes Required:**
1. **Fork RandomX library** and change Argon2 salt:
   ```cpp
   -#define RANDOMX_ARGON_SALT "RandomX-alpha\x01"
   +#define RANDOMX_ARGON_SALT "CoinbaseChain\x01"
   ```

2. **Keep separate epoch seed string:**
   ```cpp
   static const char *RANDOMX_EPOCH_SEED_STRING = "CoinbaseChain/RandomX/Epoch/%d";
   ```

3. **Update CMakeLists.txt** to use forked library:
   ```cmake
   GIT_REPOSITORY https://github.com/YOUR-ORG/RandomX.git
   ```

**Benefits:**
- ✅ Complete independence from Unicity
- ✅ Can customize RandomX parameters freely

**Drawbacks:**
- ❌ Must maintain separate RandomX fork
- ❌ Cannot share hashpower with Unicity
- ❌ Separate mining ecosystem
- ❌ More maintenance overhead
- ❌ Miners need different configurations

**Use Case:** CoinbaseChain is completely independent blockchain with no relation to Unicity

---

## Recommendation

### Recommended Approach: **Option 1 (Share Unicity's Configuration)**

**Rationale:**
1. **Already using Unicity's RandomX fork** - keeping it simple
2. **Easier maintenance** - no need to fork and maintain RandomX
3. **Potential for shared infrastructure** - mining pools, tools, etc.
4. **Less code divergence** - easier to track upstream changes

**Implementation Steps:**

1. **Update epoch seed strings:**
   ```cpp
   // src/chain/randomx_pow.cpp (line 17)
   -static const char *RANDOMX_EPOCH_SEED_STRING = "CoinbaseChain/RandomX/Epoch/%d";
   +// Use Unicity's seed strings for compatibility
   +static const char *RANDOMX_EPOCH_SEED_STRING = "Scash/RandomX/Epoch/%d";
   +static const char *RANDOMX_EPOCH_SEED_STRING_ALPHA = "Alpha/RandomX/Epoch/%d";
   ```

2. **Add alpha mode support (if needed):**
   ```cpp
   // Option A: Always use normal mode (Scash)
   uint256 GetSeedHash(uint32_t nEpoch) {
     char buffer[128];
     snprintf(buffer, sizeof(buffer), RANDOMX_EPOCH_SEED_STRING, nEpoch);
     // ... rest of function
   }

   // Option B: Support both modes (add g_isAlpha flag)
   extern bool g_isAlpha;  // Define this based on your needs

   uint256 GetSeedHash(uint32_t nEpoch) {
     char buffer[128];
     const char* seed_str = g_isAlpha ?
         RANDOMX_EPOCH_SEED_STRING_ALPHA : RANDOMX_EPOCH_SEED_STRING;
     snprintf(buffer, sizeof(buffer), seed_str, nEpoch);
     // ... rest of function
   }
   ```

3. **Add comment documenting the choice:**
   ```cpp
   // RandomX epoch seed strings - MUST match Unicity Consensus for compatibility
   // Using Unicity's seed strings ensures:
   // - Same RandomX cache keys
   // - Compatible block hashes
   // - Shared mining infrastructure potential
   static const char *RANDOMX_EPOCH_SEED_STRING = "Scash/RandomX/Epoch/%d";
   static const char *RANDOMX_EPOCH_SEED_STRING_ALPHA = "Alpha/RandomX/Epoch/%d";
   ```

4. **Testing:**
   - Generate test blocks with known epochs
   - Compare RandomX hashes with Unicity
   - Verify cache keys match for same epoch

---

## Summary of RandomX Configuration Layers

| Layer | Parameter | CoinbaseChain (Current) | Unicity | Shared? |
|-------|-----------|------------------------|---------|---------|
| **Library** | Fork Source | `unicitynetwork/RandomX` | `unicitynetwork/RandomX` | ✅ YES |
| **RandomX Internal** | Argon2 Salt | `"RandomX-alpha\x01"` | `"RandomX-alpha\x01"` | ✅ YES |
| **Application** | Normal Seed | `"CoinbaseChain/..."` | `"Scash/..."` | ❌ NO |
| **Application** | Alpha Seed | N/A | `"Alpha/..."` | ❌ NO |

**Bottom Line:**
- Library fork: ✅ Already compatible
- Internal config (Argon2): ✅ Already compatible
- **Application config (Epoch seed): ❌ NEEDS FIX**

---

## Next Steps

1. **Decide:** Share Unicity's network (Option 1) or create separate network (Option 2)?
2. **If Option 1 (Recommended):**
   - Update `RANDOMX_EPOCH_SEED_STRING` to match Unicity
   - Add alpha mode support if needed
   - Test hash compatibility
3. **If Option 2:**
   - Fork RandomX library
   - Change Argon2 salt
   - Update CMakeLists.txt
   - Document divergence

---

## References

- **Unicity RandomX Fork:** https://github.com/unicitynetwork/RandomX
- **Unicity PoW Implementation:** `unicity-consensus/src/pow.cpp`
- **CoinbaseChain PoW Implementation:** `src/chain/randomx_pow.cpp`
- **RandomX Spec:** https://github.com/tevador/RandomX
