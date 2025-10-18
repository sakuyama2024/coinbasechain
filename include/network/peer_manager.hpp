#ifndef COINBASECHAIN_PEER_MANAGER_HPP
#define COINBASECHAIN_PEER_MANAGER_HPP

#include "network/addr_manager.hpp"
#include "network/peer.hpp"
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace coinbasechain {
namespace network {

/**
 * PeerManager - Manages the lifecycle of peer connections
 *
 * Simplified version for headers-only blockchain:
 * - Tracks active peer connections
 * - Handles peer addition/removal
 * - Coordinates with AddressManager
 * - Manages connection limits
 * - Handles peer disconnections
 *
 * Note: This is a simplified version. Bitcoin's PeerManager
 * (net_processing.cpp) is much more complex with 82+ methods. We intentionally
 * split responsibilities across multiple components for better modularity.
 *
 * TODO: Features from Bitcoin's PeerManager we may need:
 *
 * 1. Peer State Management (per-peer):
 *    - InitializeNode() / FinalizeNode() callbacks for setup/cleanup
 *    - Per-peer sync state (sync height, common height, in-flight requests)
 *    - Detailed peer statistics (CNodeStateStats)
 *    - Peer service flags tracking
 *
 * 2. Misbehavior & DoS Protection:
 *    - Misbehavior scoring system (discouragement threshold)
 *    - ConsiderEviction() - evict bad peers
 *    - CheckForStaleTipAndEvictPeers() - detect if our tip is stale
 *    - Ban management integration
 *
 * 3. Message Processing (currently handled by individual Peer class):
 *    - ProcessMessages() - message routing and validation
 *    - ProcessMessage() - handle specific message types
 *    - Message rate limiting
 *
 * 4. Address Relay Management:
 *    - AddAddressKnown() - track which addresses sent to which peer
 *    - Rate limiting for ADDR messages (m_addr_processed, m_addr_rate_limited)
 *    - Address relay enable/disable per peer
 *
 * 5. Scheduled Tasks:
 *    - StartScheduledTasks() with CScheduler integration
 *    - SendPings() to all peers periodically
 *    - Periodic eviction checks
 *
 * 6. Validation Interface Integration:
 *    - CValidationInterface inheritance
 *    - SetBestBlock() - update when chain tip changes
 *    - BlockConnected() / BlockDisconnected() callbacks
 *
 * 7. Header/Block Sync Coordination (will be in HeaderSync/BlockSync):
 *    - FetchBlock() - request specific blocks from peer
 *    - FindNextBlocksToDownload() - determine what to sync
 *    - CheckHeadersPoW() / CheckHeadersAreContinuous() - validation
 *    - Track in-flight block/header requests per peer
 *
 * Current design: We split Bitcoin's monolithic PeerManager into:
 * - PeerManager (this) = connection lifecycle only
 * - HeaderSync/BlockSync (Phases 6-7) = sync logic
 * - NetworkManager (Phase 5) = coordination & message routing
 */
class PeerManager {
public:
  struct Config {
    size_t max_outbound_peers;    // Max outbound connections
    size_t max_inbound_peers;     // Max inbound connections
    size_t target_outbound_peers; // Try to maintain this many outbound

    Config()
        : max_outbound_peers(8), max_inbound_peers(125),
          target_outbound_peers(8) {}
  };

  explicit PeerManager(boost::asio::io_context &io_context,
                       AddressManager &addr_manager,
                       const Config &config = Config{});

  ~PeerManager();

  // Add a peer
  bool add_peer(PeerPtr peer);

  // Remove a peer by ID
  void remove_peer(int peer_id);

  // Get a peer by ID
  PeerPtr get_peer(int peer_id);

  // Find peer ID by address:port (thread-safe)
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

  // Try to evict a peer to make room for a new inbound connection
  // Returns true if a peer was evicted
  bool evict_inbound_peer();

  // Disconnect and remove all peers
  void disconnect_all();

  // Process periodic tasks (cleanup, connection maintenance)
  void process_periodic();

  // Set callback for when a peer is removed
  using PeerRemovedCallback = std::function<void(int peer_id)>;
  void set_peer_removed_callback(PeerRemovedCallback callback);

private:
  boost::asio::io_context &io_context_;
  AddressManager &addr_manager_;
  Config config_;

  mutable std::mutex mutex_;
  std::map<int, PeerPtr> peers_;

  PeerRemovedCallback peer_removed_callback_;

  // Get next available peer ID
  int next_peer_id_ = 0;
  int allocate_peer_id();
};

} // namespace network
} // namespace coinbasechain

#endif // COINBASECHAIN_PEER_MANAGER_HPP
