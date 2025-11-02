#include "network/message_router.hpp"
#include "network/addr_manager.hpp"
#include "network/header_sync_manager.hpp"
#include "network/block_relay_manager.hpp"
#include "network/peer_manager.hpp"
#include "network/protocol.hpp"
#include "util/logging.hpp"
#include "util/time.hpp"
#include <unordered_set>
#include <algorithm>
#include <cassert>
#include <random>

namespace coinbasechain {
namespace network {

MessageRouter::MessageRouter(AddressManager* addr_mgr,
                             HeaderSyncManager* header_sync,
                             BlockRelayManager* block_relay,
                             PeerManager* peer_mgr)
    : addr_manager_(addr_mgr),
      header_sync_manager_(header_sync),
      block_relay_manager_(block_relay),
      peer_manager_(peer_mgr),
      rng_(std::random_device{}()) {}

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
    if (!addr_msg) {
      LOG_NET_ERROR("MessageRouter: bad payload type for ADDR from peer {}", peer ? peer->id() : -1);
      return false;
    }
    return handle_addr(peer, addr_msg);
  }

  if (command == protocol::commands::GETADDR) {
    return handle_getaddr(peer);
  }

if (command == protocol::commands::INV) {
    auto *inv_msg = dynamic_cast<message::InvMessage *>(msg.get());
    if (!inv_msg) {
      LOG_NET_ERROR("MessageRouter: bad payload type for INV from peer {}", peer ? peer->id() : -1);
      return false;
    }
    return handle_inv(peer, inv_msg);
  }

if (command == protocol::commands::HEADERS) {
    auto *headers_msg = dynamic_cast<message::HeadersMessage *>(msg.get());
    if (!headers_msg) {
      LOG_NET_ERROR("MessageRouter: bad payload type for HEADERS from peer {}", peer ? peer->id() : -1);
      return false;
    }
    return handle_headers(peer, headers_msg);
  }

if (command == protocol::commands::GETHEADERS) {
    auto *getheaders_msg = dynamic_cast<message::GetHeadersMessage *>(msg.get());
    if (!getheaders_msg) {
      LOG_NET_ERROR("MessageRouter: bad payload type for GETHEADERS from peer {}", peer ? peer->id() : -1);
      return false;
    }
    return handle_getheaders(peer, getheaders_msg);
  }

  // Unknown message - log and return true (not an error)
  LOG_NET_TRACE("MessageRouter: unhandled message type: {}", command);
  return true;
}

bool MessageRouter::handle_verack(PeerPtr peer) {
  // Verify peer is still connected
  if (!peer || !peer->is_connected()) {
    LOG_NET_TRACE("Ignoring VERACK from disconnected peer");
    return true;
  }
  // Router is invoked after Peer::handle_verack() marks the peer as successfully connected.
  // Sanity check: by this point, the peer must be successfully_connected().
  assert(peer->successfully_connected() && "VERACK routed before peer marked successfully connected");
  if (!peer->successfully_connected()) {
    return true; // Defensive in release builds
  }

  // Mark outbound connections as successful in address manager
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

  // Announce our tip to this peer immediately
  // This allows peers to discover our chain and request headers if we're ahead
  if (block_relay_manager_) {
    block_relay_manager_->AnnounceTipToPeer(peer.get());
  }

  return true;
}

