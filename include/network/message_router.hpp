#pragma once

#include "network/peer.hpp"
#include "network/message.hpp"
#include <memory>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <deque>
#include <vector>
#include <random>

namespace coinbasechain {

// Forward declarations
namespace network {
class AddressManager;
class HeaderSyncManager;
class BlockRelayManager;
class PeerManager;
}

namespace network {

// MessageRouter - Routes incoming protocol messages to appropriate handlers
// Coordinates message handling between specialized managers
class MessageRouter {
public:
  MessageRouter(AddressManager* addr_mgr,
                HeaderSyncManager* header_sync,
                BlockRelayManager* block_relay,
                PeerManager* peer_mgr = nullptr);

  // Route incoming message to appropriate handler
  // Returns true if message was handled successfully
  bool RouteMessage(PeerPtr peer, std::unique_ptr<message::Message> msg);

  // Peer lifecycle - cleanup per-peer state on disconnect
  void OnPeerDisconnected(int peer_id);

  // Debug stats snapshot for GETADDR handling (for tests/triage)
  struct GetAddrDebugStats {
    uint64_t total{0};
    uint64_t served{0};
    uint64_t ignored_outbound{0};
    uint64_t ignored_prehandshake{0};
    uint64_t ignored_repeat{0};
    size_t last_from_addrman{0};
    size_t last_from_recent{0};
    size_t last_from_learned{0};
    size_t last_suppressed{0};
  };
  GetAddrDebugStats GetGetAddrDebugStats() const;
  // Test-only: seed RNG for deterministic shuffles
  void TestSeedRng(uint64_t seed);

private:
  AddressManager* addr_manager_;
  HeaderSyncManager* header_sync_manager_;
  BlockRelayManager* block_relay_manager_;
  PeerManager* peer_manager_;

  // AddressKey for binary IP:port keying (avoids string encode/decode)
  struct AddressKey {
    std::array<uint8_t,16> ip{};
    uint16_t port{0};
    struct Hasher {
      size_t operator()(const AddressKey& k) const noexcept {
        // FNV-1a 64-bit
        uint64_t h = 1469598103934665603ULL;
        auto mix = [&](uint8_t b){ h ^= b; h *= 1099511628211ULL; };
        for (auto b : k.ip) mix(b);
        mix(static_cast<uint8_t>(k.port >> 8));
        mix(static_cast<uint8_t>(k.port & 0xFF));
        return static_cast<size_t>(h);
      }
    };
    bool operator==(const AddressKey& o) const noexcept {
      return port == o.port && ip == o.ip;
    }
  };

  // Learned address entry (preserves services and timestamp)
  struct LearnedEntry {
    protocol::TimestampedAddress ts_addr{};
    int64_t last_seen_s{0};
  };

  using LearnedMap = std::unordered_map<AddressKey, LearnedEntry, AddressKey::Hasher>;

  // GETADDR policy: once-per-connection tracking
  std::unordered_set<int> getaddr_replied_;

  // Echo suppression TTL (do not echo back addresses learned from the requester within TTL)
  static constexpr int64_t ECHO_SUPPRESS_TTL_SEC = 600; // 10 minutes

  // Cap per-peer learned cache to bound memory
  static constexpr size_t MAX_LEARNED_PER_PEER = 2000;

  // Per-peer learned addresses (for echo suppression)
  std::unordered_map<int, LearnedMap> learned_addrs_by_peer_;

  // Mutex guarding per-peer GETADDR/echo-suppression maps and recent ring
  mutable std::mutex addr_mutex_;

  // Recently learned addresses (global ring buffer) to improve GETADDR responsiveness
  std::deque<protocol::TimestampedAddress> recent_addrs_;
  static constexpr size_t RECENT_ADDRS_MAX = 5000;

  // Debug counters/state for GETADDR decisions
  mutable std::mutex stats_mutex_;
  uint64_t stats_getaddr_total_{0};
  uint64_t stats_getaddr_served_{0};
  uint64_t stats_getaddr_ignored_outbound_{0};
  uint64_t stats_getaddr_ignored_prehandshake_{0};
  uint64_t stats_getaddr_ignored_repeat_{0};
  size_t last_resp_from_addrman_{0};
  size_t last_resp_from_recent_{0};
  size_t last_resp_from_learned_{0};
  size_t last_resp_suppressed_{0};

  // RNG for GETADDR reply randomization
  std::mt19937 rng_;

  // Helper to build binary key
  static AddressKey MakeKey(const protocol::NetworkAddress& a);

  // Message-specific handlers
  bool handle_verack(PeerPtr peer);
  bool handle_addr(PeerPtr peer, message::AddrMessage* msg);
  bool handle_getaddr(PeerPtr peer);
  bool handle_inv(PeerPtr peer, message::InvMessage* msg);
  bool handle_headers(PeerPtr peer, message::HeadersMessage* msg);
  bool handle_getheaders(PeerPtr peer, message::GetHeadersMessage* msg);
};

} // namespace network
} // namespace coinbasechain

