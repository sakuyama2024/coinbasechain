# P2P Message Compatibility Matrix

Scope: headers-only surface parity with Bitcoin Core. Status values: Unverified, Matches, Deviates, N/A.
Address relay scope: IPv4/IPv6 only; no ADDRV2 (BIP155) on this network.

## Handshake
- Command: version / verack
  - Intent: Match Core handshake ordering and fields.
  - Invariants:
    - We send `version` first on outbound; we reply `version` on inbound, then `verack` after peer `version`.
    - Single `verack` per-connection; disconnect on malformed/unsupported proto.
    - Respect `sendaddrv2` negotiation if advertised by peer.
  - Implementation:
    - `src/network/peer.cpp` — start() sends VERSION for outbound (lines 112–121); send_version (244–277); handle_version (279–342); version-first enforcement in process_message (444–453); handle_verack (344–378).
    - `src/network/network_manager.cpp` — VERSION self-connection guard in handle_message (858–886).
    - `src/network/message_router.cpp` — handle_verack marks outbound good, sends GETADDR, and announces tip (60–99).
  - Status: Unverified

## Liveness
- Command: ping / pong
  - Intent: Match Core nonce echo, timeouts, and rate-limits.
  - Invariants:
    - On `ping(nonce)`, respond `pong(nonce)`; ignore unsolicited `pong`.
    - Throttle pings we initiate; disconnect on missing responses within timeout.
  - Implementation:
    - `src/network/peer.cpp` — schedule_ping (501–525), send_ping (527–533), PING→PONG auto-reply and PONG handling in process_message/handle_pong (485–493, 535–546), inactivity timeout (560–579).
  - Status: Unverified

## Address Relay
- Command: getaddr → addr/addrv2 (with sendaddrv2)
  - Intent: Match Core’s privacy and DoS protections.
  - Invariants:
    - Respond only under allowed conditions (historically: inbound peers; once per connection).
    - Enforce cooldown; cap result count; randomize; filter per policy; use `addrv2` if negotiated.
  - Implementation:
    - `src/network/message_router.cpp` — handle_getaddr builds ADDR from AddressManager and sends (110–119).
    - `src/network/addr_manager.cpp` — get_addresses caps at MAX_ADDR_SIZE, combines tried/new, shuffles (263–288).
    - Note: No inbound-only gating, once-per-connection flag, or cooldown present; no ADDRV2/sendaddrv2 support.
  - Status: Unverified

- Command: addr / addrv2 (unsolicited relays)
  - Intent: Accept valid peer addresses; apply source bucketing and limits.
  - Invariants:
    - Validate, dedupe, rate-limit; ignore private/non-routable per policy.
  - Implementation:
    - `src/network/message_router.cpp` — handle_addr → AddressManager::add_multiple (101–108).
    - `src/network/addr_manager.cpp` — add_multiple (98–110), add_internal with filtering and terrible/stale checks (65–96), cleanup_stale (305–320).
    - Note: No ADDRV2 parser; only ADDR is supported.
  - Status: Unverified

## Header Synchronization
- Command: getheaders → headers
  - Intent: Match Core locator/stop-hash semantics and limits (≤ 2000 headers).
  - Invariants:
    - Use best-known locator; reply contiguous headers up to stop; handle unknown locator gracefully.
    - Do not send headers after tip unless `sendheaders` dictates announcements policy.
  - Implementation:
    - `src/network/message_router.cpp` — routes GETHEADERS/HEADERS to HeaderSyncManager (45–53, 129–143).
    - `src/network/header_sync_manager.cpp` — RequestHeadersFromPeer (105–133), HandleHeadersMessage (135–434, 401–434), ShouldRequestMore (526–539), GetLocatorFromPrev (541–557), HandleGetHeadersMessage (436–511).
    - `include/network/protocol.hpp` — MAX_HEADERS_SIZE = 2000 (lines ~101–106).
  - Status: Unverified

- Command: sendheaders
  - Intent: After negotiation, announce via `headers` instead of `inv`.
  - Invariants:
    - Persist per-connection flag; respect across reorgs and stalls.
  - Implementation:
    - Not implemented (constant declared in `include/network/protocol.hpp`; no negotiation/handling present).
  - Status: Unverified

