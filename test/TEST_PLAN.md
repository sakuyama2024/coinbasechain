# test2 Networking Test Plan

This plan organizes comprehensive networking tests for the new simulated framework (SimulatedNetwork + SimulatedNode + NetworkBridgedTransport) with deterministic seeds.

## Foundations
- Handshake & lifecycle (handshake_tests.cpp) [network][handshake]
  - VERSION must be first; duplicate VERACK ignored
  - Reject obsolete version (< MIN_PROTOCOL_VERSION)
  - Self-connection via nonce rejected (inbound)
  - Handshake timeout enforces disconnect
  - READY state reached for inbound/outbound
  - Feeler completes handshake then disconnects
- Framing/transport invariants (framing_tests.cpp) [network][framing][malformed]
  - Wrong magic -> disconnect
  - Checksum mismatch -> disconnect
  - length > MAX_PROTOCOL_MESSAGE_LENGTH -> disconnect
  - Truncated payload -> disconnect
  - Unknown command ignored
  - Receive buffer flood > DEFAULT_RECV_FLOOD_SIZE -> disconnect
  - Header/payload boundary splits processed correctly

## Header Sync Pipeline
- Initial sync behavior (headers_sync_tests.cpp) [network][sync]
  - Only one sync peer at a time (SetSyncPeer semantics)
  - GETHEADERS locator sizes (empty/small/many) respect MAX_LOCATOR_SZ
  - hash_stop respected
  - Empty HEADERS clears sync peer; full batch requests continuation
  - Duplicate headers benign
  - If last header is on active chain, skip DoS checks for batch
- Continuity & PoW gates (headers_validation_tests.cpp) [network][sync][validation]
  - Non-continuous headers -> penalty
  - Cheap PoW commitment check fail -> penalty
  - Low-work handling: ignore non-full batch; continue when full
  - Orphan acceptance within limits
  - Orphan-limit exceeded -> penalty

## Reorgs and Chain Selection
- Reorg scenarios (reorg_tests.cpp) [network][reorg]
  - 2- and 3-way competition; delayed higher-work arrival
  - Deep reorg behaves correctly
  - Single ActivateBestChain call per batch
  - Peers converge on best work consistently

## Block Relay and Announcements
- Announcement flow (block_announcement_tests.cpp) [network][relay]
  - AnnounceTipToPeer on VERACK
  - Periodic AnnounceTipToAllPeers
  - FlushBlockAnnouncements cadence
  - MAX_BLOCK_RELAY_AGE window respected

## Misbehavior, Permissions, Bans
- Scoring & thresholds (misbehavior_tests.cpp) [network][dos]
  - INVALID_POW, OVERSIZED, NON_CONTINUOUS, TOO_MANY_UNCONNECTING, TOO_MANY_ORPHANS penalties
  - Discourage at threshold (100)
  - NoBan peers tracked but not disconnected
  - Manual peers can be disconnected programmatically
  - PeerManager::ShouldDisconnect logic verified
- BanMan interactions (ban_tests.cpp) [network][ban]
  - Discourage marks address
  - Unban reverses
  - NoBan immunity to discouragement effects

## Address Manager and Discovery
- ADDR/GETADDR (addr_manager_tests2.cpp) [network][addr]
  - GETADDR only answered by inbound
  - ADDR size limit (MAX_ADDR_SIZE)
  - good() on outbound after VERACK
  - cleanup_stale behavior
  - No leakage when empty
- Feeler & anchors (feeler_anchor_tests.cpp) [network][feeler]
  - Feeler connects, marks good/failed, disconnects
  - No outbound slot consumption
  - Anchor load/save if datadir configured (optional)

## Network Conditions and Partitions
- Latency/jitter/loss (conditions_tests.cpp) [network][conditions]
  - Handshake and headers under latency/jitter
  - Message ordering stability via sequence numbers
  - Packet loss resilience (re-requests)
  - Bandwidth shaping influences delivery time as expected
- Partitions (partition_tests.cpp) [network][partition]
  - A|B split blocks propagation
  - Healing resyncs to best work
  - No cross-group delivery while active

## Timeouts and Liveness
- Timers (timeout_tests.cpp) [network][timeouts]
  - Handshake timeout
  - Inactivity timeout; traffic resets timer
  - Ping/pong cycle and ping timeout disconnect
  - No timer thrash on duplicate messages

## Scaling and Limits
- Capacity & eviction (capacity_tests.cpp) [network][scale]
  - Max inbound enforced, eviction preference (worst ping; age tie-break)
  - Outbound cap respected
  - Smoke tests with 20–50 peers
- Resource bounds (limits_tests.cpp) [network][limits]
  - MAX_HEADERS_SIZE, MAX_INV_SIZE, MAX_ADDR_SIZE enforced
  - Incremental allocation uses MAX_VECTOR_ALLOCATE
  - Strings (user_agent) bounded by MAX_SIZE / MAX_SUBVERSION_LENGTH
  - Memory does not spike on adversarial inputs

## Malformed and Fuzz-ish
- Additional malformed (malformed_message_tests.cpp) [network][malformed]
  - VERSION truncated fields
  - INV with bad count / truncated entries
  - GETHEADERS with bad count/hash sizes
  - HEADERS with truncated header
  - Zero-length/garbage command
  - Out-of-order handshake sequences
- Stateful mini-fuzz (state_fuzz_tests.cpp) [network][fuzz][slow]
  - Randomized sequences of VERSION/VERACK/PING under constraints
  - Assert no crashes and proper disconnects

## Determinism and CI
- Determinism (determinism_tests.cpp) [infra][det]
  - Repeat runs with same seeds yield identical outcomes
  - Stable timelines across runs
- Sanitizers (tsan_asan_jobs) [infra]
  - CI jobs with -DSANITIZE=address/thread for subsets: [handshake], [sync], [dos]

## Orchestrator and Helpers
- TestOrchestrator: add WaitForReady(), WaitForBothDisconnected(), ConnectFully(), AdvanceGradually(step_ms, total_ms)
- NetworkObserver: per-test auto timeline dump on failure; filtered dumps by node/event

## Tagging and Runtime Profiles
- Fast: [simple][handshake][malformed][limits] (<1s each)
- Medium: [sync][reorg][dos][addr][timeouts] (1–5s each)
- Slow/stress: [partition][scale][fuzz] (opt-in, nightly)

## Rollout Phases
1. Handshake, framing, malformed, basic sync
2. Reorgs, permissions/misbehavior depth, addr manager, timeouts
3. Conditions/partitions, scaling/eviction, fuzz/stress, sanitizer gates

## Success Criteria
- >90% of networking code paths exercised
- Zero flakes over 100 reruns locally with deterministic seeds
- Sanitizer-clean on Phase 1–2 suites
- Stable runtimes and reproducible outcomes