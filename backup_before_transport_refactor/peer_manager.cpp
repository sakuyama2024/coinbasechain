#include "network/peer_manager.hpp"
#include "util/logging.hpp"
#include <algorithm>

namespace coinbasechain {
namespace network {

PeerManager::PeerManager(boost::asio::io_context& io_context,
                         AddressManager& addr_manager,
                         const Config& config)
    : io_context_(io_context)
    , addr_manager_(addr_manager)
    , config_(config) {
}

PeerManager::~PeerManager() {
    disconnect_all();
}

int PeerManager::allocate_peer_id() {
    return next_peer_id_++;
}

bool PeerManager::add_peer(PeerPtr peer) {
    if (!peer) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Check connection limits
    bool is_inbound = peer->is_inbound();
    size_t current_inbound = 0;
    size_t current_outbound = 0;

    for (const auto& [id, p] : peers_) {
        if (p->is_inbound()) {
            current_inbound++;
        } else {
            current_outbound++;
        }
    }

    // Check outbound limit (no eviction for outbound)
    if (!is_inbound && current_outbound >= config_.max_outbound_peers) {
        return false; // Too many outbound connections
    }

    // Check inbound limit - try eviction if at capacity
    if (is_inbound && current_inbound >= config_.max_inbound_peers) {
        // Release lock temporarily to call evict_inbound_peer
        // (evict_inbound_peer will acquire its own lock)
        mutex_.unlock();
        bool evicted = evict_inbound_peer();
        mutex_.lock();

        if (!evicted) {
            return false; // Couldn't evict anyone, reject connection
        }
        // Successfully evicted a peer, continue with adding new peer
    }

    // Allocate ID and add peer
    int peer_id = allocate_peer_id();
    peers_[peer_id] = std::move(peer);

    return true;
}

void PeerManager::remove_peer(int peer_id) {
    PeerPtr peer;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = peers_.find(peer_id);
        if (it == peers_.end()) {
            // Peer already removed - this is OK, just return silently
            return;
        }

        peer = it->second;
        peers_.erase(it);
        LOG_NET_INFO("remove_peer: Erased peer {} from map (map size now: {})", peer_id, peers_.size());
    }

    // Disconnect outside the lock
    if (peer) {
        LOG_NET_INFO("remove_peer: Calling disconnect() on peer {}", peer_id);
        peer->disconnect();
    }

    // Notify callback
    if (peer_removed_callback_) {
        peer_removed_callback_(peer_id);
    }
}

PeerPtr PeerManager::get_peer(int peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peers_.find(peer_id);
    return (it != peers_.end()) ? it->second : nullptr;
}

int PeerManager::find_peer_by_address(const std::string& address, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Search all peers for matching address:port
    for (const auto& [id, peer] : peers_) {
        if (!peer) continue;

        // Match both address and port
        if (peer->address() == address && peer->port() == port) {
            return id;
        }
    }

    return -1;  // Not found
}

std::vector<PeerPtr> PeerManager::get_all_peers() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PeerPtr> result;
    result.reserve(peers_.size());

    for (const auto& [id, peer] : peers_) {
        result.push_back(peer);
    }

    return result;
}

std::vector<PeerPtr> PeerManager::get_outbound_peers() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PeerPtr> result;

    for (const auto& [id, peer] : peers_) {
        if (!peer->is_inbound()) {
            result.push_back(peer);
        }
    }

    return result;
}

std::vector<PeerPtr> PeerManager::get_inbound_peers() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PeerPtr> result;

    for (const auto& [id, peer] : peers_) {
        if (peer->is_inbound()) {
            result.push_back(peer);
        }
    }

    return result;
}

size_t PeerManager::peer_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peers_.size();
}

size_t PeerManager::outbound_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;

    for (const auto& [id, peer] : peers_) {
        if (!peer->is_inbound()) {
            count++;
        }
    }

    return count;
}

size_t PeerManager::inbound_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;

    for (const auto& [id, peer] : peers_) {
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

    for (const auto& [id, peer] : peers_) {
        // Only consider inbound peers
        if (!peer->is_inbound()) {
            continue;
        }

        // Protect recently connected peers (within 10 seconds)
        auto connection_age = std::chrono::duration_cast<std::chrono::seconds>(
            now - peer->stats().connected_time
        );
        if (connection_age.count() < 10) {
            continue;
        }

        candidates.push_back({
            id,
            peer->stats().connected_time,
            peer->stats().ping_time_ms
        });
    }

    // If no candidates, can't evict
    if (candidates.empty()) {
        return false;
    }

    // Simple eviction strategy for headers-only chain:
    // Evict the peer with the worst (highest) ping time, or oldest connection if no ping data
    int worst_peer_id = -1;
    int64_t worst_ping = -1;
    auto oldest_connected = std::chrono::steady_clock::time_point::max();

    for (const auto& candidate : candidates) {
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
    }

    // Disconnect all peers outside the lock
    for (auto& [id, peer] : peers_to_disconnect) {
        if (peer) {
            peer->disconnect();
        }
    }
}

void PeerManager::process_periodic() {
    std::vector<int> to_remove;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Find disconnected peers
        for (const auto& [id, peer] : peers_) {
            if (!peer->is_connected()) {
                to_remove.push_back(id);
            }
        }
    }

    // Remove disconnected peers
    for (int peer_id : to_remove) {
        remove_peer(peer_id);
    }

    // Cleanup stale addresses in AddressManager
    addr_manager_.cleanup_stale();
}

void PeerManager::set_peer_removed_callback(PeerRemovedCallback callback) {
    peer_removed_callback_ = std::move(callback);
}

} // namespace network
} // namespace coinbasechain
