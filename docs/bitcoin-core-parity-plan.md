# Bitcoin Core Parity — Action Plan

Owner: TBD
Status: Draft

## Goals
- Systematically surface and close behavioral gaps with Bitcoin Core on the P2P/headers-only surface.
- Turn Core behavior into executable, repeatable tests and a living compatibility matrix.

## Reference Version
- Target Bitcoin Core: select and freeze a baseline (default: v27.0). Revisit only on explicit upgrade.

## Methodology (behavior-first)
1. Inventory compatibility surface
   - Messages: version/verack, ping/pong, addr/getaddr, inv/getdata/notfound, headers/getheaders, sendheaders, addrrelay/sendaddrv2 (as applicable), feefilter (N/A here), compact blocks (N/A), tx/block (N/A).
   - Processes: header sync, peer admission/eviction, DoS scoring, address relay, timeouts, trickle/send intervals.
   - Outputs: Compatibility Matrix with status {Match | Deviates | N/A} + links to invariants/tests.

2. Define invariants and acceptance criteria per feature
   - Derive from Core’s source (net_processing.cpp, net.cpp, addrman, protocol.h) and release notes.
   - Capture as bullet invariants (inputs → expected outputs, timing windows, limits, privacy constraints).

3. Static mapping
   - Map invariants to this codebase: entry points, handlers, and data structures (e.g., MessageRouter, HeaderSyncManager, AddrManager, ChainstateManager for headers-only).
   - Produce a traceable table: invariant → file:line anchors.

4. Dynamic black-box parity harness
   - Build a small driver that can speak the subset of the Bitcoin P2P protocol to a target node, script message sequences, and record normalized events.
   - Run identical scenarios against: (A) Bitcoin Core (regtest), (B) this node; diff normalized event streams.

5. Parity tests in-repo
   - Add deterministic tests under test/parity/ with tags [parity][network].
   - Each test enforces the invariant and fails loudly on drift; do not rely on logs for assertions.

6. Trace normalization
   - Standardize log/event schema (timestamp, peerid, dir, command, payload-summary).
   - Provide a small tool to normalize Core and this node logs to the same schema for diffing.

7. Triage & fix loop
   - For each deviation: reproduce, write/extend test, fix, prove green on both static and dynamic checks.

8. Reporting
   - CI job: run parity tests on every PR; publish Compatibility Matrix fragment as an artifact.

## Milestones
- M1: Framework scaffolding
  - Choose Core version; create Compatibility Matrix doc; create parity test folder, tags, and CI job skeleton.
- M2: GETADDR parity (pilot)
- M3: Header sync (headers/getheaders) parity
- M4: INV/GETDATA/NOTFOUND parity
- M5: Peer lifecycle (handshake, timeouts, DoS scoring) parity

## Initial Focus: GETADDR Parity (pilot)

Behavioral baseline to match (from Core):
- Respond to GETADDR only under specific conditions (historically: inbound peers; once per connection).
- Limit count and prefer “terrible” filtering rules; randomize selection; exclude private/unknown according to addr relay policy.
- Enforce a cooldown to avoid repeated/address leakage amplification; respect addr relay gating (addrrelay/sendaddrv2) when applicable to the chosen Core version.

What to verify in this codebase:
- Handler gating: inbound-only (or documented deviation), single-response per connection flag, and cooldown storage (per-conn and/or per-peer).
- Address source: AddrManager query, randomization, cap, filtering.
- Logging: trace event emitted with counts and reasons for suppression.

Tests to add ([parity][network]):
- getaddr_once_per_connection: second GETADDR elicits no ADDR/ADDRV2.
- outbound_getaddr_ignored: GETADDR from our outbound peer yields no response (if matching Core policy).
- cooldown_enforced: disconnect/reconnect within cooldown does not elicit response (document window per Core baseline).
- cap_and_randomize: response size ≤ cap; two runs produce different order/subset with high probability.
- no_addrrelay_no_response: when addr relay isn’t negotiated (if applicable), GETADDR gets no response.

Artifacts:
- docs/compatibility-matrix.md entry for GETADDR: invariants, files, test names, status.

## Deliverables
- docs/compatibility-matrix.md (living table)
- test/parity/* (deterministic parity tests)
- tools/parity/ (harness + log normalizer)
- CI job: parity-tests (runs on PRs)

## Tooling & Setup
- Reference node: bitcoind (regtest), pinned to selected version.
- Harness: small Python or C++ driver to craft messages and capture events; reuse existing simulated network for this node where possible.
- Call graph support: ctags/clangd index to map handlers quickly.

## Success Criteria
- 100% green on defined invariants for the selected surface; no flaky timing tests; Compatibility Matrix shows only intentional deviations.

## Open Questions
- Exact GETADDR cooldown semantics to pin to the chosen Core version (document once confirmed).
- Which Core version to standardize on (v27.0 suggested).
