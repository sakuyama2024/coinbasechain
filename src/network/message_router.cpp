#include "network/message_router.hpp"
#include "network/addr_manager.hpp"
#include "network/header_sync_manager.hpp"
#include "network/block_relay_manager.hpp"
#include "network/protocol.hpp"
#include "util/logging.hpp"
#include "util/time.hpp"
#include <sstream>
#include <iomanip>
#include <unordered_set>

namespace coinbasechain {
namespace network {

namespace {
// Create a stable string key for a NetworkAddress: 16-byte hex + ':' + port
static std::string MakeAddrKey(const protocol::NetworkAddress& a) {
  std::ostringstream ss;
  for (size_t i = 0; i < 16; ++i) {
    ss << std::hex << std::setw(2) << std::setfill('0') << (int)a.ip[i];
  }
  ss << ":" << std::dec << a.port;
  return ss.str();
}

// Parse an address key back into a NetworkAddress; returns false on failure
static bool ParseAddrKey(const std::string& key, protocol::NetworkAddress& out) {
  auto pos = key.find(':');
  if (pos == std::string::npos) return false;
  const std::string hex = key.substr(0, pos);
  const std::string port_str = key.substr(pos + 1);
  if (hex.size() != 32) return false; // 16 bytes * 2 hex chars
  for (size_t i = 0; i < 16; ++i) {
    std::string byte_hex = hex.substr(i * 2, 2);
    unsigned int byte_val = 0;
    std::istringstream iss(byte_hex);
    iss >> std::hex >> byte_val;
    if (iss.fail()) return false;
    out.ip[i] = static_cast<uint8_t>(byte_val & 0xFF);
  }
  try {
    int p = std::stoi(port_str);
    if (p < 0 || p > 65535) return false;
    out.port = static_cast<uint16_t>(p);
  } catch (...) {
    return false;
  }
  out.services = protocol::ServiceFlags::NODE_NETWORK;
  return true;
}
} // namespace

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