## Inventory/Requests
- Command: inv
  - Intent: Limit to objects we support (no tx); align with Core behaviors for blocks/headers announcements as applicable.
  - Invariants:
    - Ignore/ban unknown types; cap inventory.
  - Implementation:
    - `src/network/message_router.cpp` — handle_inv forwards to BlockRelayManager (121–127).
    - `src/network/block_relay_manager.cpp` — HandleInvMessage processes MSG_BLOCK and triggers header requests (150–186); AnnounceTipTo* and FlushBlockAnnouncements assemble/sent INV.
  - Status: Unverified

## Not Supported (intentional)
- Commands: tx, block, mempool, getblocks, feefilter, cmpctblock
  - Intent: N/A for headers-only design; reject/ignore with policy consistent with Core’s handling of unknown/unsupported features.
  - Invariants:
    - Do not advertise or accept transactions or full blocks.
  - Status: N/A

## Next Steps
- Add [parity][network] tests per invariant, starting with GETADDR gating/cooldown and HEADERS limits.
- Record per-command status (Matches/Deviates/N/A) with links to tests and code anchors.

---

## Full Message Index (Bitcoin Core parity surface)

Legend:
- Support: Yes | Type-only (defined, unused) | No (ignored)
- Status: Unverified | Matches | Deviates | N/A (intentional)

Handshake/control
- version — Support: Yes; Status: Matches
  - Impl: src/network/peer.cpp (start 112–121, 244–277, 279–342, 444–453), src/network/network_manager.cpp (858–886)
  - Notes: Omits optional 'relay' boolean; Core tolerates missing trailing fields. addr_from set to empty, which Core commonly does.
- verack — Support: Yes; Status: Matches
  - Impl: src/network/peer.cpp (344–378), src/network/message_router.cpp (60–99)
- sendheaders — Support: No (constant only); Status: N/A (optimization)
  - Impl: Declared in include/network/protocol.hpp (61); no negotiation/handling; default announcements via INV.
- wtxidrelay — Support: No; Status: N/A (no transactions)

Liveness
- ping — Support: Yes; Status: Matches
  - Impl: src/network/peer.cpp (501–533), process_message auto-reply (485–493)
- pong — Support: Yes; Status: Matches
  - Impl: src/network/peer.cpp (535–546)

Address relay
- getaddr — Support: Yes; Status: Deviates
  - Impl: src/network/message_router.cpp (110–119); Address source in src/network/addr_manager.cpp (263–288)
  - Notes: No inbound-only gating; no once-per-connection; no cooldown; responds with ADDR (ADDR v1 only by design).
- addr — Support: Yes; Status: Deviates
  - Impl: src/network/message_router.cpp (101–108); storage in src/network/addr_manager.cpp (98–110, 65–96)
  - Notes: ADDR v1 only; no addrv2; no filtering of private/non-routable; basic dedupe only.
- addrv2 — Support: No; Status: N/A (v2 address relay not used; IPv4/IPv6 only)
- sendaddrv2 — Support: No; Status: N/A (no v2 negotiation on this network)

Headers sync
- getheaders — Support: Yes; Status: Matches
  - Impl: src/network/message_router.cpp (50–53, 137–143); src/network/header_sync_manager.cpp (105–133, 436–511)
  - Notes: Respects hash_stop (0 means no limit); caps at 2000 and stops at tip.
- headers — Support: Yes; Status: Matches
  - Impl: src/network/message_router.cpp (45–48, 129–135); src/network/header_sync_manager.cpp (135–434)

Inventory/request
- inv — Support: Yes; Status: Matches
  - Impl: src/network/message_router.cpp (40–43, 121–127); src/network/block_relay_manager.cpp (150–186)
  - Notes: Only MSG_BLOCK is used (headers-first); Core supports more types.

Blocks/transactions (intentional not supported)
- tx, block — Support: No; Status: N/A (headers-only)
- getblocks — Support: No; Status: N/A (use getheaders)
- mempool — Support: No; Status: N/A
- feefilter — Support: No; Status: N/A
- sendcmpct, cmpctblock, getblocktxn, blocktxn — Support: No; Status: N/A

Bloom/filters (legacy)
- filterload, filteradd, filterclear, merkleblock — Support: No; Status: N/A

Deprecated/other
- reject — Support: No; Status: N/A (deprecated in Core)
