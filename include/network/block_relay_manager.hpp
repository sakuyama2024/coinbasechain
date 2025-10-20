#ifndef COINBASECHAIN_BLOCK_RELAY_MANAGER_HPP
#define COINBASECHAIN_BLOCK_RELAY_MANAGER_HPP

#include "network/peer.hpp"
#include "network/peer_manager.hpp"
#include "network/message.hpp"
#include "chain/uint.hpp"
#include <memory>

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

private:
  validation::ChainstateManager& chainstate_manager_;
  PeerManager& peer_manager_;
  HeaderSyncManager* header_sync_manager_; // Optional - for INV->GETHEADERS coordination

  // Last announced tip (for tracking, not for deduplication)
  uint256 last_announced_tip_;
};

} // namespace network
} // namespace coinbasechain

#endif // COINBASECHAIN_BLOCK_RELAY_MANAGER_HPP
