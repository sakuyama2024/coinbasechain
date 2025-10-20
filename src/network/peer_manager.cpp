#include "network/peer_manager.hpp"
#include "chain/logging.hpp"
#include <algorithm>

namespace coinbasechain {
namespace network {

PeerManager::PeerManager(boost::asio::io_context &io_context,
                         AddressManager &addr_manager, const Config &config)
    : io_context_(io_context), addr_manager_(addr_manager), config_(config) {}

PeerManager::~PeerManager() { disconnect_all(); }

int PeerManager::allocate_peer_id() { return next_peer_id_++; }

int PeerManager::add_peer(PeerPtr peer, NetPermissionFlags permissions,
                          const std::string &address) {
  if (!peer) {
    return -1;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  // Check connection limits
  bool is_inbound = peer->is_inbound();
  size_t current_inbound = 0;
  size_t current_outbound = 0;

  for (const auto &[id, p] : peers_) {
    if (p->is_inbound()) {
      current_inbound++;
    } else {
      current_outbound++;
    }
  }

  // Check outbound limit (no eviction for outbound)
  if (!is_inbound && current_outbound >= config_.max_outbound_peers) {
    return -1; // Too many outbound connections
  }

  // Check inbound limit - try eviction if at capacity
  if (is_inbound && current_inbound >= config_.max_inbound_peers) {
    // Release lock temporarily to call evict_inbound_peer
    // (evict_inbound_peer will acquire its own lock)
    mutex_.unlock();
    bool evicted = evict_inbound_peer();
    mutex_.lock();

    if (!evicted) {
      return -1; // Couldn't evict anyone, reject connection
    }
    // Successfully evicted a peer, continue with adding new peer
  }

  // Allocate ID and add peer
  int peer_id = allocate_peer_id();
  peer->set_id(peer_id);  // Set the ID on the peer object
  peers_[peer_id] = std::move(peer);

  // Initialize misbehavior tracking
  std::string peer_address = address.empty() ? peers_[peer_id]->address() : address;
  peer_misbehavior_[peer_id] = PeerMisbehaviorData{
      .misbehavior_score = 0,
      .should_discourage = false,
      .num_unconnecting_headers_msgs = 0,
      .permissions = permissions,
      .address = peer_address};

  return peer_id;  // Return the assigned ID
}

void PeerManager::remove_peer(int peer_id) {
  PeerPtr peer;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    LOG_NET_TRACE("remove_peer({}): peers_.size() BEFORE = {}", peer_id, peers_.size());

    auto it = peers_.find(peer_id);
    if (it == peers_.end()) {
      // Peer already removed - this is OK, just return silently
      LOG_NET_TRACE("remove_peer({}): peer NOT FOUND in map", peer_id);
      return;
    }

    peer = it->second;
    peers_.erase(it);

    // Also remove misbehavior tracking data
    peer_misbehavior_.erase(peer_id);

    LOG_NET_TRACE("remove_peer({}): peers_.size() AFTER = {}", peer_id, peers_.size());
    LOG_NET_INFO("remove_peer: Erased peer {} from map (map size now: {})",
                 peer_id, peers_.size());
  }

  // Disconnect outside the lock
  if (peer) {
    LOG_NET_INFO("remove_peer: Calling disconnect() on peer {}", peer_id);
    peer->disconnect();
  }
}

PeerPtr PeerManager::get_peer(int peer_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = peers_.find(peer_id);
  return (it != peers_.end()) ? it->second : nullptr;
}

int PeerManager::find_peer_by_address(const std::string &address,
                                      uint16_t port) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Search all peers for matching address:port
  for (const auto &[id, peer] : peers_) {
    if (!peer)
      continue;

    // Match both address and port
    if (peer->address() == address && peer->port() == port) {
      return id;
    }
  }

  return -1; // Not found
}

std::vector<PeerPtr> PeerManager::get_all_peers() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<PeerPtr> result;
  result.reserve(peers_.size());

  for (const auto &[id, peer] : peers_) {
    result.push_back(peer);
  }

