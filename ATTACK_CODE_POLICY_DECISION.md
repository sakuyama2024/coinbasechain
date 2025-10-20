# Attack Code in Repository - Policy Decision

**Status**: PENDING DECISION
**Created**: 2025-10-19
**Topic**: Should we check in adversarial/attack test code?

## Context

We created tests for **inbound slot exhaustion attack**:
- `test/network/inbound_slot_attack_proof.cpp` - Vulnerability proof tests (4 tests, all PASS)
- `test/network/inbound_slot_defense_tests.cpp` - Defense tests (5 tests, all SKIPPED with [.])

## Key Architectural Consideration

**This is a headers-only chain**, which fundamentally changes the attack impact:

### Traditional Full-Node Bitcoin:
- Block size: ~1-4 MB
- 500,000 blocks = ~500 GB - 2 TB
- IBD takes hours to days
- **Inbound slot exhaustion is devastating** - new nodes can't sync

### Headers-Only Chain:
- Header size: ~80 bytes
- 500,000 headers = ~40 MB
- IBD takes seconds to minutes
- **Inbound slot exhaustion is mostly irrelevant** - sync is fast even with limited connections

### Impact Analysis:

1. **Sync is already fast** - Even with limited connections, downloading 40 MB is trivial
2. **Bandwidth is cheap** - Headers are small, connection quality barely matters
3. **Outbound is enough** - With 8 outbound connections, can sync entire chain in seconds
4. **No disk bottleneck** - Tiny storage requirements

### Simpler Alternatives to Complex Eviction:

```cpp
// Option 1: Increase max inbound (headers are cheap)
static const int MAX_INBOUND_CONNECTIONS = 1000;  // vs Bitcoin's 125

// Option 2: Rely on outbound (you control these)
static const int MAX_OUTBOUND_CONNECTIONS = 16;   // vs Bitcoin's 8

// Option 3: Simple rate limiting
if (new_connections_last_minute > 100) {
    delay_accept();
}
```

## Security Policy Question

**Should we check in attack/vulnerability proof code in public repository?**

### Industry Standard: Bitcoin Core's Approach

**Full transparency** - They check in extensive attack code:
- `denial_of_service_tests.cpp`
- `invalid_block_generation.cpp`
- `orphan_attack_scenarios.cpp`
- All publicly available on GitHub

**Philosophy**: Security through transparency, not obscurity.

### Arguments FOR Including Attack Tests

1. **Protocol is already public** - P2P protocol is documented, attack is trivial:
   ```bash
   # Anyone can do this without our code:
   for i in {1..125}; do nc victim.com 8333 &; done
   ```

2. **Defensive documentation**:
   - Shows we considered this attack
   - Documents why we didn't fix it
   - Prevents accidental regression

3. **Research & audit** - Security researchers can verify:
   - "Is this system actually vulnerable?"
   - "Did they consider attack X?"

4. **No secret exploit** - Not revealing:
   - A 0-day vulnerability
   - Cryptographic weakness
   - Implementation bugs
   - Just testing designed behavior under adversarial load

### Arguments AGAINST Including Attack Tests

1. **Lowers barrier to entry** - Script kiddies can:
   ```bash
   git clone coinbasechain
   ./build/coinbasechain_tests "[vulnerability]"  # Learn attack
   # Modify and weaponize
   ```

2. **Legal/ethical concerns**:
   - Some jurisdictions treat "hacking tools" as illegal
   - Repository could be blocked/flagged

3. **Reputational risk**:
   - "Cryptocurrency Project Publishes DDoS Attack Code"
   - Misunderstood by non-technical audience

4. **Enables automation** - Attacker doesn't need to:
   - Understand the protocol
   - Write test harness
   - Figure out edge cases
   - Just copy-paste our well-commented code

## Policy Options

### Option 1: Bitcoin Core Model (Full Disclosure)

Keep attack code with prominent warnings:

```cpp
// ============================================================================
// SECURITY WARNING: Adversarial Test Code
// ============================================================================
//
// This file contains proof-of-concept attack code for security testing.
//
// LEGAL NOTICE:
// - This code is for defensive security research ONLY
// - Attacking networks you don't own is ILLEGAL
// - Use only on private test networks
//
// WHY THIS CODE EXISTS:
// - Documents considered attack vectors
// - Proves architectural security decisions
// - Enables regression testing
// - Allows independent security verification
//
// ARCHITECTURAL DECISION:
// We do NOT implement defenses against this attack because this is a
// headers-only chain where sync is trivial even with limited connections.
// ============================================================================
```

**Pros**: Transparent, auditable, educational
**Cons**: Could be weaponized

### Option 2: Document-Only (Delete Implementation)

Keep markdown document describing attack, delete code:

```markdown
# Known Attack: Inbound Slot Exhaustion

## Description
An attacker can fill all 125 inbound connection slots...

## Why No Defense
Headers-only chain makes this non-critical because...

## Proof of Concept
(Describe the attack, don't provide code)
```

**Pros**: Documents consideration without weaponizing
**Cons**: Can't regression test, harder to verify claims

### Option 3: Gated Access (Private Security Repo)

- Public repo: Only high-level tests
- Private repo: Attack implementations
- Shared with security researchers on request

**Pros**: Best of both worlds
**Cons**: Adds complexity, security by obscurity

### Option 4: Disabled by Default

```cpp
#ifdef ENABLE_ATTACK_TESTS  // Requires explicit build flag
TEST_CASE("PROOF: Inbound slot exhaustion") {
    // Attack code here
}
#endif
```

**Pros**: Present but not easily runnable
**Cons**: Security by obscurity, inconvenient for audits

## Preliminary Recommendation

**Keep the attack proof tests** (Option 1) because:

1. **Low actual risk** - Attack is trivial to implement independently
2. **High defensive value** - Documents threat model, enables regression testing
3. **Follows industry standard** - Bitcoin Core does this
4. **Headers-only architecture** - Attack has minimal impact anyway

**Next Steps**:

1. Add prominent security warnings to attack test files
2. Delete defense test file (`inbound_slot_defense_tests.cpp`) - complexity we don't need
3. Document architectural decision in code comments
4. Consider what attacks we SHOULD focus on for headers-only chains

## Questions to Resolve

1. **Threat model**: Academic/research vs production financial system?
2. **Liability concerns**: Any legal review needed?
3. **Target audience**: Who is this system for?
4. **Attack priorities**: What attacks DO matter for headers-only chains?
   - Invalid header floods?
   - Malicious reorg attempts?
   - Time-warp attacks?
   - Header spam?

## Current Status

**Files exist but decision pending**:
- ✅ `test/network/inbound_slot_attack_proof.cpp` - Created, compiled, in CMakeLists.txt
- ✅ `test/network/inbound_slot_defense_tests.cpp` - Created, compiled, in CMakeLists.txt
- ⏸️  No security warnings added yet
- ⏸️  No architectural decision documented in code

**Action Required**: Make policy decision before finalizing code.

---

## Reference Links

- Bitcoin Core adversarial tests: https://github.com/bitcoin/bitcoin/tree/master/src/test
- CVE-2012-3789: Bitcoin network flood protection
- Eclipse attack on Bitcoin: https://eprint.iacr.org/2015/263.pdf
