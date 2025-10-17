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

/**
 * DoS Protection Constants
 * Based on Bitcoin Core design
 */
static constexpr int DISCOURAGEMENT_THRESHOLD = 100;  // Misbehavior score threshold for disconnection

/**
 * Common misbehavior penalties
 */
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

/**
 * NetPermissionFlags - Permission flags for peer connections
 * Based on Bitcoin Core's NetPermissionFlags
 */
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

/**
 * Peer - Tracks state and misbehavior for a single peer connection
 *
 * Simplified from Bitcoin's Peer struct for headers-only chain.
 * No block download tracking, no transaction relay, just header sync.
 */
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

/**
 * PeerManager - Manages peer connections and misbehavior tracking
 *
 * Responsibilities:
 * - Track misbehavior scores for all connected peers
 * - Mark peers for disconnection when threshold exceeded
 * - Provide thread-safe access to peer state
 *
 * Simplified from Bitcoin's PeerManager (no actual network management,
 * just misbehavior tracking for DoS protection).
 */
class PeerManager {
public:
    PeerManager() = default;
    ~PeerManager() = default;

    /**
     * Add a new peer
     * @param peer_id Peer identifier
     * @param address Peer address (for logging)
     * @param permissions Permission flags (NoBan, Manual, etc.)
     */
    void AddPeer(int peer_id, const std::string& address = "unknown",
                 NetPermissionFlags permissions = NetPermissionFlags::None);

    /**
     * Remove a peer (on disconnect)
     */
    void RemovePeer(int peer_id);

    /**
     * Record misbehavior for a peer
     *
     * @param peer_id Peer identifier
     * @param howmuch Misbehavior penalty to add
     * @param message Reason for misbehavior (for logging)
     * @return true if peer should be disconnected
     */
    bool Misbehaving(int peer_id, int howmuch, const std::string& message);

    /**
     * Check if peer should be disconnected
     */
    bool ShouldDisconnect(int peer_id) const;

    /**
     * Get peer's current misbehavior score
     */
    int GetMisbehaviorScore(int peer_id) const;

    /**
     * Get peer count (for stats/debugging)
     */
    size_t GetPeerCount() const;

    /**
     * Increment unconnecting headers counter
     * @return true if threshold exceeded (peer should be penalized)
     */
    bool IncrementUnconnectingHeaders(int peer_id);

    /**
     * Reset unconnecting headers counter (on successful connection)
     */
    void ResetUnconnectingHeaders(int peer_id);

private:
    mutable std::mutex peers_mutex_;
    std::map<int, std::shared_ptr<Peer>> peers_;
};

} // namespace sync
} // namespace coinbasechain

#endif // COINBASECHAIN_SYNC_PEER_MANAGER_HPP