  return result;
}

std::vector<PeerPtr> PeerManager::get_outbound_peers() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<PeerPtr> result;

  for (const auto &[id, peer] : peers_) {
    if (!peer->is_inbound()) {
      result.push_back(peer);
    }
  }

  return result;
}

std::vector<PeerPtr> PeerManager::get_inbound_peers() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<PeerPtr> result;

  for (const auto &[id, peer] : peers_) {
    if (peer->is_inbound()) {
      result.push_back(peer);
    }
  }

  return result;
}

size_t PeerManager::peer_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  LOG_NET_TRACE("peer_count() called: returning {}", peers_.size());
  return peers_.size();
}

size_t PeerManager::outbound_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t count = 0;

  for (const auto &[id, peer] : peers_) {
    if (!peer->is_inbound()) {
      count++;
    }
  }

  return count;
}

size_t PeerManager::inbound_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t count = 0;

  for (const auto &[id, peer] : peers_) {
    if (peer->is_inbound()) {
      count++;
    }
  }

  return count;
}

bool PeerManager::needs_more_outbound() const {
  return outbound_count() < config_.target_outbound_peers;
}

bool PeerManager::can_accept_inbound() const {
  return inbound_count() < config_.max_inbound_peers;
}

bool PeerManager::evict_inbound_peer() {
  std::lock_guard<std::mutex> lock(mutex_);

  // Collect inbound peers that can be evicted
  // Protection rules (inspired by Bitcoin's SelectNodeToEvict):
  // 1. Never evict outbound peers
  // 2. Protect recently connected peers (last 10 seconds)
  // 3. Prefer evicting peers with worst ping times

  struct EvictionCandidate {
    int peer_id;
    std::chrono::steady_clock::time_point connected_time;
    int64_t ping_time_ms;
  };

  std::vector<EvictionCandidate> candidates;
  auto now = std::chrono::steady_clock::now();

  for (const auto &[id, peer] : peers_) {
    // Only consider inbound peers
    if (!peer->is_inbound()) {
      continue;
    }

    // Protect recently connected peers (within 10 seconds)
    auto connection_age = std::chrono::duration_cast<std::chrono::seconds>(
        now - peer->stats().connected_time);
    if (connection_age.count() < 10) {
      continue;
    }

    candidates.push_back(
        {id, peer->stats().connected_time, peer->stats().ping_time_ms});
  }

  // If no candidates, can't evict
  if (candidates.empty()) {
    return false;
  }

  // Simple eviction strategy for headers-only chain:
  // Evict the peer with the worst (highest) ping time, or oldest connection if
  // no ping data
  int worst_peer_id = -1;
  int64_t worst_ping = -1;
  auto oldest_connected = std::chrono::steady_clock::time_point::max();

  for (const auto &candidate : candidates) {
    if (candidate.ping_time_ms > worst_ping) {
      worst_ping = candidate.ping_time_ms;
      worst_peer_id = candidate.peer_id;
      oldest_connected = candidate.connected_time;
    } else if (candidate.ping_time_ms == worst_ping) {
      // Tie-breaker: prefer evicting older connection
      if (candidate.connected_time < oldest_connected) {
        worst_peer_id = candidate.peer_id;
        oldest_connected = candidate.connected_time;
      }
    }
  }

  if (worst_peer_id >= 0) {
    // Evict this peer
    auto it = peers_.find(worst_peer_id);
    if (it != peers_.end()) {
      PeerPtr peer = it->second;
      peers_.erase(it);
      peer_misbehavior_.erase(worst_peer_id);

      // Disconnect outside the lock (already have lock, will unlock at end)
      if (peer) {
        peer->disconnect();
      }

      return true;
    }
  }

  return false;
}

void PeerManager::disconnect_all() {
  std::map<int, PeerPtr> peers_to_disconnect;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    peers_to_disconnect = peers_;
    peers_.clear();
    peer_misbehavior_.clear();
  }

  // Disconnect all peers outside the lock
  for (auto &[id, peer] : peers_to_disconnect) {
    if (peer) {
      peer->disconnect();
    }
  }
}