bool MessageRouter::handle_addr(PeerPtr peer, message::AddrMessage* msg) {
  if (!msg) {
    return false;
  }

  // Gate ADDR on post-VERACK (Bitcoin Core parity) - check before null manager
  if (!peer || !peer->successfully_connected()) {
    LOG_NET_TRACE("Ignoring ADDR from non-connected peer");
    return true; // Not an error, just gated
  }

  if (!addr_manager_) {
    return false;
  }

  // Validate size and apply misbehavior if available
  if (msg->addresses.size() > protocol::MAX_ADDR_SIZE) {
    LOG_NET_WARN("Peer {} sent oversized ADDR message ({} addrs, max {}), truncating",
                 peer->id(), msg->addresses.size(), protocol::MAX_ADDR_SIZE);
    if (peer_manager_) {
      peer_manager_->ReportOversizedMessage(peer->id());
      if (peer_manager_->ShouldDisconnect(peer->id())) {
        peer_manager_->remove_peer(peer->id());
        return false;
      }
    }
    msg->addresses.resize(protocol::MAX_ADDR_SIZE);
  }

  // Feed AddressManager after validation
  addr_manager_->add_multiple(msg->addresses);

  const int peer_id = peer->id();
  const int64_t now_s = util::GetTime();
  {
    std::lock_guard<std::mutex> g(addr_mutex_);
    auto& learned = learned_addrs_by_peer_[peer_id];

    // Prune old entries by TTL
    for (auto it = learned.begin(); it != learned.end(); ) {
      const int64_t age = now_s - it->second.last_seen_s;
      if (age > ECHO_SUPPRESS_TTL_SEC) {
        it = learned.erase(it);
      } else {
        ++it;
      }
    }

    // Insert/update learned entries and record recent ring
    for (const auto& ta : msg->addresses) {
      AddressKey k = MakeKey(ta.address);
      auto& e = learned[k];
      if (e.last_seen_s == 0 || ta.timestamp >= e.ts_addr.timestamp) {
        e.ts_addr = ta; // preserve services + latest timestamp
      }
      e.last_seen_s = now_s;

      // Global recent ring (O(1) eviction)
      recent_addrs_.push_back(ta);
      if (recent_addrs_.size() > RECENT_ADDRS_MAX) {
        recent_addrs_.pop_front();
      }
    }

    // Enforce per-peer cap by evicting oldest
    while (learned.size() > MAX_LEARNED_PER_PEER) {
      auto victim = learned.begin();
      for (auto it = learned.begin(); it != learned.end(); ++it) {
        if (it->second.last_seen_s < victim->second.last_seen_s) {
          victim = it;
        }
      }
      learned.erase(victim);
    }
  }
  return true;
}

