#include "network/block_relay_manager.hpp"
#include "network/peer_manager.hpp"
#include "network/header_sync_manager.hpp"
#include "chain/chainstate_manager.hpp"
#include "chain/logging.hpp"
#include "network/protocol.hpp"
#include <algorithm>
#include <cstring>
#include <chrono>

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

  // Time now (steady clock, microseconds)
  const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  // Re-announce interval (10 minutes)
  static constexpr int64_t REANNOUNCE_INTERVAL_US = 10LL * 60 * 1000 * 1000;

  // Add to all ready peers' announcement queues with per-peer deduplication + TTL
  auto all_peers = peer_manager_.get_all_peers();
  for (const auto &peer : all_peers) {
    if (peer && peer->is_connected() && peer->state() == PeerState::READY) {
      bool should_enqueue = true;
      {
        std::lock_guard<std::mutex> guard(announce_mutex_);
        auto it_hash = last_announced_to_peer_.find(peer->id());
        auto it_time = last_announce_time_us_.find(peer->id());
        const bool same_tip = (it_hash != last_announced_to_peer_.end() && it_hash->second == current_tip_hash);
        const bool have_time = (it_time != last_announce_time_us_.end());
        const bool within_ttl = have_time && (now_us - it_time->second < REANNOUNCE_INTERVAL_US);
        if (same_tip && within_ttl) {
          should_enqueue = false; // Skip frequent re-announcements of same tip
        }
      }
      if (!should_enqueue) {
        continue;
      }

      std::lock_guard<std::mutex> lock(peer->block_inv_mutex_);

      // Only add if not already in this peer's queue (per-peer deduplication)
      auto& queue = peer->blocks_for_inv_relay_;
      if (std::find(queue.begin(), queue.end(), current_tip_hash) == queue.end()) {
        queue.push_back(current_tip_hash);
        // Record last announcement (hash + time)
        std::lock_guard<std::mutex> guard(announce_mutex_);
        last_announced_to_peer_[peer->id()] = current_tip_hash;
        last_announce_time_us_[peer->id()] = now_us;
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

  // Time now (steady clock, microseconds)
  const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  static constexpr int64_t REANNOUNCE_INTERVAL_US = 10LL * 60 * 1000 * 1000;

  // Avoid re-announcing the same tip to the same peer repeatedly (respect TTL)
  {
    std::lock_guard<std::mutex> guard(announce_mutex_);
    auto it_hash = last_announced_to_peer_.find(peer->id());
    auto it_time = last_announce_time_us_.find(peer->id());
    const bool same_tip = (it_hash != last_announced_to_peer_.end() && it_hash->second == current_tip_hash);
    const bool have_time = (it_time != last_announce_time_us_.end());
    const bool within_ttl = have_time && (now_us - it_time->second < REANNOUNCE_INTERVAL_US);
    if (same_tip && within_ttl) {
      return; // Already announced recently
    }
  }

  // Add to peer's announcement queue (like Bitcoin's m_blocks_for_inv_relay)
  std::lock_guard<std::mutex> lock(peer->block_inv_mutex_);

  // Only add if not already in this peer's queue (per-peer deduplication)
  auto& queue = peer->blocks_for_inv_relay_;
  if (std::find(queue.begin(), queue.end(), current_tip_hash) == queue.end()) {
    queue.push_back(current_tip_hash);
    std::lock_guard<std::mutex> guard(announce_mutex_);
    last_announced_to_peer_[peer->id()] = current_tip_hash;
    last_announce_time_us_[peer->id()] = now_us;
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
        if (header_sync_manager_->HasSyncPeer()) {
          if (sync_id == static_cast<uint64_t>(peer->id())) {
            header_sync_manager_->RequestHeadersFromPeer(peer);
          } else {
            // Ignore INV-driven requests from non-sync peers during initial sync
          }
        } else {
          // Initial sync not yet started: only adopt during IBD (Core behavior)
          if (chainstate_manager_.IsInitialBlockDownload()) {
            LOG_NET_DEBUG("HandleInv: (IBD) adopting peer={} and requesting headers", peer->id());
            header_sync_manager_->SetSyncPeer(peer->id());
            // Mark sync started to avoid repeated adoption on subsequent INVs
            peer->set_sync_started(true);
            header_sync_manager_->RequestHeadersFromPeer(peer);
          } else {
            // POST-IBD: We still need to fetch headers when peers announce new blocks.
            // Do NOT adopt a sync peer; simply request headers from the announcing peer.
            LOG_NET_DEBUG("HandleInv: (post-IBD) requesting headers on INV from peer={}", peer->id());
            header_sync_manager_->RequestHeadersFromPeer(peer);
          }
        }
      }
    }
  }

  return true;
}

void BlockRelayManager::OnPeerDisconnected(int peer_id) {
  std::lock_guard<std::mutex> guard(announce_mutex_);
  last_announced_to_peer_.erase(peer_id);
  last_announce_time_us_.erase(peer_id);
}

} // namespace network
} // namespace coinbasechain
