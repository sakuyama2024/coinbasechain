#include "network/peer_manager.hpp"
#include "network/notifications.hpp"
#include "util/logging.hpp"
#include "network/protocol.hpp"
#include <algorithm>
#include <boost/asio/ip/address.hpp>
#include <limits>

namespace coinbasechain {
namespace network {

namespace {
// Normalize an IP string; map IPv4-mapped IPv6 to dotted-quad IPv4.
static std::string normalize_ip_string(const std::string& s) {
  try {
    boost::system::error_code ec;
    auto ip = boost::asio::ip::make_address(s, ec);
    if (ec) return s;
    if (ip.is_v6() && ip.to_v6().is_v4_mapped()) {
      auto v4 = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, ip.to_v6());
      return v4.to_string();
    }
    return ip.to_string();
  } catch (...) {
    return s;
  }
}
} // namespace

PeerManager::PeerManager(boost::asio::io_context &io_context,
                         AddressManager &addr_manager, const Config &config)
    : io_context_(io_context), addr_manager_(addr_manager), config_(config) {}

PeerManager::~PeerManager() {
  Shutdown();
  disconnect_all();
}

void PeerManager::Shutdown() {
  shutting_down_.store(true, std::memory_order_release);
}

int PeerManager::allocate_peer_id() { return next_peer_id_.fetch_add(1, std::memory_order_relaxed); }

int PeerManager::add_peer(PeerPtr peer, NetPermissionFlags permissions,
                          const std::string &address) {
  if (!peer) {
    return -1;
  }

  // Reject additions during bulk shutdown (atomic check)
  if (stopping_all_.load(std::memory_order_acquire)) {
    LOG_NET_TRACE("add_peer: rejected while disconnect_all in progress");
    return -1;
  }

  // Check connection limits
  bool is_inbound = peer->is_inbound();
  bool is_feeler_new = peer->is_feeler();
  size_t current_inbound = 0;
  size_t current_outbound_nonfeeler = 0;

  // Count current connections using peer_states_
  peer_states_.ForEach([&](int id, const PerPeerState& state) {
    if (state.peer->is_inbound()) {
      current_inbound++;
    } else {
      // Outbound: only count full-relay (exclude feelers from slot consumption)
      if (!state.peer->is_feeler()) {
        current_outbound_nonfeeler++;
      }
    }
  });

  // Check outbound limit (no eviction for outbound)
  // Do not count feeler connections against outbound capacity, and do not gate them here
  if (!is_inbound && !is_feeler_new && current_outbound_nonfeeler >= config_.max_outbound_peers) {
    return -1; // Too many outbound full-relay connections
  }

  // Check inbound limit - try eviction if at capacity
  if (is_inbound && current_inbound >= config_.max_inbound_peers) {
    bool evicted = evict_inbound_peer();

    if (!evicted) {
      LOG_NET_TRACE("add_peer: inbound at capacity and eviction failed (likely all peers protected by recent-connection window)");
      return -1; // Couldn't evict anyone, reject connection
    }
    // Recompute inbound counts after eviction to avoid TOCTOU
    if (is_inbound) {
      size_t inbound_now = 0;
      peer_states_.ForEach([&](int id, const PerPeerState& state) {
        if (state.peer && state.peer->is_inbound()) inbound_now++;
      });
      if (inbound_now >= config_.max_inbound_peers) {
        LOG_NET_TRACE("add_peer: inbound still at capacity after eviction, rejecting");
        return -1;
      }
    }
    // Successfully evicted and capacity confirmed; continue
  }

  // Enforce per-IP inbound limit before adding
  if (is_inbound) {
    const std::string new_addr = normalize_ip_string(peer->address());
    int same_ip_inbound = 0;
    peer_states_.ForEach([&](int id, const PerPeerState& state) {
      if (state.peer->is_inbound() && normalize_ip_string(state.peer->address()) == new_addr) {
        same_ip_inbound++;
      }
    });
    if (same_ip_inbound >= MAX_INBOUND_PER_IP) {
      return -1; // Reject new inbound from same IP
    }
  }

  // Allocate ID and add peer
  int peer_id = allocate_peer_id();
  peer->set_id(peer_id);  // Set the ID on the peer object

  // Create and insert PerPeerState
  std::string peer_address = address.empty() ? peer->address() : address;
  auto creation_time = std::chrono::steady_clock::now();

  PerPeerState state(peer, creation_time);
  state.misbehavior.permissions = permissions;
  state.misbehavior.address = peer_address;
  peer_states_.Insert(peer_id, std::move(state));

  LOG_NET_DEBUG("Added connection peer={}", peer_id);

  return peer_id;  // Return the assigned ID
}

bool PeerManager::add_peer_with_id(int peer_id, PeerPtr peer, NetPermissionFlags permissions,
                                   const std::string &address) {
  if (!peer) {
    return false;
  }

  // Verify peer ID matches
  if (peer->id() != peer_id) {
    LOG_NET_ERROR("add_peer_with_id: peer ID mismatch (expected={}, got={})", peer_id, peer->id());
    return false;
  }

  // Reject additions during bulk shutdown (atomic check)
  if (stopping_all_.load(std::memory_order_acquire)) {
    LOG_NET_TRACE("add_peer_with_id: rejected while disconnect_all in progress");
    return false;
  }

  // Check if peer_id already exists (shouldn't happen, but defensive)
  if (peer_states_.Get(peer_id).has_value()) {
    LOG_NET_ERROR("add_peer_with_id: peer_id {} already exists", peer_id);
    return false;
  }

  // Check connection limits
  bool is_inbound = peer->is_inbound();
  bool is_feeler_new = peer->is_feeler();
  size_t current_inbound = 0;
  size_t current_outbound_nonfeeler = 0;

  peer_states_.ForEach([&](int id, const PerPeerState& state) {
    if (state.peer->is_inbound()) {
      current_inbound++;
    } else {
      if (!state.peer->is_feeler()) {
        current_outbound_nonfeeler++;
      }
    }
  });

  // Check outbound limit
  if (!is_inbound && !is_feeler_new && current_outbound_nonfeeler >= config_.max_outbound_peers) {
    return false;
  }

  // Check inbound limit - try eviction if at capacity
  if (is_inbound && current_inbound >= config_.max_inbound_peers) {
    bool evicted = evict_inbound_peer();

    if (!evicted) {
      LOG_NET_TRACE("add_peer_with_id: inbound at capacity and eviction failed");
      return false;
    }
    // Recompute inbound counts after eviction
    if (is_inbound) {
      size_t inbound_now = 0;
      peer_states_.ForEach([&](int id, const PerPeerState& state) {
        if (state.peer && state.peer->is_inbound()) inbound_now++;
      });
      if (inbound_now >= config_.max_inbound_peers) {
        LOG_NET_TRACE("add_peer_with_id: inbound still at capacity after eviction");
        return false;
      }
    }
  }

  // Enforce per-IP inbound limit
  if (is_inbound) {
    const std::string new_addr = normalize_ip_string(peer->address());
    int same_ip_inbound = 0;
    peer_states_.ForEach([&](int id, const PerPeerState& state) {
      if (state.peer->is_inbound() && normalize_ip_string(state.peer->address()) == new_addr) {
        same_ip_inbound++;
      }
    });
    if (same_ip_inbound >= MAX_INBOUND_PER_IP) {
      return false;
    }
  }

  // Create and insert PerPeerState
  std::string peer_address = address.empty() ? peer->address() : address;
  auto creation_time = std::chrono::steady_clock::now();

  PerPeerState state(peer, creation_time);
  state.misbehavior.permissions = permissions;
  state.misbehavior.address = peer_address;
  peer_states_.Insert(peer_id, std::move(state));

  LOG_NET_DEBUG("Added connection with pre-allocated peer_id={}", peer_id);

  return true;
}

void PeerManager::remove_peer(int peer_id) {
  // Get peer state and erase from map (thread-safe)
  auto state_opt = peer_states_.Get(peer_id);
  if (!state_opt) {
    // Peer already removed - this is OK, just return silently
    LOG_NET_TRACE("remove_peer({}): peer NOT FOUND in map", peer_id);
    return;
  }

  const PerPeerState& state = *state_opt;
  PeerPtr peer = state.peer;
  std::string peer_address;
  std::string disconnect_reason = "disconnected";
  std::string addr_str_after;
  uint16_t addr_port_after = 0;

  // Capture peer address for notification
  if (peer) {
    peer_address = peer->address();
  }

  // Before erasing, capture misbehavior score for addrman update logic
  int misbehavior_score = state.misbehavior.misbehavior_score;
  if (state.misbehavior.should_discourage) {
    disconnect_reason = "misbehavior (score: " + std::to_string(misbehavior_score) + ")";
  }

  // Decide whether to mark as good in addrman
  if (peer && peer->successfully_connected() && misbehavior_score == 0 &&
      !peer->is_inbound() && !peer->is_feeler()) {
    addr_str_after = peer->target_address();
    addr_port_after = peer->target_port();
  }

  bool skip_notifications = shutting_down_.load(std::memory_order_acquire);

  // Erase from peer_states_ (thread-safe)
  peer_states_.Erase(peer_id);
  LOG_NET_TRACE("remove_peer: erased peer {} from map", peer_id);

  // Update addrman for good outbound peers
  if (addr_port_after != 0 && !addr_str_after.empty()) {
    try {
      auto net_addr = protocol::NetworkAddress::from_string(addr_str_after, addr_port_after);
      addr_manager_.good(net_addr);
      LOG_NET_TRACE("Updated addrman for disconnected peer {}:{}", addr_str_after, addr_port_after);
    } catch (const std::exception& e) {
      LOG_NET_WARN("Failed to update addrman for disconnected peer {}:{}: {}",
                   addr_str_after, addr_port_after, e.what());
    }
  }

  // Publish disconnect notification (replaces callback)
  if (!skip_notifications) {
    NetworkEvents().NotifyPeerDisconnected(peer_id, peer_address, disconnect_reason);
  }

  // Disconnect the peer
  if (peer) {
    LOG_NET_TRACE("remove_peer: calling disconnect() on peer {}", peer_id);
    peer->disconnect();
  }
}

PeerPtr PeerManager::get_peer(int peer_id) {
  auto state = peer_states_.Get(peer_id);
  return state ? state->peer : nullptr;
}

int PeerManager::find_peer_by_address(const std::string &address,
                                      uint16_t port) {
  const std::string needle_addr = normalize_ip_string(address);
  int result = -1;

  peer_states_.ForEach([&](int id, const PerPeerState& state) {
    if (result != -1) return; // Already found
    if (!state.peer) return;
    const std::string peer_addr = normalize_ip_string(state.peer->address());
    if (peer_addr != needle_addr) return;
    if (port != 0) {
      if (state.peer->port() == port) result = id;
      return;
    }
    // No port specified: return the first matching address
    result = id;
  });

  return result;
}

std::vector<PeerPtr> PeerManager::get_all_peers() {
  std::vector<PeerPtr> result;

  peer_states_.ForEach([&](int id, const PerPeerState& state) {
    result.push_back(state.peer);
  });

  // Sort by peer ID to ensure deterministic iteration order
  // (unordered_map iteration is non-deterministic)
  std::sort(result.begin(), result.end(), [](const PeerPtr& a, const PeerPtr& b) {
    return a->id() < b->id();
  });

  return result;
}

std::vector<PeerPtr> PeerManager::get_outbound_peers() {
  std::vector<PeerPtr> result;

  peer_states_.ForEach([&](int id, const PerPeerState& state) {
    if (!state.peer->is_inbound()) {
      result.push_back(state.peer);
    }
  });

  // Sort by peer ID to ensure deterministic iteration order
  // (unordered_map iteration is non-deterministic)
  std::sort(result.begin(), result.end(), [](const PeerPtr& a, const PeerPtr& b) {
    return a->id() < b->id();
  });

  return result;
}

std::vector<PeerPtr> PeerManager::get_inbound_peers() {
  std::vector<PeerPtr> result;

  peer_states_.ForEach([&](int id, const PerPeerState& state) {
    if (state.peer->is_inbound()) {
      result.push_back(state.peer);
    }
  });

  // Sort by peer ID to ensure deterministic iteration order
  // (unordered_map iteration is non-deterministic)
  std::sort(result.begin(), result.end(), [](const PeerPtr& a, const PeerPtr& b) {
    return a->id() < b->id();
  });

  return result;
}

size_t PeerManager::peer_count() const {
  size_t count = 0;
  peer_states_.ForEach([&](int id, const PerPeerState& state) {
    count++;
  });
  LOG_NET_TRACE("peer_count() called: returning {}", count);
  return count;
}

size_t PeerManager::outbound_count() const {
  size_t count = 0;

  // Count outbound peers (excluding feelers and manual which don't consume outbound slots)
  // Bitcoin Core pattern: Feelers and manual connections don't count against MAX_OUTBOUND_FULL_RELAY_CONNECTIONS
  peer_states_.ForEach([&](int id, const PerPeerState& state) {
    if (!state.peer->is_inbound() && !state.peer->is_feeler() && !state.peer->is_manual()) {
      count++;
    }
  });

  return count;
}

size_t PeerManager::inbound_count() const {
  size_t count = 0;

  peer_states_.ForEach([&](int id, const PerPeerState& state) {
    if (state.peer->is_inbound()) {
      count++;
    }
  });

  return count;
}

bool PeerManager::needs_more_outbound() const {
  return outbound_count() < config_.target_outbound_peers;
}

bool PeerManager::can_accept_inbound() const {
  return inbound_count() < config_.max_inbound_peers;
}

bool PeerManager::can_accept_inbound_from(const std::string& address) const {
  // Check global inbound limit
  size_t inbound_now = 0;
  peer_states_.ForEach([&](int id, const PerPeerState& state) {
    if (state.peer && state.peer->is_inbound()) inbound_now++;
  });
  if (inbound_now >= config_.max_inbound_peers) return false;

  const std::string needle = normalize_ip_string(address);
  int same_ip_inbound = 0;
  peer_states_.ForEach([&](int id, const PerPeerState& state) {
    if (!state.peer || !state.peer->is_inbound()) return;
    if (normalize_ip_string(state.peer->address()) == needle) {
      same_ip_inbound++;
    }
  });

  return same_ip_inbound < MAX_INBOUND_PER_IP;
}
bool PeerManager::evict_inbound_peer() {
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

  peer_states_.ForEach([&](int id, const PerPeerState& state) {
    // Only consider inbound peers
    if (!state.peer->is_inbound()) {
      return;
    }

    // Protect recently connected peers (within 10 seconds)
    auto connection_age = std::chrono::duration_cast<std::chrono::seconds>(
        now - state.peer->stats().connected_time);
    if (connection_age.count() < 10) {
      return;
    }

    candidates.push_back(
        {id, state.peer->stats().connected_time, state.peer->stats().ping_time_ms});
  });

  // If no candidates, can't evict
  if (candidates.empty()) {
    return false;
  }

  // Simple eviction strategy for headers-only chain:
  // Evict the peer with the worst (highest) ping time, or oldest connection if
  // no ping data
  int worst_peer_id = -1;
  auto oldest_connected = std::chrono::steady_clock::time_point::max();
  // Map unknown ping (-1) to a large sentinel so we prefer evicting unknowns
  auto map_ping = [](int64_t p){ return p < 0 ? std::numeric_limits<int64_t>::max()/2 : p; };
  int64_t worst_score = std::numeric_limits<int64_t>::min();

  for (const auto &candidate : candidates) {
    int64_t cand = map_ping(candidate.ping_time_ms);
    if (cand > worst_score) {
      worst_score = cand;
      worst_peer_id = candidate.peer_id;
      oldest_connected = candidate.connected_time;
    } else if (cand == worst_score) {
      // Tie-breaker: prefer evicting older connection
      if (candidate.connected_time < oldest_connected) {
        worst_peer_id = candidate.peer_id;
        oldest_connected = candidate.connected_time;
      } else if (candidate.connected_time == oldest_connected) {
        // Final tie-breaker: lower peer_id (deterministic for tests with simultaneous connections)
        if (candidate.peer_id < worst_peer_id) {
          worst_peer_id = candidate.peer_id;
        }
      }
    }
  }

  if (worst_peer_id >= 0) {
    remove_peer(worst_peer_id);
    return true;
  }

  return false;
}

void PeerManager::disconnect_all() {
  // Set stopping flag to prevent new peers from being added
  stopping_all_.store(true, std::memory_order_release);

  bool skip_notifications = shutting_down_.load(std::memory_order_acquire);

  // Get all peers to disconnect
  std::map<int, PeerPtr> peers_to_disconnect;
  peer_states_.ForEach([&](int id, const PerPeerState& state) {
    peers_to_disconnect[id] = state.peer;
  });

  // Publish disconnect notifications for all peers
  if (!skip_notifications) {
    for (const auto &[id, peer] : peers_to_disconnect) {
      if (peer) {
        NetworkEvents().NotifyPeerDisconnected(id, peer->address(), "shutdown");
      }
    }
  }

  // Clear all peer states
  peer_states_.Clear();

  // Disconnect all peers
  for (auto &[id, peer] : peers_to_disconnect) {
    if (peer) {
      peer->disconnect();
    }
  }

  // Allow add_peer() after bulk disconnect completes
  stopping_all_.store(false, std::memory_order_release);
}

void PeerManager::TestOnlySetPeerCreatedAt(int peer_id, std::chrono::steady_clock::time_point tp) {
  peer_states_.Modify(peer_id, [&](PerPeerState& state) {
    state.created_at = tp;
  });
}

void PeerManager::process_periodic() {
  std::vector<int> to_remove;

  // Count peers for logging
  size_t peer_count = 0;
  peer_states_.ForEach([&](int id, const PerPeerState& state) {
    peer_count++;
  });
  LOG_NET_TRACE("process_periodic() peers={}", peer_count);

  // Find disconnected peers and peers marked for disconnection
  peer_states_.ForEach([&](int id, const PerPeerState& state) {
    if (!state.peer->is_connected()) {
      LOG_NET_TRACE("process_periodic: peer={} not connected, marking for removal", id);
      to_remove.push_back(id);
      return;
    }

    // Enforce feeler max lifetime
    if (state.peer->is_feeler()) {
      auto age = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - state.created_at);
      if (age.count() >= FEELER_MAX_LIFETIME_SEC) {
        LOG_NET_TRACE("process_periodic: feeler peer={} exceeded lifetime ({}s >= {}s), marking for removal",
                      id, age.count(), FEELER_MAX_LIFETIME_SEC);
        to_remove.push_back(id);
        return;
      }
    }

    // Check for peers marked for disconnection due to misbehavior
    LOG_NET_TRACE("process_periodic: checking peer={} score={} should_discourage={}",
                  id, state.misbehavior.misbehavior_score, state.misbehavior.should_discourage);

    if (state.misbehavior.should_discourage) {
      // Never disconnect peers with NoBan permission (matches Bitcoin)
      if (HasPermission(state.misbehavior.permissions, NetPermissionFlags::NoBan)) {
        LOG_NET_TRACE("process_periodic: skipping NoBan peer={} (score={} but protected)",
                      id, state.misbehavior.misbehavior_score);
        return;
      }

      // Add to removal list if not already there
      if (std::find(to_remove.begin(), to_remove.end(), id) == to_remove.end()) {
        to_remove.push_back(id);
        LOG_NET_TRACE("process_periodic: marking peer={} for removal (score={})",
                      id, state.misbehavior.misbehavior_score);
        LOG_NET_INFO("process_periodic: Disconnecting misbehaving peer {} (score: {})",
                     id, state.misbehavior.misbehavior_score);
      }
    }
  });

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
  LOG_NET_TRACE("Misbehaving() peer={} penalty={} reason={}", peer_id, penalty, reason);

