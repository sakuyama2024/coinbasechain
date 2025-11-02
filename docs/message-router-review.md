# MessageRouter: comprehensive review and action plan

## Role and current behavior
- Dispatches P2P messages to managers:
  - VERACK → marks outbound peer as good (AddrMan), sends our GETADDR once per outbound, announces tip.
  - ADDR/GETADDR → discovery policy and replies.
  - INV → BlockRelayManager.
  - GETHEADERS/HEADERS → HeaderSyncManager.
- Maintains transient per‑peer discovery state (echo suppression, recent ring) and clears it on disconnect.

## Bugs and risky behavior
1) GETADDR privacy regression
- Current: reply once every 60s per peer; expected: once per connection to limit scraping.

2) Handshake gating gaps
- Router doesn’t require successfully_connected() before responding to GETADDR/processing ADDR; replying pre‑VERACK should be avoided (defense‑in‑depth).

3) Metadata loss on reconstruction
- ParseAddrKey forces services=NODE_NETWORK and fabricated timestamps for some entries; replies may misreport services/freshness.

4) Potential stale/leaky replies
- GETADDR candidate set pulls from learned maps regardless of age; may over‑share old/unvetted addrs.

5) O(n) ring pruning
- recent_addrs_ is a vector; front erases on overflow lead to quadratic behavior under large ADDR bursts.

6) Unbounded per‑peer learned map
- Only TTL pruning on access; no hard cap or eviction policy → memory growth risk.

7) High‑cost candidate build
- GETADDR unions learned maps across all peers via string keys; grows with peer count and learned size; unnecessary allocation/hashing.

8) Duplication/drift
- MakeAddrKey/ParseAddrKey duplicate AddrInfo::get_key() semantics; risk of inconsistency.

9) Weak observability
- Sparse metrics/logs for GETADDR decisions (gating, suppression, composition); hard to debug privacy behavior.

## Design and layering issues
- Router mixes dispatch with discovery policy and per‑peer caches → per‑peer state scattered.
- String keys for binary address data (IP:port) are wasteful and cause lossy ParseAddrKey.
- Raw pointer deps are non‑owning (fine), but references/not_null would encode intent better.

## Action plan

### P0 — Correctness/privacy
- Change GETADDR to once‑per‑connection (replace time‑based cooldown with a set of peer_ids that have been served).
- Require peer->successfully_connected() before accepting ADDR or replying to GETADDR.
- Preserve metadata: eliminate MakeAddrKey/ParseAddrKey; store full TimestampedAddress in the learned cache; never fabricate services/timestamps.
- Limit leakage: reply primarily from AddrMan; optionally top‑up from a bounded recent ring; always filter requester’s own address and respect echo‑suppression TTL.

### P1 — DoS/performance
- Replace recent_addrs_ vector with std::deque or circular buffer for O(1) eviction.
- Cap learned_addrs_by_peer_ (e.g., 2000 entries per peer) and evict oldest by last_seen; optionally enforce a global cap.
- Restrict candidate universe to a TTL window (only recent learns), avoid union across all peers when not needed.
- Use a binary AddressKey {ip[16], port} + custom hasher; avoid string conversions.

### P2 — Structure/API
- Keep Router thin: move discovery/echo/GETADDR assembly into a small DiscoveryManager owned by NetworkManager (or a helper inside Router if scope must remain local).
- Replace raw manager pointers in MessageRouter ctor with references or gsl::not_null<T*>.
- (Optional) Introduce a shared AddressKey header if AddrMan should also use binary keys in the future.

### P3 — Observability/tests
- Add logs around GETADDR gating and composition: served/ignored (and why), result sizes, addrman vs recent ring counts, suppression hits.
- Unit tests:
  - GETADDR once‑per‑connection; inbound‑only; requires VERACK.
  - Echo suppression TTL; requester self‑address excluded.
  - Metadata preservation: services/timestamps round‑trip ADDR→GETADDR.
  - Memory bounds: learned map caps; recent ring bounded with O(1) eviction.
  - Performance sanity: large ADDR inputs don’t degrade due to front‑erase.
- Fuzz tests for ADDR/GETADDR parsing and edge bounds.

## Implementation sketch (localized, Option A)

Introduce binary key and learned entry (internal to Router):
- struct AddressKey { std::array<uint8_t,16> ip; uint16_t port; Hasher } with == and hash.
- struct LearnedEntry { protocol::TimestampedAddress ts_addr; int64_t last_seen_s; }.

Data structure changes:
- last_getaddr_reply_s_ → std::unordered_set<int> getaddr_replied_.
- learned_addrs_by_peer_ value type → unordered_map<AddressKey, LearnedEntry, Hasher>, with MAX_LEARNED_PER_PEER and TTL pruning + oldest eviction.
- recent_addrs_ → std::deque<protocol::TimestampedAddress> with RECENT_ADDRS_MAX.

handle_addr:
- Under lock, prune TTL; enforce cap by evicting oldest last_seen; insert/refresh entries; push to deque and pop_front when over cap.

handle_getaddr:
- Preconditions: addrman exists, peer inbound, successfully_connected, and not previously served (once‑per‑connection set).
- Build suppression set from the peer’s learned map (copy under lock); compute requester’s self AddressKey.
- Compose reply:
  - Start with AddrMan.get_addresses(MAX_ADDR_SIZE) filtered by echo suppression and self.
  - Top‑up from recent deque (reverse iterate) with same filters; dedupe via unordered_set<AddressKey> included.

Remove MakeAddrKey/ParseAddrKey; never fabricate services/timestamps.

## Alternative (Option B)
- Extract a DiscoveryManager responsible for ADDR/GETADDR policy and per‑peer caches. MessageRouter delegates ADDR/GETADDR to it. Longer‑term, consider unifying address keying with AddrMan via a shared AddressKey.

## Acceptance criteria
- GETADDR replies only once per connection, only after VERACK, inbound‑only.
- Replies preserve original services and timestamps; no reconstruction defaults.
- Memory is bounded under sustained ADDR input; no quadratic vector erases.
- Tests added for all new behaviors; logs make discovery/echo policy traceable.
