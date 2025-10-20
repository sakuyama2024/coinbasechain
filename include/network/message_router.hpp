#ifndef COINBASECHAIN_MESSAGE_ROUTER_HPP
#define COINBASECHAIN_MESSAGE_ROUTER_HPP

#include "network/peer.hpp"
#include "network/message.hpp"
#include <memory>

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

private:
  AddressManager* addr_manager_;
  HeaderSyncManager* header_sync_manager_;
  BlockRelayManager* block_relay_manager_;

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

#endif // COINBASECHAIN_MESSAGE_ROUTER_HPP