  bool should_disconnect = false;

  peer_states_.Modify(peer_id, [&](PerPeerState& state) {
    PeerMisbehaviorData &data = state.misbehavior;

    // Add penalty to score (always track, even for NoBan peers - matches Bitcoin)
    int old_score = data.misbehavior_score;
    data.misbehavior_score += penalty;

    LOG_NET_TRACE("Misbehaving() peer={} score: {} -> {} (threshold={})",
                  peer_id, old_score, data.misbehavior_score, DISCOURAGEMENT_THRESHOLD);

    LOG_NET_TRACE("peer {} ({}) misbehavior +{}: {} (total score: {})", peer_id,
                 data.address, penalty, reason, data.misbehavior_score);

    // Check if threshold exceeded
    if (data.misbehavior_score >= DISCOURAGEMENT_THRESHOLD && old_score < DISCOURAGEMENT_THRESHOLD) {
      LOG_NET_TRACE("Misbehaving() peer={} THRESHOLD EXCEEDED", peer_id);

      // Check if peer has NoBan permission (matches Bitcoin: track score but don't disconnect)
      if (HasPermission(data.permissions, NetPermissionFlags::NoBan)) {
        LOG_NET_TRACE("noban peer {} not punished (score {} >= threshold {})",
                     peer_id, data.misbehavior_score, DISCOURAGEMENT_THRESHOLD);
        // DO NOT set should_discourage for NoBan peers
        return;
      }

      // Normal peer: mark for disconnection
      data.should_discourage = true;
      should_disconnect = true;
      LOG_NET_TRACE("peer {} ({}) marked for disconnect (score {} >= threshold {})",
                   peer_id, data.address, data.misbehavior_score,
                   DISCOURAGEMENT_THRESHOLD);
    }
  });

