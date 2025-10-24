#include "network/message_router.hpp"
#include "network/addr_manager.hpp"
#include "network/header_sync_manager.hpp"
#include "network/block_relay_manager.hpp"
#include "network/protocol.hpp"
#include "chain/logging.hpp"

namespace coinbasechain {
namespace network {

MessageRouter::MessageRouter(AddressManager* addr_mgr,
                             HeaderSyncManager* header_sync,
                             BlockRelayManager* block_relay)
    : addr_manager_(addr_mgr),
      header_sync_manager_(header_sync),
      block_relay_manager_(block_relay) {}

bool MessageRouter::RouteMessage(PeerPtr peer, std::unique_ptr<message::Message> msg) {
  if (!msg || !peer) {
    return false;
  }

  const std::string &command = msg->command();
  LOG_NET_TRACE("MessageRouter: routing peer={} command={}", peer->id(), command);

  // Route to appropriate handler based on message type
  if (command == protocol::commands::VERACK) {
    return handle_verack(peer);
  }

  if (command == protocol::commands::ADDR) {
    auto *addr_msg = dynamic_cast<message::AddrMessage *>(msg.get());
    return handle_addr(peer, addr_msg);
  }

  if (command == protocol::commands::GETADDR) {
    return handle_getaddr(peer);
  }

  if (command == protocol::commands::INV) {
    auto *inv_msg = dynamic_cast<message::InvMessage *>(msg.get());
    return handle_inv(peer, inv_msg);
  }

  if (command == protocol::commands::HEADERS) {
    auto *headers_msg = dynamic_cast<message::HeadersMessage *>(msg.get());
    return handle_headers(peer, headers_msg);
  }

  if (command == protocol::commands::GETHEADERS) {
    auto *getheaders_msg = dynamic_cast<message::GetHeadersMessage *>(msg.get());
    return handle_getheaders(peer, getheaders_msg);
  }

  // Unknown message - log and return true (not an error)
  LOG_NET_TRACE("MessageRouter: unhandled message type: {}", command);
  return true;
}

bool MessageRouter::handle_verack(PeerPtr peer) {
  // Peer has completed handshake (VERSION/VERACK exchange)
  if (!peer->successfully_connected()) {
    return true;
  }

  // Announce our tip to this peer immediately (Bitcoin Core does this with no time throttling)
  // This allows peers to discover our chain and request headers if we're ahead
  if (block_relay_manager_) {
    block_relay_manager_->AnnounceTipToPeer(peer.get());
  }

  // Initiate header sync if needed (only if we don't have a sync peer yet)
  if (header_sync_manager_) {
    uint64_t current_sync_peer = header_sync_manager_->GetSyncPeerId();
    if (current_sync_peer == 0) {
      // No sync peer yet, try to set this peer as sync peer
      // Note: Logging is done in CheckInitialSync() to match Bitcoin Core patterns
      header_sync_manager_->SetSyncPeer(peer->id());

      // Send GETHEADERS to initiate sync (like Bitcoin's "initial getheaders")
      header_sync_manager_->RequestHeadersFromPeer(peer);
    }
  }

  return true;
}

bool MessageRouter::handle_addr(PeerPtr peer, message::AddrMessage* msg) {
  if (!msg || !addr_manager_) {
    return false;
  }

  addr_manager_->add_multiple(msg->addresses);
  return true;
}

bool MessageRouter::handle_getaddr(PeerPtr peer) {
  if (!addr_manager_) {
    return false;
  }

  auto response = std::make_unique<message::AddrMessage>();
  response->addresses = addr_manager_->get_addresses(protocol::MAX_ADDR_SIZE);
  peer->send_message(std::move(response));
  return true;
}

bool MessageRouter::handle_inv(PeerPtr peer, message::InvMessage* msg) {
  if (!msg || !block_relay_manager_) {
    return false;
  }

  return block_relay_manager_->HandleInvMessage(peer, msg);
}

bool MessageRouter::handle_headers(PeerPtr peer, message::HeadersMessage* msg) {
  if (!msg || !header_sync_manager_) {
    return false;
  }

  return header_sync_manager_->HandleHeadersMessage(peer, msg);
}

bool MessageRouter::handle_getheaders(PeerPtr peer, message::GetHeadersMessage* msg) {
  if (!msg || !header_sync_manager_) {
    return false;
  }

  return header_sync_manager_->HandleGetHeadersMessage(peer, msg);
}

} // namespace network
} // namespace coinbasechain
