# HeaderSync Manager Review (Headers-only chain)

Date: 2025-11-01
Scope: src/network/header_sync_manager.cpp, include/network/header_sync_manager.hpp, src/network/message_router.cpp, src/network/network_manager.cpp, src/network/peer_manager.cpp, src/chain/chainstate_manager.cpp, src/chain/validation.cpp, include/network/message.hpp, src/network/message.cpp

Summary
- This is a headers-only network. Cross-compatibility with Bitcoin Core wire formats is not a goal. HEADERS messages intentionally omit txcount=0 per header.
- HeaderSyncManager runs a single-sync-peer strategy using outbound peers only (Core parity), and advances the active chain via batch AcceptBlockHeader + ActivateBestChain.
- DoS protections are layered: message-size limits, connectivity check, cheap PoW prefilter, continuity check, and a low-work threshold (disabled during IBD).

End-to-end flow
1) Sync peer selection
   - Driven by NetworkManager (SendMessages cadence): HeaderSyncManager::CheckInitialSync().
   - Picks first eligible outbound peer only (no inbound fallback). Sets peer->set_sync_started(true) and internal sync_peer_id.
   - Sends GETHEADERS built from a locator based on pprev-of-tip to force a non-empty reply.

2) Requesting headers
   - HeaderSyncManager::RequestHeadersFromPeer() composes GetHeadersMessage with locator.vHave and zero hash_stop.
   - HEADERS batches of size 2000 trigger an immediate next GETHEADERS (ShouldRequestMore()).

3) Processing HEADERS
   - IBD gating: Ignore unsolicited large batches from non-sync peers; permit small announcements (≤2 headers) from anyone.
   - Oversize: Reject > MAX_HEADERS_SIZE with misbehavior tracking and possible disconnect.
   - Connectivity: If first header’s prev unknown → increment unconnecting counter but continue; later headers become orphans.
   - Cheap PoW: Commitment-only RandomX prefilter across the batch.
   - Continuity: headers[i].prev == headers[i-1].hash.
   - Low-work: If parent exists and not skip_dos_checks, compute total_work and enforce GetAntiDoSWorkThreshold(tip, params, is_ibd). During IBD, threshold=0 (effectively disabled).
   - Accept & activate: Each header accepted via ChainstateManager::AcceptBlockHeader(min_pow_checked=true), then a single ActivateBestChain() per batch.
   - Post-success: If batch was full (==2000), request more; otherwise keep fSyncStarted until timeout.
   - Error/edge outcomes: For empty batch, ClearSyncPeer(); for invalid/oversized/non-continuous/invalid-PoW, penalize, possibly disconnect, and ClearSyncPeer().

4) Serving GETHEADERS
   - Fork point search considers only active chain. If none match, start from genesis.
   - Respond with up to 2000 active-chain headers after fork point, honoring hash_stop.

5) Stall detection
   - Fixed 120s timeout: if no headers received from sync peer within 120s, disconnect peer; reselection follows cadence.

Verification: code matches intent
- Single sync peer and fSyncStarted lifecycle
  - Set at selection, never cleared on success; cleared on timeout/disconnect/errors; OnPeerDisconnected resets peer->set_sync_started(false). Matches comments.
- IBD large-batch gating (non-sync peers ignored): Implemented with MAX_UNSOLICITED_ANNOUNCEMENT=2 and sync-peer check. Matches comments.
- pprev locator trick: GetLocatorFromPrev() returns locator(tip->pprev) if available, else tip. Matches comments.
- Cheap PoW prefilter before heavy validation: CheckHeadersPoW() called before individual AcceptBlockHeader() calls. Matches comments.
- Orphan handling path: prev-missing batches increment unconnecting counter, continue; AcceptBlockHeader("prev-blk-not-found") leads to AddOrphanHeader(peer_id) with pool limits. Matches comments.
- Low-work gating: CalculateHeadersWork + GetAntiDoSWorkThreshold; disabled (0) during IBD; full vs non-full batch behavior matches documented simplification. Matches comments.
- ActivateBestChain called once per batch: Implemented after loop over headers. Matches comments.
- HEADERS encoding for headers-only chain: message.cpp serializes fixed 100-byte headers, no txcount. Comments explicitly note headers-only. Matches intent (non-Core wire).
- GETHEADERS serving against active chain only; respects MAX_HEADERS_SIZE and hash_stop. Matches comments.
- Stall timeout disconnect with reselection by cadence, not inline reselection. Matches comments.

Deviations from Bitcoin Core (documented, acceptable for headers-only chain)
- HEADERS wire format omits txcount=0 per header.
- Low-work gating disabled during IBD (threshold=0).
- Fixed 120s sync timeout vs Core’s dynamic heuristics.
- No HeadersSyncState; simplified handling of low-work full batches (request-more loop).
- “Skip DoS checks if last header is on active chain” uses active-chain-only predicate (safer than Core’s historic heuristic).

Bugs / risks
- Missing header include: src/network/header_sync_manager.cpp used std::log without <cmath>. Fixed in this change.
- Potential livelock: Repeated full-sized, low-work batches can lead to endless request-more without backoff/escalation. Suggest tracking consecutive occurrences per peer and either (a) reseat sync peer after N, or (b) mark peer for discouragement, or (c) introduce bounded attempts akin to Core.
- Optional robustness: In CheckInitialSync, consider syncing only with peers that completed handshake (peer->successfully_connected()) to reduce wasted requests.
- Fixed stall timeout may be brittle on slow systems; consider param-driven or adaptive timeouts.

Security posture notes
- Heavy RandomX PoW validation is deferred until after: connectivity, cheap-PoW prefilter, continuity, and (when enabled) low-work gate; this meaningfully reduces CPU DoS surface.
- Orphan pool is bounded globally and per-peer with expiry; prev-missing headers accrue misbehavior that can disconnect.
- Active-chain-only fork search for serving avoids exposing side chains in responses.

Suggested follow-ups (optional)
- Add a per-peer counter for consecutive full low-work batches with backoff/reseat policy.
- Gate sync selection on successfully_connected() to avoid sending GETHEADERS before VERACK.
- Make HEADERS stall timeout configurable in chain params.

File references
- HeaderSync core: include/network/header_sync_manager.hpp, src/network/header_sync_manager.cpp
- Routing: include/network/message_router.hpp, src/network/message_router.cpp
- Networking cadence: include/network/network_manager.hpp, src/network/network_manager.cpp
- Peer lifecycle/misbehavior: include/network/peer_manager.hpp, src/network/peer_manager.cpp
- Validation: include/chain/validation.hpp, src/chain/validation.cpp, include/chain/chainstate_manager.hpp, src/chain/chainstate_manager.cpp
- Messages: include/network/message.hpp, src/network/message.cpp
