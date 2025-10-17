// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_SYNC_PEER_MANAGER_HPP
#define COINBASECHAIN_SYNC_PEER_MANAGER_HPP

#include <string>
#include <map>
#include <mutex>
#include <memory>

namespace coinbasechain {
namespace sync {

// DoS Protection Constants (from Bitcoin Core)
static constexpr int DISCOURAGEMENT_THRESHOLD = 100;  // Misbehavior score threshold for disconnection

// Common misbehavior penalties
namespace MisbehaviorPenalty {
    static constexpr int INVALID_POW = 100;           // Instant disconnect - invalid PoW
    static constexpr int OVERSIZED_MESSAGE = 20;      // Oversized headers message
    static constexpr int NON_CONTINUOUS_HEADERS = 20; // Headers don't connect
    static constexpr int LOW_WORK_HEADERS = 10;       // Low-work header spam (after IBD)
    static constexpr int INVALID_HEADER = 100;        // Invalid header (permanent failure)
    static constexpr int TOO_MANY_UNCONNECTING = 20;  // Too many unconnecting headers messages
    static constexpr int TOO_MANY_ORPHANS = 50;       // Exceeded orphan header limit - moderate penalty
}

// Maximum unconnecting headers messages before penalty
static constexpr int MAX_UNCONNECTING_HEADERS = 10;

// NetPermissionFlags - Permission flags for peer connections (from Bitcoin Core)
enum class NetPermissionFlags : uint32_t {
    None = 0,
    NoBan = (1U << 0),          // Cannot be banned/disconnected for misbehavior
    Manual = (1U << 1),         // Manual connection (addnode RPC)
    // Future flags can be added here (Relay, BloomFilter, etc.)
};

// Bitwise operators for permission flags
inline NetPermissionFlags operator|(NetPermissionFlags a, NetPermissionFlags b) {
    return static_cast<NetPermissionFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline NetPermissionFlags operator&(NetPermissionFlags a, NetPermissionFlags b) {
    return static_cast<NetPermissionFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool HasPermission(NetPermissionFlags flags, NetPermissionFlags check) {
    return (flags & check) == check && static_cast<uint32_t>(check) != 0;
}

// Peer - Tracks state and misbehavior for a single peer connection
// Simplified from Bitcoin's Peer struct for headers-only chain
// No block download tracking, no transaction relay, just header sync
struct Peer {
    int id;                                  // Peer identifier
    int misbehavior_score{0};               // Cumulative misbehavior score
    bool should_discourage{false};          // Mark for disconnection
    std::string address;                    // Peer address (for logging)
    int num_unconnecting_headers_msgs{0};   // Counter for headers that don't connect
    NetPermissionFlags permissions{NetPermissionFlags::None};  // Permission flags

    explicit Peer(int peer_id, const std::string& peer_addr = "unknown",
                  NetPermissionFlags perms = NetPermissionFlags::None)
        : id(peer_id), address(peer_addr), permissions(perms) {}
};

// PeerManager - Manages peer connections and misbehavior tracking
// Simplified from Bitcoin's PeerManager (no actual network management,
// just misbehavior tracking for DoS protection)
class PeerManager {
public:
    PeerManager() = default;
    ~PeerManager() = default;

    void AddPeer(int peer_id, const std::string& address = "unknown",
                 NetPermissionFlags permissions = NetPermissionFlags::None);

    void RemovePeer(int peer_id);

    // Record misbehavior for peer, returns true if should disconnect
    bool Misbehaving(int peer_id, int howmuch, const std::string& message);

    bool ShouldDisconnect(int peer_id) const;
    int GetMisbehaviorScore(int peer_id) const;
    size_t GetPeerCount() const;

    // Returns true if threshold exceeded (peer should be penalized)
    bool IncrementUnconnectingHeaders(int peer_id);

    void ResetUnconnectingHeaders(int peer_id);

private:
    mutable std::mutex peers_mutex_;
    std::map<int, std::shared_ptr<Peer>> peers_;
};

} // namespace sync
} // namespace coinbasechain

#endif // COINBASECHAIN_SYNC_PEER_MANAGER_HPP
