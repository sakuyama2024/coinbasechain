# CoinbaseChain network subsystem: components and interactions

Purpose
- Provide P2P connectivity for a headers‑only blockchain: peer discovery, connection lifecycle, handshake, message routing, header sync, block tip announcements, DoS/ban policy, NAT/anchors.

Key components

- Transport (Transport/RealTransport)
  - Async TCP accept/connect, read/write. Exposes callbacks to Peer. NetworkManager owns and drives it (listen, connect).

- Peer (include/network/peer.hpp, src/network/peer.cpp)
  - One TCP connection. States: CONNECTING → CONNECTED → VERSION_SENT → READY.
  - Handles message framing (24‑byte header), checksum, send queue, receive buffer limits.
  - Runs handshake (VERSION/VERACK), ping/pong keepalive, handshake/inactivity timeouts.
  - Delegates parsed messages to NetworkManager via a MessageHandler callback.

- PeerManager
  - Owns all live PeerPtr, assigns IDs, enforces inbound/outbound limits, per‑IP inbound caps, eviction.
  - Tracks permissions (NoBan/Manual) and misbehavior scores (penalties like INVALID_HEADER, OVERSIZED_MESSAGE).
  - Exposes queries (get_all_peers, counts) and a disconnect callback used by other managers for cleanup.

- AddressManager (AddrMan)
  - Global, persistent address book with “new” and “tried” tables.
  - Adds/updates addresses (add/add_multiple), marks attempt/good/failed, selects peers for outbound and feeler connections, returns randomized samples for GETADDR.
  - Saves/loads to disk; purges stale/terrible entries from “new”.

- BanMan
  - Persistent bans and in‑memory discouragement with expiration. Queried on inbound accept; can be invoked by higher‑level managers to discourage/ban misbehaving IPs.

- HeaderSyncManager
  - Header synchronization orchestrator. Picks/maintains a sync peer, sends GETHEADERS, validates HEADERS (PoW/ASERT checks via ChainstateManager), handles orphan limits, stall detection, reselection.
  - Enforces message limits (varints, max headers per message), ignores unsolicited large HEADERS from non‑sync peers.
  - Reports misbehavior via PeerManager and can trigger discouragement/ban via BanMan (passed in at construction).

- BlockRelayManager
  - Handles INV announcements and outbound tip announcements to peers; maintains per‑peer “blocks for INV relay” queue and flushes on schedule.
  - Works with HeaderSyncManager and PeerManager (e.g., avoid spamming peers; announce only recent blocks).

- MessageRouter
  - Thin dispatcher for protocol messages to the right manager:
    - VERACK: triggers addr “good” marking for outbound peers, sends our GETADDR once per outbound, announces tip.
    - ADDR: feeds AddressManager; records transient per‑peer “learned” set to avoid echo.
    - GETADDR: inbound‑only, rate‑limited (current: 60s cool‑down), replies using AddrMan plus a small recent ring, applying echo suppression.
    - INV → BlockRelayManager; GETHEADERS/HEADERS → HeaderSyncManager.
  - Maintains small, transient per‑peer caches (echo suppression map, recent addresses ring) and clears them on disconnect.

- AnchorManager
  - Persists last good outbound peers to disk and re‑dials them on startup for eclipse resistance.

- NATManager
  - Optional UPnP port mapping for inbound reachability; started when listener is active.

- Protocol/message layer (protocol.hpp, message.hpp/.cpp)
  - Wire constants (magic, command names, size limits), MessageHeader, NetworkAddress, InventoryVector.
  - Serialization with VarInt, header checksum, and specific payload types (VERSION/VERACK/PING/PONG/ADDR/GETADDR/INV/GETHEADERS/HEADERS).

Ownership and lifecycle

- NetworkManager is the orchestrator; it owns: Transport, AddressManager, PeerManager, BanMan, NATManager, AnchorManager, HeaderSyncManager, BlockRelayManager, MessageRouter, and a (possibly internal) io_context thread pool.
- Inbound flow: Transport.accept → NetworkManager validates (BanMan, inbound limits) → creates inbound Peer, installs handler, starts peer, adds to PeerManager.
- Outbound flow: timers trigger attempt_outbound_connections → AddressManager.select → Transport.connect → Peer created and added; on TCP success Peer.start sends VERSION.
- Peer message dispatch: Peer parses messages and calls NetworkManager::handle_message → NetworkManager forwards to MessageRouter (and runs self‑connection check on inbound VERSION) → Router calls specific managers.
- Disconnect: PeerManager removes peer and triggers a disconnect callback; HeaderSyncManager, BlockRelayManager, and MessageRouter clean per‑peer state.

Message routing summary

- VERSION/VERACK: handled in Peer; NetworkManager checks self‑connection on inbound VERSION; Router reacts to VERACK (mark addr good, GETADDR on outbound, tip announce).
- ADDR/GETADDR: Router handles; AddrMan stores addresses; Router applies echo suppression and rate‑limits GETADDR replies.
- INV: BlockRelayManager; builds GETHEADERS follow‑ups or queues announcements.
- GETHEADERS/HEADERS: HeaderSyncManager; validation and chain advancement.
- PING/PONG: Peer handles internally.

Schedulers and timeouts

- NetworkManager timers: outbound connect attempts, maintenance (cleanup, BanMan sweep, sync timers, tip announce), feeler (Poisson), sendmessages (flush block announcements).
- Peer timers: handshake timeout, periodic PING, inactivity timeout.
- HeaderSyncManager: stall detection and sync peer reselection.

Why MessageRouter owns discovery policy (and not AddrMan)
- AddrMan is a global, persistent store. Per‑peer echo suppression, once‑per‑connection GETADDR gating, and “recently learned” caches are transient, per‑connection privacy/DoS policy—best kept near message handling. If desired, this can be extracted into a small DiscoveryManager used by Router.

Extensibility points

- New P2P commands: add to protocol.hpp, define message type, implement (de)serialization, route in MessageRouter to a dedicated manager, add tests.
- Alternate transports: implement Transport interface; inject into NetworkManager.
- Policy changes (GETADDR privacy, echo suppression): adjust in Router (or a future DiscoveryManager) without touching AddrMan’s persistence logic.
