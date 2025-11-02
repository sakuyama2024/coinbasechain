#pragma once

#include "network/addr_manager.hpp"
#include "network/peer.hpp"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <atomic>
#include <chrono>

namespace coinbasechain {
namespace network {

// DoS Protection Constants (from Bitcoin Core)
static constexpr int DISCOURAGEMENT_THRESHOLD = 100;

// Misbehavior penalties
namespace MisbehaviorPenalty {
static constexpr int INVALID_POW = 100;
static constexpr int OVERSIZED_MESSAGE = 20;
static constexpr int NON_CONTINUOUS_HEADERS = 20;
static constexpr int LOW_WORK_HEADERS = 10;
static constexpr int INVALID_HEADER = 100;
static constexpr int TOO_MANY_UNCONNECTING = 100;  // Instant disconnect after threshold
static constexpr int TOO_MANY_ORPHANS = 100;       // Instant disconnect
}

// Maximum unconnecting headers messages before penalty
static constexpr int MAX_UNCONNECTING_HEADERS = 10;

// Permission flags for peer connections
enum class NetPermissionFlags : uint32_t {
  None = 0,
  NoBan = (1U << 0),
  Manual = (1U << 1),
};

inline NetPermissionFlags operator|(NetPermissionFlags a, NetPermissionFlags b) {
  return static_cast<NetPermissionFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline NetPermissionFlags operator&(NetPermissionFlags a, NetPermissionFlags b) {
  return static_cast<NetPermissionFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool HasPermission(NetPermissionFlags flags, NetPermissionFlags check) {
  return (flags & check) == check && static_cast<uint32_t>(check) != 0;
}

// Peer misbehavior tracking data
struct PeerMisbehaviorData {
  int misbehavior_score{0};
  bool should_discourage{false};
  int num_unconnecting_headers_msgs{0};
  bool unconnecting_penalized{false};
  NetPermissionFlags permissions{NetPermissionFlags::None};
  std::string address;
  // Track duplicates of invalid headers reported by this peer to avoid double-penalty
  std::unordered_set<std::string> invalid_header_hashes;
};

/**
 * PeerManager - Unified peer lifecycle and misbehavior tracking
 *
 * Manages both connection lifecycle AND misbehavior/DoS protection:
 * - Tracks active peer connections
 * - Handles peer addition/removal
 * - Manages connection limits
 * - Tracks misbehavior scores
 * - Decides when to disconnect misbehaving peers
 */
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
  
  // Set callback for peer disconnect events (for sync state cleanup)
  void SetPeerDisconnectCallback(std::function<void(int)> callback) {
    peer_disconnect_callback_ = std::move(callback);
  }

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

private:
  // === Internal Misbehavior Implementation ===
  // These should NEVER be called by external code

  // Record misbehavior for a peer
  // Returns true if peer should be disconnected
  bool Misbehaving(int peer_id, int penalty, const std::string &reason);
  boost::asio::io_context &io_context_;
  AddressManager &addr_manager_;
  Config config_;

  mutable std::mutex mutex_;
  std::map<int, PeerPtr> peers_;
  std::map<int, PeerMisbehaviorData> peer_misbehavior_;

  // Get next available peer ID
  std::atomic<int> next_peer_id_{0};
  int allocate_peer_id();
  
  // Callback for peer disconnect events
  std::function<void(int)> peer_disconnect_callback_;

  // Track peer creation times (for feeler lifetime enforcement)
  std::map<int, std::chrono::steady_clock::time_point> peer_created_at_;

  // Shutdown flag to guard callbacks during destruction
  bool shutting_down_{false};
};

} // namespace network
} // namespace coinbasechain