void PeerManager::process_periodic() {
  LOG_NET_TRACE("process_periodic() peers={} misbehavior_entries={}",
                peers_.size(), peer_misbehavior_.size());

  std::vector<int> to_remove;

  {
    std::lock_guard<std::mutex> lock(mutex_);

    // Find disconnected peers and peers marked for disconnection
    for (const auto &[id, peer] : peers_) {
      if (!peer->is_connected()) {
        LOG_NET_TRACE("process_periodic: peer={} not connected, marking for removal", id);
        to_remove.push_back(id);
      }
    }

    // Also find peers marked for disconnection due to misbehavior
    for (const auto &[peer_id, data] : peer_misbehavior_) {
      LOG_NET_TRACE("process_periodic: checking peer={} score={} should_discourage={}",
                    peer_id, data.misbehavior_score, data.should_discourage);

      if (data.should_discourage) {
        // Never disconnect peers with NoBan permission (matches Bitcoin)
        if (HasPermission(data.permissions, NetPermissionFlags::NoBan)) {
          LOG_NET_TRACE("process_periodic: skipping NoBan peer={} (score={} but protected)",
                        peer_id, data.misbehavior_score);
          continue;
        }

        // Add to removal list if not already there
        if (std::find(to_remove.begin(), to_remove.end(), peer_id) == to_remove.end()) {
          to_remove.push_back(peer_id);
          LOG_NET_TRACE("process_periodic: marking peer={} for removal (score={})",
                        peer_id, data.misbehavior_score);
          LOG_NET_INFO("process_periodic: Disconnecting misbehaving peer {} (score: {})",
                       peer_id, data.misbehavior_score);
        }
      }
    }
  }

  LOG_NET_TRACE("process_periodic: removing {} peers", to_remove.size());

  // Remove disconnected peers
  for (int peer_id : to_remove) {
    remove_peer(peer_id);
  }

  // Cleanup stale addresses in AddressManager
  addr_manager_.cleanup_stale();
}

// === Misbehavior Tracking Public API ===

void PeerManager::ReportInvalidPoW(int peer_id) {
  Misbehaving(peer_id, MisbehaviorPenalty::INVALID_POW,
              "header with invalid proof of work");
}

void PeerManager::ReportOversizedMessage(int peer_id) {
  Misbehaving(peer_id, MisbehaviorPenalty::OVERSIZED_MESSAGE,
              "oversized message");
}

void PeerManager::ReportNonContinuousHeaders(int peer_id) {
  Misbehaving(peer_id, MisbehaviorPenalty::NON_CONTINUOUS_HEADERS,
              "non-continuous headers sequence");
}

void PeerManager::ReportLowWorkHeaders(int peer_id) {
  Misbehaving(peer_id, MisbehaviorPenalty::LOW_WORK_HEADERS,
              "low-work headers");
}

void PeerManager::ReportInvalidHeader(int peer_id, const std::string &reason) {
  Misbehaving(peer_id, MisbehaviorPenalty::INVALID_HEADER,
              "invalid header: " + reason);
}

void PeerManager::ReportTooManyOrphans(int peer_id) {
  Misbehaving(peer_id, MisbehaviorPenalty::TOO_MANY_ORPHANS,
              "exceeded orphan header limit");
}

// === Misbehavior Tracking Internal Implementation ===

