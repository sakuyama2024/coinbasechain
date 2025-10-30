#pragma once

#include "network/peer.hpp"
#include "network/message.hpp"
#include <memory>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace coinbasechain {

// Forward declarations
namespace network {
class AddressManager;
class HeaderSyncManager;
class BlockRelayManager;
}

namespace network {

// MessageRouter - Routes incoming protocol messages to appropriate handlers
// Coordinates message handling between specialized managers (inspired by Bitcoin's ProcessMessage)
class MessageRouter {
public:
  MessageRouter(AddressManager* addr_mgr,
                HeaderSyncManager* header_sync,
                BlockRelayManager* block_relay);

  // Route incoming message to appropriate handler
  // Returns true if message was handled successfully
  bool RouteMessage(PeerPtr peer, std::unique_ptr<message::Message> msg);

  // Peer lifecycle - cleanup per-peer state on disconnect
  void OnPeerDisconnected(int peer_id);

private:
  AddressManager* addr_manager_;
  HeaderSyncManager* header_sync_manager_;
  BlockRelayManager* block_relay_manager_;

  // GETADDR rate limiting per-peer: last reply time (unix seconds via util::GetTime())
  std::unordered_map<int, int64_t> last_getaddr_reply_s_;
  static constexpr int64_t GETADDR_COOLDOWN_SEC = 60; // 60s

  // Echo suppression: track addresses learned from each peer recently
  // key: peer_id -> (addr_key -> last_seen_unix_seconds)
  std::unordered_map<int, std::unordered_map<std::string, int64_t>> learned_addrs_by_peer_;
  static constexpr int64_t ECHO_SUPPRESS_TTL_SEC = 600; // 10 minutes

  // Mutex guarding per-peer GETADDR/echo-suppression maps
  mutable std::mutex addr_mutex_;

  // Recently learned addresses (global ring buffer) to improve GETADDR responsiveness
  std::vector<protocol::TimestampedAddress> recent_addrs_;
  static constexpr size_t RECENT_ADDRS_MAX = 5000;

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


