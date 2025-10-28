#ifndef COINBASECHAIN_BLOCK_RELAY_MANAGER_HPP
#define COINBASECHAIN_BLOCK_RELAY_MANAGER_HPP

#include "network/peer.hpp"
#include "network/peer_manager.hpp"
#include "network/message.hpp"
#include "chain/uint.hpp"
#include <memory>
#include <unordered_map>
#include <mutex>

namespace coinbasechain {

// Forward declarations
namespace validation {
class ChainstateManager;
}

namespace network {

// Forward declaration
class HeaderSyncManager;

// BlockRelayManager - Handles block announcements and relay (inspired by Bitcoin's SendMessages)
// Manages per-peer announcement queues, periodic flushing, and block relay to all peers
class BlockRelayManager {
public:
  BlockRelayManager(validation::ChainstateManager& chainstate,
                    PeerManager& peer_mgr,
                    HeaderSyncManager* header_sync);

  // Announce current tip to all connected peers (adds to their queues)
  void AnnounceTipToAllPeers();

  // Announce current tip to a specific peer (called when peer becomes READY)
  void AnnounceTipToPeer(Peer* peer);

  // Flush pending block announcements from all peers' queues
  // (sends queued blocks as INV messages)
  void FlushBlockAnnouncements();

  // Immediately relay a block to all connected peers (bypass queue)
  void RelayBlock(const uint256& block_hash);

  // Handle incoming INV message from a peer
  bool HandleInvMessage(PeerPtr peer, message::InvMessage* msg);

  // Cleanup hook: called when a peer disconnects
  void OnPeerDisconnected(int peer_id);

private:
  validation::ChainstateManager& chainstate_manager_;
  PeerManager& peer_manager_;
  HeaderSyncManager* header_sync_manager_; // Optional - for INV->GETHEADERS coordination

  // Last announced tip (for tracking)
  uint256 last_announced_tip_;
  // Per-peer last announced block to avoid re-announcing the same tip in tight loops
  std::unordered_map<int, uint256> last_announced_to_peer_;
  // Per-peer last announcement time (unix seconds via util::GetTime)
  std::unordered_map<int, int64_t> last_announce_time_s_;
  // Mutex to guard per-peer announcement tracking
  mutable std::mutex announce_mutex_;
};

} // namespace network
} // namespace coinbasechain

#endif // COINBASECHAIN_BLOCK_RELAY_MANAGER_HPP
