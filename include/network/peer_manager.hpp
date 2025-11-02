#pragma once

/*
 PeerManager — unified peer lifecycle and misbehavior tracking for CoinbaseChain

 Purpose
 - Maintain a registry of active peer connections (both inbound and outbound)
 - Enforce connection limits (max_inbound, max_outbound, per-IP limits)
 - Track misbehavior scores and apply DoS protection policies
 - Coordinate with AddressManager for connection lifecycle updates (good/failed)
 - Provide peer discovery/eviction logic for connection management

 Key responsibilities
 1. Peer lifecycle: add, remove, lookup by ID or address
 2. Connection policy: limit enforcement, feeler connections, eviction
 3. Misbehavior tracking: score accumulation, thresholds, disconnect decisions
 4. Permission system: NoBan and Manual flags to protect certain connections
 5. Integration: publishes NetworkNotifications for peer events

 Misbehavior system
 - Each peer has a misbehavior score; penalties are applied for protocol violations
 - Threshold: 100 points → automatic disconnect (DISCOURAGEMENT_THRESHOLD)
 - Permission flags can prevent banning (NoBan) or mark manual connections
 - Duplicate-invalid tracking: avoid double-penalizing the same invalid header
 - Unconnecting headers: progressive tracking with max threshold before penalty

 Penalties (from MisbehaviorPenalty namespace)
   INVALID_POW = 100 (instant ban)
   INVALID_HEADER = 100 (instant ban, unless duplicate)
   TOO_MANY_UNCONNECTING = 100 (after MAX_UNCONNECTING_HEADERS threshold)
   TOO_MANY_ORPHANS = 100 (instant ban)
   OVERSIZED_MESSAGE = 20
   NON_CONTINUOUS_HEADERS = 20
   LOW_WORK_HEADERS = 10

 Connection limits
 - max_outbound_peers: default 8 (protocol::DEFAULT_MAX_OUTBOUND_CONNECTIONS)
 - max_inbound_peers: default 125 (protocol::DEFAULT_MAX_INBOUND_CONNECTIONS)
 - target_outbound_peers: attempt to maintain this many outbound connections
 - MAX_INBOUND_PER_IP = 2: per-IP inbound limit to prevent single-host flooding

 Feeler connections
 - Short-lived test connections to validate addresses in the "new" table
 - FEELER_MAX_LIFETIME_SEC = 120: forced removal after 2 minutes
 - Marked as feeler via PeerPtr flags, tracked for cleanup in process_periodic()

 Public API design
 - Report* methods: external code (HeaderSync, message handlers) reports violations
   • ReportInvalidPoW, ReportInvalidHeader, ReportLowWorkHeaders, etc.
   • Each applies the appropriate penalty internally via private Misbehaving()
 - Increment/Reset UnconnectingHeaders: track non-connectable header sequences
 - Query methods: GetMisbehaviorScore(), ShouldDisconnect() for testing/debugging
 - NO direct penalty manipulation from external code; all penalties are internal

 AddressManager integration
 - On successful peer addition: addr_manager_.attempt() is called
 - On peer removal: addr_manager_.good() or failed() based on disconnect reason
 - PeerManager does NOT manage address selection; that's AddressManager's job

 Threading
 - All public methods are thread-safe (protected by mutex_)
 - Shutdown() sets flag to prevent notifications during destruction
 - Uses NetworkNotifications to publish peer disconnect events

 Differences from Bitcoin Core
 - Simpler permission model: only NoBan and Manual flags (no BloomFilter, etc.)
 - No NetGroupManager: per-IP limits only, no ASN-based grouping yet
 - Misbehavior data stored separately from Peer objects for cleaner separation
 - Feeler connections explicitly tracked and aged out (no implicit heuristics)
 - Inbound eviction: simple heuristic (oldest non-protected peer), not Core's
   complex network-diversity preservation logic

 Notes
 - find_peer_by_address() requires exact IP:port match if port != 0
 - evict_inbound_peer() prefers to evict older, non-protected peers first
 - TestOnlySetPeerCreatedAt() is for unit tests to simulate feeler aging
 - process_periodic() should be called regularly (e.g., every 10 seconds) to
   handle feeler cleanup and connection maintenance
*/

