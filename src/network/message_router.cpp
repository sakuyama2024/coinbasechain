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

  // Mark outbound connections as successful in address manager
  // Bitcoin Core: Does this in PeerManagerImpl::FinalizeNode() for all outbound types
  // This saves the address to the "tried" table for reconnection on restart
  // Note: FEELER connections are marked good in NetworkManager before peer->start()
  if (!peer->is_inbound() && !peer->is_feeler() && addr_manager_) {
    protocol::NetworkAddress addr = protocol::NetworkAddress::from_string(
        peer->address(), peer->port());
    // Add to address manager first if not already there (e.g., from anchors or -addnode)
    // This ensures good() can move it to the "tried" table
    addr_manager_->add(addr);
    addr_manager_->good(addr);
    LOG_NET_DEBUG("Marked outbound peer {}:{} as good in address manager",
                  peer->address(), peer->port());

    // Request additional peer addresses (Bitcoin Core: GETADDR after handshake, rate-limited)
    if (!peer->has_sent_getaddr()) {
      auto getaddr = std::make_unique<message::GetAddrMessage>();
      peer->send_message(std::move(getaddr));
      peer->mark_getaddr_sent();
      LOG_NET_DEBUG("Sent GETADDR to {}:{} to populate address manager", peer->address(), peer->port());
    }
  }

  // Announce our tip to this peer immediately (Bitcoin Core does this with no time throttling)
  // This allows peers to discover our chain and request headers if we're ahead
  if (block_relay_manager_) {
    block_relay_manager_->AnnounceTipToPeer(peer.get());
  }

  // Note: Initial header sync is now initiated in NetworkManager::run_sendmessages()
  // via periodic CheckInitialSync() calls (Bitcoin Core pattern)

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

  // Privacy: Only respond to inbound peers
  if (!peer->is_inbound()) {
    LOG_NET_TRACE("Ignoring GETADDR from outbound peer {}", peer->id());
    return true; // Not an error; just ignore
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