bool MessageRouter::handle_getaddr(PeerPtr peer) {
  // Gate GETADDR on post-VERACK (Bitcoin Core parity) - check before other logic
  if (!peer || !peer->successfully_connected()) {
    LOG_NET_TRACE("Ignoring GETADDR from pre-VERACK peer");
    return true;
  }

  if (!addr_manager_) {
    return false;
  }

  {
    std::lock_guard<std::mutex> sg(stats_mutex_);
    stats_getaddr_total_++;
  }

  // Respond only to INBOUND peers (fingerprinting protection)
  // This asymmetric behavior prevents attackers from fingerprinting nodes by:
  // 1. Sending fake addresses to victim's AddrMan
  // 2. Later requesting GETADDR to check if those addresses are returned
  // Reference: Bitcoin Core net_processing.cpp ProcessMessage("getaddr")
  if (!peer->is_inbound()) {
    {
      std::lock_guard<std::mutex> sg(stats_mutex_);
      stats_getaddr_ignored_outbound_++;
    }
    LOG_NET_DEBUG("GETADDR ignored: outbound peer={} (inbound-only policy)", peer->id());
    return true; // Not an error; just ignore
  }

  const int peer_id = peer->id();
  const int64_t now_s = util::GetTime();

  // Once-per-connection gating (reply to GETADDR only once per connection)
  {
    std::lock_guard<std::mutex> g(addr_mutex_);
    if (getaddr_replied_.count(peer_id)) {
      {
        std::lock_guard<std::mutex> sg(stats_mutex_);
        stats_getaddr_ignored_repeat_++;
      }
      LOG_NET_DEBUG("GETADDR ignored: repeat on same connection peer={}", peer_id);
      return true;
    }
    getaddr_replied_.insert(peer_id);
  }

  // Copy suppression map for this peer while pruning old entries (under lock)
  LearnedMap suppression_map_copy;
  {
    std::lock_guard<std::mutex> g(addr_mutex_);
    auto it = learned_addrs_by_peer_.find(peer_id);
    if (it != learned_addrs_by_peer_.end()) {
      // prune TTL in place before copying
      for (auto lit = it->second.begin(); lit != it->second.end(); ) {
        const int64_t age = now_s - lit->second.last_seen_s;
        if (age > ECHO_SUPPRESS_TTL_SEC) {
          lit = it->second.erase(lit);
        } else {
          ++lit;
        }
      }
      suppression_map_copy = it->second; // copy after pruning
    }
  }

  // Build response outside lock
  auto response = std::make_unique<message::AddrMessage>();

  // Requester's own address key (avoid reflecting it)
  AddressKey peer_self_key{};
  bool have_self = false;
  try {
    protocol::NetworkAddress peer_na = protocol::NetworkAddress::from_string(peer->address(), peer->port());
    peer_self_key = MakeKey(peer_na);
    have_self = true;
  } catch (...) {
    have_self = false;
  }

  // Suppression predicate
  auto is_suppressed = [&](const AddressKey& key)->bool {
    auto it = suppression_map_copy.find(key);
    if (it != suppression_map_copy.end()) {
      const int64_t age = now_s - it->second.last_seen_s;
      if (age >= 0 && age <= ECHO_SUPPRESS_TTL_SEC) return true;
    }
    if (have_self && key == peer_self_key) return true;
    return false;
  };

  std::unordered_set<AddressKey, AddressKey::Hasher> included;

  size_t c_from_recent = 0;
  size_t c_from_addrman = 0;
  size_t c_from_learned = 0;
  size_t c_suppressed = 0;

  // 1) Prefer recently learned addresses (most recent first)
  {
    std::lock_guard<std::mutex> g(addr_mutex_);
    for (auto it = recent_addrs_.rbegin(); it != recent_addrs_.rend(); ++it) {
      if (response->addresses.size() >= protocol::MAX_ADDR_SIZE) break;
      AddressKey k = MakeKey(it->address);
      if (is_suppressed(k)) { c_suppressed++; continue; }
      if (!included.insert(k).second) continue;
      response->addresses.push_back(*it);
      c_from_recent++;
    }
  }

  // 2) Top-up from AddrMan sample
  if (response->addresses.size() < protocol::MAX_ADDR_SIZE) {
    auto addrs = addr_manager_->get_addresses(protocol::MAX_ADDR_SIZE);
    for (const auto& ta : addrs) {
      if (response->addresses.size() >= protocol::MAX_ADDR_SIZE) break;
      AddressKey k = MakeKey(ta.address);
      if (is_suppressed(k)) { c_suppressed++; continue; }
      if (!included.insert(k).second) continue;
      response->addresses.push_back(ta);
      c_from_addrman++;
    }
  }

  // 3) Fallback: if still empty, include learned addresses from other peers (excluding requester)
  if (response->addresses.empty()) {
    std::lock_guard<std::mutex> g(addr_mutex_);
    for (const auto& [other_peer_id, learned_map] : learned_addrs_by_peer_) {
      if (other_peer_id == peer_id) continue;
      for (const auto& [akey, entry] : learned_map) {
        if (response->addresses.size() >= protocol::MAX_ADDR_SIZE) break;
        if (is_suppressed(akey)) { c_suppressed++; continue; }
        if (!included.insert(akey).second) continue;
        response->addresses.push_back(entry.ts_addr);
        c_from_learned++;
      }
      if (response->addresses.size() >= protocol::MAX_ADDR_SIZE) break;
    }
  }

  // Save composition stats and log
  {
    std::lock_guard<std::mutex> sg(stats_mutex_);
    last_resp_from_addrman_ = c_from_addrman;
    last_resp_from_recent_ = c_from_recent;
    last_resp_from_learned_ = c_from_learned;
    last_resp_suppressed_ = c_suppressed;
  }
  LOG_NET_DEBUG("GETADDR served peer={} addrs_total={} from_recent={} from_addrman={} from_learned={} suppressed={}",
                peer_id, response->addresses.size(), c_from_recent, c_from_addrman, c_from_learned, c_suppressed);

  // Verify peer still connected before sending (TOCTOU protection)
  if (!peer->is_connected()) {
    LOG_NET_TRACE("Peer {} disconnected before GETADDR response could be sent", peer_id);
    return true; // Not an error, just too late
  }

  // Privacy: randomize order to avoid recency leaks
  if (!response->addresses.empty()) {
    std::shuffle(response->addresses.begin(), response->addresses.end(), rng_);
  }

  peer->send_message(std::move(response));
  {
    std::lock_guard<std::mutex> sg(stats_mutex_);
    stats_getaddr_served_++;
  }
  return true;
}

