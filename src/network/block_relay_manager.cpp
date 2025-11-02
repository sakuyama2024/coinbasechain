#include "network/block_relay_manager.hpp"
#include "network/peer_manager.hpp"
#include "network/header_sync_manager.hpp"
#include "chain/chainstate_manager.hpp"
#include "util/logging.hpp"
#include "util/time.hpp"
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
  // IMPORTANT: Re-announce periodically to handle partition healing, but avoid storms during active header sync

  const chain::CBlockIndex *tip = chainstate_manager_.GetTip();
  if (!tip || tip->nHeight == 0) {
    return; // No tip to announce
  }

  uint256 current_tip_hash = tip->GetBlockHash();

  LOG_NET_DEBUG("Adding tip to all peers' announcement queues (height={}, hash={})", tip->nHeight,
                current_tip_hash.GetHex().substr(0, 16));



  // Time now (mocked unix seconds)
  const int64_t now_s = util::GetTime();
  // Re-announce interval (10 minutes)
  static constexpr int64_t REANNOUNCE_INTERVAL_SEC = 10LL * 60; // 10 min
  // TTL semantics: we only update last_announce_time_s_ when we enqueue.
  // If the tip remains in the peer's queue, we do not refresh TTL here.
  // With 1s flushing this is harmless; if flush cadence lengthens, we may
  // re-enqueue after TTL even if a duplicate sat in-queue. This is acceptable
  // and keeps logic simple.

  // Add to all ready peers' announcement queues with per-peer deduplication + TTL
  auto all_peers = peer_manager_.get_all_peers();
  for (const auto &peer : all_peers) {
    if (peer && peer->is_connected() && peer->state() == PeerState::READY) {
      // Check per-peer TTL; suppress re-announce of same tip within TTL regardless of queue state
      bool same_tip = false;
      bool within_ttl = false;
      {
        std::lock_guard<std::mutex> guard(announce_mutex_);
        auto it_hash = last_announced_to_peer_.find(peer->id());
        auto it_time = last_announce_time_s_.find(peer->id());
        same_tip = (it_hash != last_announced_to_peer_.end() && it_hash->second == current_tip_hash);
        const bool have_time = (it_time != last_announce_time_s_.end());
        within_ttl = have_time && (now_s - it_time->second < REANNOUNCE_INTERVAL_SEC);
      }
      if (same_tip && within_ttl) {
        // TTL suppression: do not refresh last_announce_time_s_ unless we enqueue
        // (suppression window stays fixed until a new enqueue occurs)
        continue;
      }

      // Thread-safe access to block announcement queue
      peer->with_block_inv_queue([&](auto& queue) {
        // Only add if not already in this peer's queue (per-peer deduplication)
        // TTL refresh policy: we refresh last_announce_time_s_ ONLY when we enqueue;
        // if already present, we leave timestamps unchanged to avoid extending TTL spuriously.
        if (std::find(queue.begin(), queue.end(), current_tip_hash) == queue.end()) {
          queue.push_back(current_tip_hash);
          // Record last announcement (hash + time)
          std::lock_guard<std::mutex> guard(announce_mutex_);
          last_announced_to_peer_[peer->id()] = current_tip_hash;
          last_announce_time_s_[peer->id()] = now_s;
        }
      });
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

  // Time now (mocked unix seconds)
  const int64_t now_s = util::GetTime();
  static constexpr int64_t REANNOUNCE_INTERVAL_SEC = 10LL * 60;

  // Add to peer's announcement queue 
  // Thread-safe access to block announcement queue
  peer->with_block_inv_queue([&](auto& queue) {
    // Only add if not already in this peer's queue (per-peer deduplication)
    const bool already_queued = (std::find(queue.begin(), queue.end(), current_tip_hash) != queue.end());

    // For per-peer READY event, ignore TTL and ensure the current tip is queued once.
    // Note: If already queued, we do NOT refresh last_announce_time_s_ here; TTL refresh only on enqueue.
    if (!already_queued) {
      queue.push_back(current_tip_hash);
      std::lock_guard<std::mutex> guard(announce_mutex_);
      last_announced_to_peer_[peer->id()] = current_tip_hash;
      last_announce_time_s_[peer->id()] = now_s;
    }
  });
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
    peer->with_block_inv_queue([&](auto& queue) {
      if (!queue.empty()) {
        blocks_to_announce = std::move(queue);
        queue.clear();
      }
    });

    // Skip if no blocks to announce
    if (blocks_to_announce.empty()) {
      continue;
    }

    // Create and send INV message(s) with pending blocks
    // Chunked to protocol::MAX_INV_SIZE to respect wire limits and avoid overlarge messages
    // (parity with Bitcoin Core policy for INV caps).
    const size_t total = blocks_to_announce.size();
    const size_t chunk = protocol::MAX_INV_SIZE;
    size_t sent_items = 0;
    size_t chunk_idx = 0;
    while (sent_items < total) {
      size_t end = std::min(total, sent_items + chunk);
      auto inv_msg = std::make_unique<message::InvMessage>();
      inv_msg->inventory.reserve(end - sent_items);
      for (size_t j = sent_items; j < end; ++j) {
        protocol::InventoryVector inv;
        inv.type = protocol::InventoryType::MSG_BLOCK;
        std::memcpy(inv.hash.data(), blocks_to_announce[j].data(), 32);
        inv_msg->inventory.push_back(inv);
      }
      ++chunk_idx;
      LOG_NET_DEBUG("Flushing {} block announcement(s) to peer {} (chunk {}/{})",
                    (end - sent_items), peer->id(), chunk_idx, (total + chunk - 1)/chunk);
      peer->send_message(std::move(inv_msg));
      sent_items = end;
    }
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

  // Time now (mocked unix seconds) for TTL bookkeeping
  const int64_t now_s = util::GetTime();

  // Send to all connected peers
  // Policy: Age gating (MAX_BLOCK_RELAY_AGE) and IBD checks are enforced by the caller
  // (Application's BlockConnected subscription) before invoking relay_block(). This function
  // focuses on efficient delivery and deduplication vs queued announcements.
  auto all_peers = peer_manager_.get_all_peers();
  for (const auto &peer : all_peers) {
    if (peer && peer->is_connected()) {
      if (peer->state() == PeerState::READY) {
        // Drop any pending duplicate of this block from the peer's INV queue before immediate relay
        peer->with_block_inv_queue([&](auto& queue) {
          queue.erase(std::remove(queue.begin(), queue.end(), block_hash), queue.end());
        });
        // Clone the message for each peer
        auto msg_copy = std::make_unique<message::InvMessage>();
        msg_copy->inventory = inv_msg->inventory;
        peer->send_message(std::move(msg_copy));
        // Record last announcement (hash + time) so periodic reannounce TTL suppresses duplicates
        {
          std::lock_guard<std::mutex> guard(announce_mutex_);
          last_announced_to_peer_[peer->id()] = block_hash;
          last_announce_time_s_[peer->id()] = now_s;
        }
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
      if (header_sync_manager_) {
        const bool in_ibd = chainstate_manager_.IsInitialBlockDownload();
        if (in_ibd) {
          // During IBD, only request from our designated sync peer
          if (header_sync_manager_->HasSyncPeer()) {
            uint64_t sync_id = header_sync_manager_->GetSyncPeerId();
            if (sync_id == static_cast<uint64_t>(peer->id())) {
              header_sync_manager_->RequestHeadersFromPeer(peer);
            } else {
              // Ignore INV-driven requests from non-sync peers during IBD
            }
          } else {
            // No sync peer yet: adopt announcer as sync peer ONLY if outbound (Core parity)
            if (!peer->is_inbound()) {
              LOG_NET_DEBUG("HandleInv: (IBD) adopting OUTBOUND peer={} and requesting headers", peer->id());
              header_sync_manager_->SetSyncPeer(peer->id());
              peer->set_sync_started(true);
              header_sync_manager_->RequestHeadersFromPeer(peer);
            } else {
              LOG_NET_TRACE("HandleInv: (IBD) ignoring inbound announcer for sync adoption (Core parity)");
            }
          }
        } else {
          // Post-IBD: Always request headers from the announcing peer, regardless of sync peer
          LOG_NET_DEBUG("HandleInv: (post-IBD) requesting headers on INV from peer={}", peer->id());
          header_sync_manager_->RequestHeadersFromPeer(peer);
        }
      }
    }
  }

  return true;
}

void BlockRelayManager::OnPeerDisconnected(int peer_id) {
  std::lock_guard<std::mutex> guard(announce_mutex_);
  last_announced_to_peer_.erase(peer_id);
  last_announce_time_s_.erase(peer_id);
}

} // namespace network
} // namespace coinbasechain
