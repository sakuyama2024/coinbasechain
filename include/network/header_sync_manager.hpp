#ifndef COINBASECHAIN_HEADER_SYNC_MANAGER_HPP
#define COINBASECHAIN_HEADER_SYNC_MANAGER_HPP

#include "chain/block.hpp"
#include "network/peer.hpp"
#include "network/message.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <memory>
#include <limits>

namespace coinbasechain {

// Forward declarations
namespace validation {
class ChainstateManager;
}

namespace network {

// Forward declarations
class PeerManager;
class BanMan;

/**
 * HeaderSyncManager - Manages blockchain header synchronization
 *
 * Responsibilities:
 * - Handle incoming headers messages from peers
 * - Request headers from peers during sync
 * - Track sync state (synced, in progress, stalled)
 * - Generate block locators for header requests
 * - Coordinate initial blockchain download (IBD) for headers
 *
 * Extracted from NetworkManager to improve modularity and maintainability.
 * This component focuses solely on header synchronization logic.
 */
class HeaderSyncManager {
public:
  static constexpr uint64_t NO_SYNC_PEER = std::numeric_limits<uint64_t>::max();
  HeaderSyncManager(validation::ChainstateManager& chainstate,
                    PeerManager& peer_mgr,
                    BanMan* ban_man);

  // Message handlers
  bool HandleHeadersMessage(PeerPtr peer, message::HeadersMessage* msg);
  bool HandleGetHeadersMessage(PeerPtr peer, message::GetHeadersMessage* msg);

  // Sync coordination
  void RequestHeadersFromPeer(PeerPtr peer);
  void CheckInitialSync();
  
  // Periodic maintenance (timeouts, retries)
  void ProcessTimers();

  // State queries
  bool IsSynced(int64_t max_age_seconds = 3600) const;
  bool ShouldRequestMore() const;

  // Block locator generation
  CBlockLocator GetLocatorFromPrev() const;

  // Sync tracking
  uint64_t GetSyncPeerId() const { return sync_peer_id_.load(std::memory_order_acquire); }
  bool HasSyncPeer() const { return GetSyncPeerId() != NO_SYNC_PEER; }
  void SetSyncPeer(uint64_t peer_id);
  void ClearSyncPeer();
  
  // Peer lifecycle - called when a peer disconnects
  void OnPeerDisconnected(uint64_t peer_id);

private:
  // Component references
  validation::ChainstateManager& chainstate_manager_;
  PeerManager& peer_manager_;
  BanMan* ban_man_;  // Optional (can be nullptr)

  // Sync state (atomic for thread-safe access)
  std::atomic<uint64_t> sync_peer_id_{NO_SYNC_PEER};  // NO_SYNC_PEER = no sync peer
  std::atomic<int64_t> sync_start_time_{0};  // When did sync start? (microseconds since epoch)
  std::atomic<int64_t> last_headers_received_{0};  // Last time we received headers (microseconds)
  std::atomic<bool> initial_sync_started_{false};   // Prevent fan-out: only start initial sync once
  // Header batch tracking
  mutable std::mutex sync_mutex_;
  size_t last_batch_size_{0};  // Size of last headers batch received
};

} // namespace network
} // namespace coinbasechain

#endif // COINBASECHAIN_HEADER_SYNC_MANAGER_HPP