bool MessageRouter::handle_inv(PeerPtr peer, message::InvMessage* msg) {
  if (!msg) {
    return false;
  }

  // Gate INV on post-VERACK (Bitcoin Core parity) - check before null manager
  if (!peer || !peer->successfully_connected()) {
    LOG_NET_TRACE("Ignoring INV from pre-VERACK peer");
    return true;
  }

  if (!block_relay_manager_) {
    return false;
  }

  return block_relay_manager_->HandleInvMessage(peer, msg);
}

bool MessageRouter::handle_headers(PeerPtr peer, message::HeadersMessage* msg) {
  if (!msg) {
    return false;
  }

  // Gate HEADERS on post-VERACK (Bitcoin Core parity) - check before null manager
  if (!peer || !peer->successfully_connected()) {
    LOG_NET_TRACE("Ignoring HEADERS from pre-VERACK peer");
    return true;
  }

  if (!header_sync_manager_) {
    return false;
  }

  return header_sync_manager_->HandleHeadersMessage(peer, msg);
}

bool MessageRouter::handle_getheaders(PeerPtr peer, message::GetHeadersMessage* msg) {
  if (!msg) {
    return false;
  }

  // Gate GETHEADERS on post-VERACK (Bitcoin Core parity) - check before null manager
  if (!peer || !peer->successfully_connected()) {
    LOG_NET_TRACE("Ignoring GETHEADERS from pre-VERACK peer");
    return true;
  }

  if (!header_sync_manager_) {
    return false;
  }

  return header_sync_manager_->HandleGetHeadersMessage(peer, msg);
}

// Helper to build binary key
MessageRouter::AddressKey MessageRouter::MakeKey(const protocol::NetworkAddress& a) {
  AddressKey k; k.ip = a.ip; k.port = a.port; return k;
}

void MessageRouter::OnPeerDisconnected(int peer_id) {
  std::lock_guard<std::mutex> g(addr_mutex_);
  getaddr_replied_.erase(peer_id);
  learned_addrs_by_peer_.erase(peer_id);
}

MessageRouter::GetAddrDebugStats MessageRouter::GetGetAddrDebugStats() const {
  std::lock_guard<std::mutex> sg(stats_mutex_);
  GetAddrDebugStats s;
  s.total = stats_getaddr_total_;
  s.served = stats_getaddr_served_;
  s.ignored_outbound = stats_getaddr_ignored_outbound_;
  s.ignored_prehandshake = stats_getaddr_ignored_prehandshake_;
  s.ignored_repeat = stats_getaddr_ignored_repeat_;
  s.last_from_addrman = last_resp_from_addrman_;
  s.last_from_recent = last_resp_from_recent_;
  s.last_from_learned = last_resp_from_learned_;
  s.last_suppressed = last_resp_suppressed_;
  return s;
}

void MessageRouter::TestSeedRng(uint64_t seed) {
  rng_.seed(seed);
}

} // namespace network
} // namespace coinbasechain