bool PeerManager::Misbehaving(int peer_id, int penalty,
                               const std::string &reason) {
  std::lock_guard<std::mutex> lock(mutex_);

  LOG_NET_TRACE("Misbehaving() peer={} penalty={} reason={}", peer_id, penalty, reason);

  auto it = peer_misbehavior_.find(peer_id);
  if (it == peer_misbehavior_.end()) {
    // Peer not found - may have been disconnected already
    LOG_NET_TRACE("Misbehaving() peer={} not found in map (already disconnected?)", peer_id);
    return false;
  }

  PeerMisbehaviorData &data = it->second;

  // Add penalty to score (always track, even for NoBan peers - matches Bitcoin)
  int old_score = data.misbehavior_score;
  data.misbehavior_score += penalty;

  LOG_NET_TRACE("Misbehaving() peer={} score: {} -> {} (threshold={})",
                peer_id, old_score, data.misbehavior_score, DISCOURAGEMENT_THRESHOLD);

  LOG_NET_INFO("Peer {} ({}) misbehavior +{}: {} (total score: {})", peer_id,
               data.address, penalty, reason, data.misbehavior_score);

  // Check if threshold exceeded
  if (data.misbehavior_score >= DISCOURAGEMENT_THRESHOLD && old_score < DISCOURAGEMENT_THRESHOLD) {
    LOG_NET_TRACE("Misbehaving() peer={} THRESHOLD EXCEEDED", peer_id);

    // Check if peer has NoBan permission (matches Bitcoin: track score but don't disconnect)
    if (HasPermission(data.permissions, NetPermissionFlags::NoBan)) {
      LOG_NET_WARN("Warning: not punishing noban peer {} (score {} >= threshold {})",
                   peer_id, data.misbehavior_score, DISCOURAGEMENT_THRESHOLD);
      // DO NOT set should_discourage for NoBan peers
      return false;
    }

    // Normal peer: mark for disconnection
    data.should_discourage = true;
    LOG_NET_WARN("Peer {} ({}) marked for disconnect (score {} >= threshold {})",
                 peer_id, data.address, data.misbehavior_score,
                 DISCOURAGEMENT_THRESHOLD);
    return true;
  }

  LOG_NET_TRACE("Misbehaving() peer={} threshold not exceeded, continuing", peer_id);
  return false;
}

bool PeerManager::ShouldDisconnect(int peer_id) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = peer_misbehavior_.find(peer_id);
  if (it == peer_misbehavior_.end()) {
    return false;
  }

  // Never disconnect peers with NoBan permission (matches Bitcoin)
  if (HasPermission(it->second.permissions, NetPermissionFlags::NoBan)) {
    return false;
  }

  return it->second.should_discourage;
}

int PeerManager::GetMisbehaviorScore(int peer_id) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = peer_misbehavior_.find(peer_id);
  if (it == peer_misbehavior_.end()) {
    return 0;
  }

  return it->second.misbehavior_score;
}

void PeerManager::IncrementUnconnectingHeaders(int peer_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = peer_misbehavior_.find(peer_id);
  if (it == peer_misbehavior_.end()) {
    LOG_NET_TRACE("IncrementUnconnectingHeaders: peer {} not found in misbehavior map", peer_id);
    return;
  }

  PeerMisbehaviorData &data = it->second;
  data.num_unconnecting_headers_msgs++;

  LOG_NET_TRACE("IncrementUnconnectingHeaders: peer {} now has {} unconnecting msgs (threshold={})",
                peer_id, data.num_unconnecting_headers_msgs, MAX_UNCONNECTING_HEADERS);

  if (data.num_unconnecting_headers_msgs >= MAX_UNCONNECTING_HEADERS) {
    LOG_NET_INFO("Peer {} ({}) sent too many unconnecting headers ({} >= {})",
                 peer_id, data.address, data.num_unconnecting_headers_msgs,
                 MAX_UNCONNECTING_HEADERS);

    // Apply penalty - delegate to Misbehaving (must unlock first to avoid deadlock)
    LOG_NET_TRACE("IncrementUnconnectingHeaders: Threshold exceeded, calling Misbehaving");
    mutex_.unlock();
    Misbehaving(peer_id, MisbehaviorPenalty::TOO_MANY_UNCONNECTING,
                "too many unconnecting headers");
    mutex_.lock();
  }
}

void PeerManager::ResetUnconnectingHeaders(int peer_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = peer_misbehavior_.find(peer_id);
  if (it == peer_misbehavior_.end()) {
    return;
  }

  it->second.num_unconnecting_headers_msgs = 0;
}

} // namespace network
} // namespace coinbasechain