#include "network/addr_manager.hpp"
#include "network/peer.hpp"
#include "network/peer_misbehavior.hpp"  // For PeerMisbehaviorData, NetPermissionFlags, etc.
#include "network/peer_state.hpp"
#include "util/threadsafe_containers.hpp"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <atomic>
#include <chrono>

namespace coinbasechain {
namespace network {

// Note: DoS constants, MisbehaviorPenalty, NetPermissionFlags, and PeerMisbehaviorData
// have been moved to peer_misbehavior.hpp to avoid circular dependencies with peer_state.hpp

class PeerManager {
public:
  struct Config {
    size_t max_outbound_peers;    // Max outbound connections
    size_t max_inbound_peers;     // Max inbound connections
    size_t target_outbound_peers; // Try to maintain this many outbound

    Config()
        : max_outbound_peers(protocol::DEFAULT_MAX_OUTBOUND_CONNECTIONS),
          max_inbound_peers(protocol::DEFAULT_MAX_INBOUND_CONNECTIONS),
          target_outbound_peers(protocol::DEFAULT_MAX_OUTBOUND_CONNECTIONS) {}
  };

  explicit PeerManager(boost::asio::io_context &io_context,
                       AddressManager &addr_manager,
                       const Config &config = Config{});

  // Max lifetime for a feeler connection before forced removal (defense-in-depth)
  static constexpr int FEELER_MAX_LIFETIME_SEC = 120;

  ~PeerManager();

  // Shutdown: disable callbacks and mark as shutting down to avoid UAF during destructor
  void Shutdown();

  // Pre-allocate a peer ID (for async connection setup)
  // This allows NetworkManager to create the peer with a known ID before the connection completes
  int allocate_peer_id();

  // Add a peer with pre-allocated ID (for async connection setup)
  // Returns true on success, false on failure
  // Peer's id must match peer_id parameter
  bool add_peer_with_id(int peer_id, PeerPtr peer, NetPermissionFlags permissions = NetPermissionFlags::None,
                        const std::string &address = "");

  // Add a peer (with optional permissions)
  // Returns the assigned peer_id on success, -1 on failure
  int add_peer(PeerPtr peer, NetPermissionFlags permissions = NetPermissionFlags::None,
               const std::string &address = "");

  // Remove a peer by ID
  void remove_peer(int peer_id);

  // Get a peer by ID
  PeerPtr get_peer(int peer_id);

  // Find peer ID by address:port (thread-safe)
  // Contract: if port != 0, requires exact address:port match; returns -1 if no exact match even if IP matches on a different port.
  // Returns -1 if not found
  int find_peer_by_address(const std::string &address, uint16_t port);

  // Get all active peers
  std::vector<PeerPtr> get_all_peers();

  // Get outbound peers only
  std::vector<PeerPtr> get_outbound_peers();

  // Get inbound peers only
  std::vector<PeerPtr> get_inbound_peers();

  // Get count of active peers
  size_t peer_count() const;
  size_t outbound_count() const;
  size_t inbound_count() const;

  // Check if we need more outbound connections
  bool needs_more_outbound() const;

  // Check if we can accept more inbound connections
  bool can_accept_inbound() const;
  
  // Check if we can accept more inbound connections from a specific IP address
  bool can_accept_inbound_from(const std::string& address) const;
  
  // Per-IP inbound limit (policy)
  static constexpr int MAX_INBOUND_PER_IP = 2;

  // Try to evict a peer to make room for a new inbound connection
  // Returns true if a peer was evicted
  bool evict_inbound_peer();