  if (!peer_states_.Get(peer_id)) {
    // Peer not found - may have been disconnected already
    LOG_NET_TRACE("Misbehaving() peer={} not found in map (already disconnected?)", peer_id);
    return false;
  }

  LOG_NET_TRACE("Misbehaving() peer={} threshold not exceeded, continuing", peer_id);
  return should_disconnect;
}

bool PeerManager::ShouldDisconnect(int peer_id) const {
  auto state = peer_states_.Get(peer_id);
  if (!state) {
    return false;
  }

  // Never disconnect peers with NoBan permission (matches Bitcoin)
  if (HasPermission(state->misbehavior.permissions, NetPermissionFlags::NoBan)) {
    return false;
  }

  return state->misbehavior.should_discourage;
}

int PeerManager::GetMisbehaviorScore(int peer_id) const {
  auto state = peer_states_.Get(peer_id);
  if (!state) {
    return 0;
  }

  return state->misbehavior.misbehavior_score;
}

void PeerManager::NoteInvalidHeaderHash(int peer_id, const uint256& hash) {
  peer_states_.Modify(peer_id, [&](PerPeerState& state) {
    state.misbehavior.invalid_header_hashes.insert(hash.GetHex());
  });
}

bool PeerManager::HasInvalidHeaderHash(int peer_id, const uint256& hash) const {
  auto state = peer_states_.Get(peer_id);
  if (!state) return false;
  return state->misbehavior.invalid_header_hashes.find(hash.GetHex()) != state->misbehavior.invalid_header_hashes.end();
}

