#include "network/block_relay_manager.hpp"
#include "network/peer_manager.hpp"
#include "network/header_sync_manager.hpp"
#include "chain/chainstate_manager.hpp"
#include "chain/logging.hpp"
#include "network/protocol.hpp"
#include <algorithm>
#include <cstring>

namespace coinbasechain {
namespace network {

BlockRelayManager::BlockRelayManager(validation::ChainstateManager& chainstate,
                                     PeerManager& peer_mgr,
                                     HeaderSyncManager* header_sync)
    : chainstate_manager_(chainstate),
      peer_manager_(peer_mgr),
      header_sync_manager_(header_sync) {}

void BlockRelayManager::AnnounceTipToAllPeers() {
  // Periodic re-announcement to all connected peers
  // This is called from run_maintenance() every 30 seconds
  // IMPORTANT: Bitcoin re-announces periodically even if tip unchanged to handle partition healing

  const chain::CBlockIndex *tip = chainstate_manager_.GetTip();
  if (!tip || tip->nHeight == 0) {
    return; // No tip to announce
  }

  uint256 current_tip_hash = tip->GetBlockHash();

  LOG_NET_DEBUG("Adding tip to all peers' announcement queues (height={}, hash={})", tip->nHeight,
                current_tip_hash.GetHex().substr(0, 16));

  last_announced_tip_ = current_tip_hash;

  // Add to all ready peers' announcement queues with per-peer deduplication
  // (Bitcoin per-peer approach: don't send same hash twice to same peer)
  auto all_peers = peer_manager_.get_all_peers();
  for (const auto &peer : all_peers) {
    if (peer && peer->is_connected() && peer->state() == PeerState::READY) {
      std::lock_guard<std::mutex> lock(peer->block_inv_mutex_);

      // Only add if not already in this peer's queue (per-peer deduplication)
      auto& queue = peer->blocks_for_inv_relay_;
      if (std::find(queue.begin(), queue.end(), current_tip_hash) == queue.end()) {
        queue.push_back(current_tip_hash);
      }
    }
  }
}

void BlockRelayManager::AnnounceTipToPeer(Peer* peer) {
  // Announce current tip to a single peer (called when peer becomes READY)
  // Bitcoin does this per-peer - adds to peer's announcement queue

  if (!peer || !peer->is_connected() || peer->state() != PeerState::READY) {
    return;
  }

  const chain::CBlockIndex *tip = chainstate_manager_.GetTip();
  if (!tip || tip->nHeight == 0) {
    return;
  }

  uint256 current_tip_hash = tip->GetBlockHash();

  LOG_NET_DEBUG("Adding tip to peer {} announcement queue (height={}, hash={})",
                peer->id(), tip->nHeight, current_tip_hash.GetHex().substr(0, 16));

  // Add to peer's announcement queue (like Bitcoin's m_blocks_for_inv_relay)
  std::lock_guard<std::mutex> lock(peer->block_inv_mutex_);

  // Only add if not already in this peer's queue (per-peer deduplication)
  auto& queue = peer->blocks_for_inv_relay_;
  if (std::find(queue.begin(), queue.end(), current_tip_hash) == queue.end()) {
    queue.push_back(current_tip_hash);
  }
}

void BlockRelayManager::FlushBlockAnnouncements() {
  // Flush pending block announcements from all peers' queues
  // This is called periodically (like Bitcoin's SendMessages loop)
  auto all_peers = peer_manager_.get_all_peers();

  LOG_NET_TRACE("FlushBlockAnnouncements: checking {} peers", all_peers.size());

  for (const auto &peer : all_peers) {
    if (!peer || !peer->is_connected() || peer->state() != PeerState::READY) {
      continue;
    }

    // Get and clear this peer's pending blocks
    std::vector<uint256> blocks_to_announce;
    {
      std::lock_guard<std::mutex> lock(peer->block_inv_mutex_);
      if (peer->blocks_for_inv_relay_.empty()) {
        continue;
      }
      blocks_to_announce = std::move(peer->blocks_for_inv_relay_);
      peer->blocks_for_inv_relay_.clear();
    }

    // Create and send INV message with pending blocks
    auto inv_msg = std::make_unique<message::InvMessage>();
    for (const auto& block_hash : blocks_to_announce) {
      protocol::InventoryVector inv;
      inv.type = protocol::InventoryType::MSG_BLOCK;
      std::memcpy(inv.hash.data(), block_hash.data(), 32);
      inv_msg->inventory.push_back(inv);
    }

    LOG_NET_DEBUG("Flushing {} block announcement(s) to peer {}",
                  blocks_to_announce.size(), peer->id());
    peer->send_message(std::move(inv_msg));
  }
}

void BlockRelayManager::RelayBlock(const uint256 &block_hash) {
  // Create INV message with the new block
  auto inv_msg = std::make_unique<message::InvMessage>();

  protocol::InventoryVector inv;
  inv.type = protocol::InventoryType::MSG_BLOCK;
  std::memcpy(inv.hash.data(), block_hash.data(), 32);

  inv_msg->inventory.push_back(inv);

  LOG_NET_INFO("Relaying block {} to {} peers", block_hash.GetHex(),
               peer_manager_.peer_count());

  // Send to all connected peers
  auto all_peers = peer_manager_.get_all_peers();
  int ready_count = 0;
  int sent_count = 0;
  for (const auto &peer : all_peers) {
    if (peer && peer->is_connected()) {
      if (peer->state() == PeerState::READY) {
        ready_count++;
        // Clone the message for each peer
        auto msg_copy = std::make_unique<message::InvMessage>();
        msg_copy->inventory = inv_msg->inventory;
        peer->send_message(std::move(msg_copy));
        sent_count++;
      }
    }
  }
}

bool BlockRelayManager::HandleInvMessage(PeerPtr peer,
                                         message::InvMessage *msg) {
  if (!peer || !msg) {
    return false;
  }

  LOG_NET_TRACE("Received INV with {} items from peer {}",
                msg->inventory.size(), peer->id());

  // Process each inventory item
  for (const auto &inv : msg->inventory) {
    if (inv.type == protocol::InventoryType::MSG_BLOCK) {
      // Convert array to uint256
      uint256 block_hash;
      std::memcpy(block_hash.data(), inv.hash.data(), 32);

      LOG_NET_DEBUG("Peer {} announced block: {}", peer->id(),
                    block_hash.GetHex());

      // Check if we already have this block
      const chain::CBlockIndex *pindex =
          chainstate_manager_.LookupBlockIndex(block_hash);
      if (pindex) {
        LOG_NET_DEBUG("Already have block {}", block_hash.GetHex());
        continue;
      }

      // Request headers to get this new block
      // Since this is headers-only, we request the header via GETHEADERS
      // BUT during initial sync, only request from the designated sync peer
      if (header_sync_manager_) {
        uint64_t sync_id = header_sync_manager_->GetSyncPeerId();
        if (sync_id != 0) {
          if (sync_id == static_cast<uint64_t>(peer->id())) {
            header_sync_manager_->RequestHeadersFromPeer(peer);
          } else {
            // Ignore INV-driven requests from non-sync peers during initial sync
          }
        } else {
          // Initial sync not yet started: select this peer as sync source and request headers
          header_sync_manager_->SetSyncPeer(peer->id());
          header_sync_manager_->RequestHeadersFromPeer(peer);
        }
      }
    }
  }

  return true;
}

} // namespace network
} // namespace coinbasechain
