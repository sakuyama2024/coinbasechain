// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "sync/peer_manager.hpp"
#include "util/logging.hpp"

namespace coinbasechain {
namespace sync {

void PeerManager::AddPeer(int peer_id, const std::string &address,
                          NetPermissionFlags permissions) {
  std::lock_guard<std::mutex> lock(peers_mutex_);

  if (peers_.find(peer_id) != peers_.end()) {
    LOG_WARN("Peer {} already exists, updating address", peer_id);
  }

  peers_[peer_id] = std::make_shared<Peer>(peer_id, address, permissions);

  std::string perm_str;
  if (HasPermission(permissions, NetPermissionFlags::NoBan)) {
    perm_str += " [NoBan]";
  }
  if (HasPermission(permissions, NetPermissionFlags::Manual)) {
    perm_str += " [Manual]";
  }

  LOG_INFO("Added peer {}: {}{}", peer_id, address, perm_str);
}

void PeerManager::RemovePeer(int peer_id) {
  std::lock_guard<std::mutex> lock(peers_mutex_);

  auto it = peers_.find(peer_id);
  if (it != peers_.end()) {
    LOG_INFO("Removed peer {}: {}", peer_id, it->second->address);
    peers_.erase(it);
  }
}

bool PeerManager::Misbehaving(int peer_id, int howmuch,
                              const std::string &message) {
  if (howmuch <= 0) {
    return false;
  }

  std::lock_guard<std::mutex> lock(peers_mutex_);

  auto it = peers_.find(peer_id);
  if (it == peers_.end()) {
    // Peer not found - might have been disconnected already
    LOG_DEBUG("Misbehaving called for unknown peer {}", peer_id);
    return false;
  }

  std::shared_ptr<Peer> peer = it->second;
  int score_before = peer->misbehavior_score;
  peer->misbehavior_score += howmuch;
  int score_now = peer->misbehavior_score;

  // Check if threshold is crossed
  bool threshold_crossed = (score_now >= DISCOURAGEMENT_THRESHOLD &&
                            score_before < DISCOURAGEMENT_THRESHOLD);

  std::string warning;
  if (threshold_crossed) {
    // Check for NoBan permission - peers with NoBan cannot be disconnected
    if (HasPermission(peer->permissions, NetPermissionFlags::NoBan)) {
      warning = " THRESHOLD EXCEEDED BUT NOBAN PROTECTED";
      LOG_WARN("Misbehaving: NoBan peer={} ({}) score: {} -> {} - protected "
               "from disconnect{}",
               peer_id, peer->address, score_before, score_now,
               message.empty() ? "" : (": " + message));
    } else {
      // Mark for disconnection
      warning = " DISCONNECT THRESHOLD EXCEEDED";
      peer->should_discourage = true;
    }
  }

  std::string message_prefixed = message.empty() ? "" : (": " + message);

  if (score_now >= DISCOURAGEMENT_THRESHOLD) {
    LOG_WARN("Misbehaving: peer={} ({}) score: {} -> {}{}{}", peer_id,
             peer->address, score_before, score_now, warning, message_prefixed);
  } else {
    LOG_INFO("Misbehaving: peer={} ({}) score: {} -> {}{}", peer_id,
             peer->address, score_before, score_now, message_prefixed);
  }

  return peer->should_discourage;
}

bool PeerManager::ShouldDisconnect(int peer_id) const {
  std::lock_guard<std::mutex> lock(peers_mutex_);

  auto it = peers_.find(peer_id);
  if (it == peers_.end()) {
    return false; // Peer doesn't exist
  }

  const auto &peer = it->second;

  // Check if peer should be disconnected
  if (!peer->should_discourage) {
    return false;
  }

  // Check for NoBan permission - only NoBan flag prevents disconnection
  // Manual peers CAN still be banned if they misbehave
  if (HasPermission(peer->permissions, NetPermissionFlags::NoBan)) {
    LOG_WARN("Not disconnecting NoBan peer {}", peer_id);
    return false;
  }

  return true;
}

int PeerManager::GetMisbehaviorScore(int peer_id) const {
  std::lock_guard<std::mutex> lock(peers_mutex_);

  auto it = peers_.find(peer_id);
  if (it == peers_.end()) {
    return 0; // Unknown peer
  }

  return it->second->misbehavior_score;
}

size_t PeerManager::GetPeerCount() const {
  std::lock_guard<std::mutex> lock(peers_mutex_);
  return peers_.size();
}

bool PeerManager::IncrementUnconnectingHeaders(int peer_id) {
  std::lock_guard<std::mutex> lock(peers_mutex_);

  auto it = peers_.find(peer_id);
  if (it == peers_.end()) {
    LOG_DEBUG("IncrementUnconnectingHeaders called for unknown peer {}",
              peer_id);
    return false;
  }

  std::shared_ptr<Peer> peer = it->second;
  peer->num_unconnecting_headers_msgs++;

  LOG_DEBUG("Peer {} ({}) unconnecting headers count: {}", peer_id,
            peer->address, peer->num_unconnecting_headers_msgs);

  // Check if threshold exceeded
  if (peer->num_unconnecting_headers_msgs >= MAX_UNCONNECTING_HEADERS) {
    LOG_WARN("Peer {} ({}) exceeded unconnecting headers threshold ({})",
             peer_id, peer->address, peer->num_unconnecting_headers_msgs);
    return true;
  }

  return false;
}

void PeerManager::ResetUnconnectingHeaders(int peer_id) {
  std::lock_guard<std::mutex> lock(peers_mutex_);

  auto it = peers_.find(peer_id);
  if (it == peers_.end()) {
    return; // Peer doesn't exist
  }

  it->second->num_unconnecting_headers_msgs = 0;
}

} // namespace sync
} // namespace coinbasechain