void PeerManager::IncrementUnconnectingHeaders(int peer_id) {
  bool threshold_exceeded = false;

  peer_states_.Modify(peer_id, [&](PerPeerState& state) {
    PeerMisbehaviorData &data = state.misbehavior;
    if (data.unconnecting_penalized) {
      return; // already penalized; do nothing further
    }
    data.num_unconnecting_headers_msgs++;

    LOG_NET_TRACE("IncrementUnconnectingHeaders: peer {} now has {} unconnecting msgs (threshold={})",
                  peer_id, data.num_unconnecting_headers_msgs, MAX_UNCONNECTING_HEADERS);

    if (data.num_unconnecting_headers_msgs >= MAX_UNCONNECTING_HEADERS) {
      LOG_NET_TRACE("peer {} ({}) sent too many unconnecting headers ({} >= {})",
                   peer_id, data.address, data.num_unconnecting_headers_msgs,
                   MAX_UNCONNECTING_HEADERS);
      data.unconnecting_penalized = true; // latch to avoid repeated penalties
      threshold_exceeded = true;
    }
  });

  if (!peer_states_.Get(peer_id)) {
    LOG_NET_TRACE("IncrementUnconnectingHeaders: peer {} not found in misbehavior map", peer_id);
    return;
  }

  if (threshold_exceeded) {
    Misbehaving(peer_id, MisbehaviorPenalty::TOO_MANY_UNCONNECTING,
                "too many unconnecting headers");
  }
}