  return true;
}

bool MessageRouter::handle_addr(PeerPtr peer, message::AddrMessage* msg) {
  if (!msg || !addr_manager_) {
    return false;
  }

  // Add to AddressManager
  addr_manager_->add_multiple(msg->addresses);

  // Record learned addresses for echo-suppression against this peer
  const int peer_id = peer ? peer->id() : -1;
  if (peer_id >= 0) {
    const int64_t now_s = util::GetTime();
    std::lock_guard<std::mutex> g(addr_mutex_);
    auto& learned = learned_addrs_by_peer_[peer_id];

    // Prune old entries to avoid unbounded growth
    for (auto it = learned.begin(); it != learned.end(); ) {
      const int64_t age = now_s - it->second;
      if (age > ECHO_SUPPRESS_TTL_SEC) {
        it = learned.erase(it);
      } else {
        ++it;
      }
    }

    for (const auto& ta : msg->addresses) {
      learned[MakeAddrKey(ta.address)] = now_s;
      // Record globally (ring buffer)
      recent_addrs_.push_back(ta);
      if (recent_addrs_.size() > RECENT_ADDRS_MAX) {
        recent_addrs_.erase(recent_addrs_.begin(), recent_addrs_.begin() + (recent_addrs_.size() - RECENT_ADDRS_MAX));
      }
    }
  }
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

  const int peer_id = peer->id();
  const int64_t now_s = util::GetTime();

  // Check and update rate limit under lock
  {
    std::lock_guard<std::mutex> g(addr_mutex_);
    auto it = last_getaddr_reply_s_.find(peer_id);
    if (it != last_getaddr_reply_s_.end()) {
      const int64_t elapsed = now_s - it->second;
      if (elapsed >= 0 && elapsed < GETADDR_COOLDOWN_SEC) {
        LOG_NET_TRACE("Rate-limiting GETADDR from peer {} (elapsed={}s < cooldown={}s)", peer_id, elapsed, GETADDR_COOLDOWN_SEC);
        return true; // silently ignore within cooldown
      }
    }
    last_getaddr_reply_s_[peer_id] = now_s;
  }

  // Copy suppression map for this peer while pruning old entries (under lock)
  std::unordered_map<std::string, int64_t> suppression_map_copy;
  {
    std::lock_guard<std::mutex> g(addr_mutex_);
    auto& learned = learned_addrs_by_peer_[peer_id];
    for (auto it = learned.begin(); it != learned.end(); ) {
      const int64_t age = now_s - it->second;
      if (age > ECHO_SUPPRESS_TTL_SEC) {
        it = learned.erase(it);
      } else {
        ++it;
      }
    }
    suppression_map_copy = learned; // copy after pruning
  }

  // Build response outside lock
  auto response = std::make_unique<message::AddrMessage>();
  auto addrs = addr_manager_->get_addresses(protocol::MAX_ADDR_SIZE);


  // Also derive the peer's own address key to avoid sending it back
  std::string peer_addr_key;
  try {
    protocol::NetworkAddress peer_na = protocol::NetworkAddress::from_string(peer->address(), peer->port());
    peer_addr_key = MakeAddrKey(peer_na);
  } catch (...) {
    peer_addr_key.clear();
  }

  // Build candidate set of addresses learned recently from ANY peers,
  // excluding those learned from the requesting peer within TTL (echo suppression)
  std::unordered_set<std::string> candidate_keys;
  {
    std::lock_guard<std::mutex> g(addr_mutex_);
    for (const auto& kv : learned_addrs_by_peer_) {
      for (const auto& kv2 : kv.second) {
        const int64_t age = now_s - kv2.second;
        // Consider all learned addresses (regardless of age); suppression logic will block recent echoes
        // Skip if this key is in the requester's suppression map (i.e., learned from them within TTL)
        auto it_req = suppression_map_copy.find(kv2.first);
        if (kv.first == peer_id && it_req != suppression_map_copy.end()) {
          continue;
        }
        candidate_keys.insert(kv2.first);
      }
    }
  }

  // Helper to decide suppression for a given key
  auto is_suppressed = [&](const std::string& key)->bool {
    if (!peer_addr_key.empty() && key == peer_addr_key) return true;
    auto it_k = suppression_map_copy.find(key);
    if (it_k != suppression_map_copy.end()) {
      const int64_t age = now_s - it_k->second;
      if (age >= 0 && age <= ECHO_SUPPRESS_TTL_SEC) return true;
    }
    return false;
  };

  // First pass: include learned candidate addresses even if addrman does not return them
  std::unordered_set<std::string> included_keys;

  // First add from the recent ring buffer (most recent first)
  {
    std::lock_guard<std::mutex> g(addr_mutex_);
    for (auto it = recent_addrs_.rbegin(); it != recent_addrs_.rend(); ++it) {
      if (response->addresses.size() >= protocol::MAX_ADDR_SIZE) break;
      const std::string key = MakeAddrKey(it->address);
      if (is_suppressed(key)) continue;
      if (included_keys.find(key) != included_keys.end()) continue;
      response->addresses.push_back(*it);
      included_keys.insert(key);
    }
  }

  // Include learned candidate addresses even if addrman does not return them
  for (const auto& key : candidate_keys) {
    if (response->addresses.size() >= protocol::MAX_ADDR_SIZE) break;
    if (included_keys.find(key) != included_keys.end()) continue;
    if (is_suppressed(key)) continue;
    protocol::NetworkAddress na;
    if (ParseAddrKey(key, na)) {
      protocol::TimestampedAddress ta;
      ta.timestamp = static_cast<uint32_t>(now_s);
      ta.address = na;
      response->addresses.push_back(ta);
      included_keys.insert(key);
    }
  }

  // Second pass: include any matching addresses from addrman output that we haven't added yet
  for (const auto& ta : addrs) {
    if (response->addresses.size() >= protocol::MAX_ADDR_SIZE) break;
    const std::string key = MakeAddrKey(ta.address);
    if (included_keys.find(key) != included_keys.end()) continue;
    if (is_suppressed(key)) continue;
    response->addresses.push_back(ta);
    included_keys.insert(key);
  }

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

void MessageRouter::OnPeerDisconnected(int peer_id) {
  std::lock_guard<std::mutex> g(addr_mutex_);
  last_getaddr_reply_s_.erase(peer_id);
  learned_addrs_by_peer_.erase(peer_id);
}

} // namespace network
} // namespace coinbasechain
