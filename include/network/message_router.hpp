#pragma once

#include "network/peer.hpp"
#include "network/message.hpp"
#include "network/notifications.hpp"
#include "network/peer_state.hpp"  // For AddressKey, LearnedEntry, LearnedMap
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

// MessageRouter - Provides message handler implementations
// NOTE: Message routing is now handled by MessageDispatcher (handler registry pattern)
// This class provides the actual handler implementations and will be phased out as
// handlers are migrated to specialized managers (Phase 4 refactoring)
class MessageRouter {
public:
  MessageRouter(AddressManager* addr_mgr,
                HeaderSyncManager* header_sync,
                BlockRelayManager* block_relay,
                PeerManager* peer_mgr = nullptr);

  // DEPRECATED: Route message to handler (kept for test compatibility)
  // Production code should use MessageDispatcher instead
  // This will be removed when MessageRouter is phased out (Phase 4)
  bool RouteMessage(PeerPtr peer, std::unique_ptr<message::Message> msg);

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

  // Message-specific handlers (public for MessageDispatcher integration)
  bool handle_verack(PeerPtr peer);
  bool handle_addr(PeerPtr peer, message::AddrMessage* msg);
  bool handle_getaddr(PeerPtr peer);
  bool handle_inv(PeerPtr peer, message::InvMessage* msg);
  bool handle_headers(PeerPtr peer, message::HeadersMessage* msg);
  bool handle_getheaders(PeerPtr peer, message::GetHeadersMessage* msg);

private:
  // Peer lifecycle - cleanup per-peer state on disconnect (via NetworkNotifications)
  void OnPeerDisconnected(int peer_id);

  AddressManager* addr_manager_;
  HeaderSyncManager* header_sync_manager_;
  BlockRelayManager* block_relay_manager_;
  PeerManager* peer_manager_;

  // Note: AddressKey, LearnedEntry, and LearnedMap are now defined in peer_state.hpp
  // and shared across PeerManager and MessageRouter

  // Note: Per-peer GETADDR reply tracking and learned addresses are now stored
  // in PeerManager's consolidated PerPeerState. No separate maps needed.

  // Echo suppression TTL (do not echo back addresses learned from the requester within TTL)
  static constexpr int64_t ECHO_SUPPRESS_TTL_SEC = 600; // 10 minutes

  // Cap per-peer learned cache to bound memory
  static constexpr size_t MAX_LEARNED_PER_PEER = 2000;

  // Mutex guarding recent ring buffer (recent_addrs_)
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

  // NetworkNotifications subscription (RAII cleanup on destruction)
  NetworkNotifications::Subscription peer_disconnect_subscription_;

  // Helper to build binary key (uses shared AddressKey from peer_state.hpp)
  static network::AddressKey MakeKey(const protocol::NetworkAddress& a);
};

} // namespace network
} // namespace coinbasechain