  // Disconnect and remove all peers
  void disconnect_all();

  // Process periodic tasks (cleanup, connection maintenance)
  void process_periodic();

  // Test-only: set a peer's creation time (used to simulate feeler aging)
  void TestOnlySetPeerCreatedAt(int peer_id, std::chrono::steady_clock::time_point tp);

  // === Misbehavior Tracking (Public API) ===
  // These are the ONLY methods that external code (like HeaderSync) should call
  // All penalty application is handled internally

  // Track unconnecting headers from a peer
  void IncrementUnconnectingHeaders(int peer_id);
  void ResetUnconnectingHeaders(int peer_id);

  // Report specific protocol violations (used by message handlers)
  void ReportInvalidPoW(int peer_id);
  void ReportOversizedMessage(int peer_id);
  void ReportNonContinuousHeaders(int peer_id);
  void ReportLowWorkHeaders(int peer_id);
  void ReportInvalidHeader(int peer_id, const std::string &reason);
  void ReportTooManyOrphans(int peer_id);

  // Duplicate-invalid tracking
  void NoteInvalidHeaderHash(int peer_id, const uint256& hash);
  bool HasInvalidHeaderHash(int peer_id, const uint256& hash) const;

  // Query misbehavior state (for testing/debugging)
  int GetMisbehaviorScore(int peer_id) const;
  bool ShouldDisconnect(int peer_id) const;

  // === PerPeerState Accessors (for BlockRelayManager, MessageRouter) ===
  // Thread-safe accessors for consolidated per-peer state

  // Block relay state accessors
  std::optional<uint256> GetLastAnnouncedBlock(int peer_id) const;
  int64_t GetLastAnnounceTime(int peer_id) const;  // Returns 0 if not found
  void SetLastAnnouncedBlock(int peer_id, const uint256& hash, int64_t time_s);
  std::vector<uint256> GetBlocksForInvRelay(int peer_id);
  void AddBlockForInvRelay(int peer_id, const uint256& hash);
  void ClearBlocksForInvRelay(int peer_id);

  // Address discovery state accessors
  bool HasRepliedToGetAddr(int peer_id) const;
  void MarkGetAddrReplied(int peer_id);
  void AddLearnedAddress(int peer_id, const AddressKey& key, const LearnedEntry& entry);
  std::optional<LearnedMap> GetLearnedAddresses(int peer_id) const;
  void ClearLearnedAddresses(int peer_id);
  // In-place modification of learned addresses (for efficient bulk updates)
  template <typename Func>
  void ModifyLearnedAddresses(int peer_id, Func&& modifier) {
    peer_states_.Modify(peer_id, [&](PerPeerState& state) {
      modifier(state.learned_addresses);
    });
  }
  // Get all peers' learned addresses (for iteration in GETADDR fallback)
  std::vector<std::pair<int, LearnedMap>> GetAllLearnedAddresses() const;

private:
  // === Internal Misbehavior Implementation ===
  // These should NEVER be called by external code

  // Record misbehavior for a peer
  // Returns true if peer should be disconnected
  bool Misbehaving(int peer_id, int penalty, const std::string &reason);
  boost::asio::io_context &io_context_;
  AddressManager &addr_manager_;
  Config config_;

  // === State Consolidation ===
  // Unified per-peer state (replaces old peers_, peer_misbehavior_, peer_created_at_ maps)
  // Thread-safe via ThreadSafeMap - no separate mutex needed
  util::ThreadSafeMap<int, PerPeerState> peer_states_;

  // Get next available peer ID
  std::atomic<int> next_peer_id_{0};

  // Shutdown flag to guard callbacks during destruction (atomic for thread-safety)
  std::atomic<bool> shutting_down_{false};
  // In-progress bulk shutdown (disconnect_all); reject add_peer while true (atomic for thread-safety)
  std::atomic<bool> stopping_all_{false};
};

} // namespace network
} // namespace coinbasechain