void PeerManager::ResetUnconnectingHeaders(int peer_id) {
  peer_states_.Modify(peer_id, [](PerPeerState& state) {
    state.misbehavior.num_unconnecting_headers_msgs = 0;
  });
}

// === PerPeerState Accessors ===

std::optional<uint256> PeerManager::GetLastAnnouncedBlock(int peer_id) const {
  auto state = peer_states_.Get(peer_id);
  if (!state) return std::nullopt;
  return state->last_announced_block;
}

int64_t PeerManager::GetLastAnnounceTime(int peer_id) const {
  auto state = peer_states_.Get(peer_id);
  if (!state) return 0;
  return state->last_announce_time_s;
}

void PeerManager::SetLastAnnouncedBlock(int peer_id, const uint256& hash, int64_t time_s) {
  peer_states_.Modify(peer_id, [&](PerPeerState& state) {
    state.last_announced_block = hash;
    state.last_announce_time_s = time_s;
  });
}

std::vector<uint256> PeerManager::GetBlocksForInvRelay(int peer_id) {
  auto state = peer_states_.Get(peer_id);
  if (!state) return {};
  return state->blocks_for_inv_relay;
}

void PeerManager::AddBlockForInvRelay(int peer_id, const uint256& hash) {
  peer_states_.Modify(peer_id, [&](PerPeerState& state) {
    state.blocks_for_inv_relay.push_back(hash);
  });
}

