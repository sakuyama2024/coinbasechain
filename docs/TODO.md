# TODO

## P2P: Near-tip headers sync exception (parity with Bitcoin Core)
- Summary: When our tip is recent (within ~24h), allow multiple peers to announce/sync headers in parallel, and keep the designated sync peer until timeout rather than clearing on partial batches.
- Why: Improves tip convergence latency, reduces dependence on a single peer (better resilience vs stalls/eclipse), and prevents request thrash on sub-max headers batches.
- Current behavior: Single sync peer enforced always; we may ClearSyncPeer on partial batches; no near-tip exception.
- Consequences of missing (non-consensus): slower tip updates, more reliance on one peer, occasional GETHEADERS churn.
- Implementation outline:
  1) HeaderSyncManager::CheckInitialSync: if tip time > now - 24h, skip the single-peer restriction (don’t gate on initial_sync_started_ when near tip).
  2) HandleHeadersMessage: do not ClearSyncPeer when batch is non-empty but < MAX_HEADERS_SIZE; keep fSyncStarted until timeout/empty batch.
  3) Add per-peer INV-triggered getheaders rate-limit/flag to avoid spam re-requests during near-tip.
  4) Keep existing stall timeout logic; only switch peers on timeout or explicit disconnect.
- Tests/acceptance:
  - Simulated near-tip: multiple peers can contribute headers; no duplicate or thrash beyond rate limit.
  - Partial-batch headers do not drop sync peer; only timeout or empty batch ends it.
  - No regression in IBD single-peer behavior.