void PeerManager::ClearBlocksForInvRelay(int peer_id) {
  peer_states_.Modify(peer_id, [](PerPeerState& state) {
    state.blocks_for_inv_relay.clear();
  });
}

bool PeerManager::HasRepliedToGetAddr(int peer_id) const {
  auto state = peer_states_.Get(peer_id);
  if (!state) return false;
  return state->getaddr_replied;
}

void PeerManager::MarkGetAddrReplied(int peer_id) {
  peer_states_.Modify(peer_id, [](PerPeerState& state) {
    state.getaddr_replied = true;
  });
}

void PeerManager::AddLearnedAddress(int peer_id, const AddressKey& key, const LearnedEntry& entry) {
  peer_states_.Modify(peer_id, [&](PerPeerState& state) {
    state.learned_addresses[key] = entry;
  });
}

std::optional<LearnedMap> PeerManager::GetLearnedAddresses(int peer_id) const {
  auto state = peer_states_.Get(peer_id);
  if (!state) return std::nullopt;
  return state->learned_addresses;
}

void PeerManager::ClearLearnedAddresses(int peer_id) {
  peer_states_.Modify(peer_id, [](PerPeerState& state) {
    state.learned_addresses.clear();
  });
}

std::vector<std::pair<int, LearnedMap>> PeerManager::GetAllLearnedAddresses() const {
  std::vector<std::pair<int, LearnedMap>> result;
  peer_states_.ForEach([&](int peer_id, const PerPeerState& state) {
    if (!state.learned_addresses.empty()) {
      result.emplace_back(peer_id, state.learned_addresses);
    }
  });

  // Sort by peer ID to ensure deterministic iteration order
  // (unordered_map iteration is non-deterministic)
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    return a.first < b.first;
  });

  return result;
}

} // namespace network
} // namespace coinbasechain
