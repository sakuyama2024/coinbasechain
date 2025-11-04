#include "network/peer_discovery_manager.hpp"
#include "network/addr_manager.hpp"
#include "network/anchor_manager.hpp"
#include "network/peer_lifecycle_manager.hpp"
#include "network/protocol.hpp"
#include "network/notifications.hpp"
#include "chain/chainparams.hpp"
#include "util/logging.hpp"
#include "util/time.hpp"
#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v6.hpp>

namespace coinbasechain {
namespace network {

PeerDiscoveryManager::PeerDiscoveryManager(PeerLifecycleManager* peer_manager, const std::string& datadir)
    : datadir_(datadir),
      peer_manager_(peer_manager),
      rng_(std::random_device{}()) {
  // Create and own AddressManager
  addr_manager_ = std::make_unique<AddressManager>();
  LOG_NET_INFO("PeerDiscoveryManager created AddressManager");

  // Create and own AnchorManager
  if (!peer_manager) {
    LOG_NET_ERROR("PeerDiscoveryManager: peer_manager is null, cannot create AnchorManager");
  } else {
    anchor_manager_ = std::make_unique<AnchorManager>(*peer_manager);
    LOG_NET_INFO("PeerDiscoveryManager created AnchorManager");

    // Inject self into PeerLifecycleManager for address lifecycle tracking
    peer_manager->SetDiscoveryManager(this);
  }

  // Subscribe to NetworkNotifications for address lifecycle management
  // Filter notifications to only process events from our associated PeerLifecycleManager
  peer_connected_sub_ = NetworkEvents().SubscribePeerConnected(
      [this](int peer_id, const std::string& address, uint16_t port, const std::string& connection_type) {
        // Only process if this peer belongs to our PeerLifecycleManager instance
        if (!peer_manager_ || !peer_manager_->get_peer(peer_id)) {
          return; // Not our peer, ignore
        }

        // Only process outbound connections (not feelers, not inbound)
        if (connection_type == "outbound") {
          try {
            protocol::NetworkAddress net_addr = protocol::NetworkAddress::from_string(address, port);
            // Add to address manager first if not already there
            addr_manager_->add(net_addr);
            // Mark as good (moves to "tried" table)
            addr_manager_->good(net_addr);
            LOG_NET_DEBUG("PeerDiscoveryManager: marked outbound peer {}:{} as good in address manager",
                          address, port);
          } catch (const std::exception& e) {
            LOG_NET_WARN("PeerDiscoveryManager: failed to update address manager for connected peer {}:{}: {}",
                         address, port, e.what());
          }
        }
      });

  // Subscribe to PeerDisconnected to mark addresses as good when appropriate
  // PeerLifecycleManager calculates the mark_addr_good flag based on peer state
  peer_disconnected_sub_ = NetworkEvents().SubscribePeerDisconnected(
      [this](int peer_id, const std::string& address, uint16_t port,
             const std::string& reason, bool mark_addr_good) {
        // Only process if this peer belongs to our PeerLifecycleManager instance
        if (!peer_manager_ || !peer_manager_->get_peer(peer_id)) {
          return; // Not our peer, ignore
        }

        // Mark address as good if flagged by PeerLifecycleManager
        if (mark_addr_good && port != 0 && !address.empty()) {
          try {
            protocol::NetworkAddress net_addr = protocol::NetworkAddress::from_string(address, port);
            addr_manager_->good(net_addr);
            LOG_NET_TRACE("PeerDiscoveryManager: marked disconnected peer {}:{} as good in address manager",
                          address, port);
          } catch (const std::exception& e) {
            LOG_NET_WARN("PeerDiscoveryManager: failed to update address manager for disconnected peer {}:{}: {}",
                         address, port, e.what());
          }
        }
      });

  LOG_NET_INFO("PeerDiscoveryManager initialized with NetworkNotifications subscriptions");
}

PeerDiscoveryManager::~PeerDiscoveryManager() {
  LOG_NET_INFO("PeerDiscoveryManager destroyed");
}

void PeerDiscoveryManager::Start(ConnectToAnchorsCallback connect_anchors) {
  // Load and connect to anchor peers (eclipse attack resistance)
  // Anchors are the last 2-3 outbound peers we connected to before shutdown
  if (!datadir_.empty()) {
    std::string anchors_path = datadir_ + "/anchors.json";
    if (std::filesystem::exists(anchors_path)) {
      auto anchor_addrs = LoadAnchors(anchors_path);
      if (!anchor_addrs.empty()) {
        LOG_NET_TRACE("Loaded {} anchors, connecting to them first", anchor_addrs.size());
        connect_anchors(anchor_addrs);
      } else {
        LOG_NET_DEBUG("No anchors loaded from {}", anchors_path);
      }
    }
  }

  // Bootstrap from fixed seeds if AddressManager is empty
  // (matches Bitcoin's ThreadDNSAddressSeed logic: query all seeds if addrman.Size() == 0)
  if (Size() == 0) {
    BootstrapFromFixedSeeds(chain::GlobalChainParams::Get());
  }
}

bool PeerDiscoveryManager::HandleAddr(PeerPtr peer, message::AddrMessage* msg) {
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

  // Update learned addresses via ConnectionManager
  if (peer_manager_) {
    peer_manager_->ModifyLearnedAddresses(peer_id, [&](LearnedMap& learned) {
      // Prune old entries by TTL
      for (auto it = learned.begin(); it != learned.end(); ) {
        const int64_t age = now_s - it->second.last_seen_s;
        if (age > ECHO_SUPPRESS_TTL_SEC) {
          it = learned.erase(it);
        } else {
          ++it;
        }
      }

      // Insert/update learned entries
      for (const auto& ta : msg->addresses) {
        network::AddressKey k = MakeKey(ta.address);
        auto& e = learned[k];
        if (e.last_seen_s == 0 || ta.timestamp >= e.ts_addr.timestamp) {
          e.ts_addr = ta; // preserve services + latest timestamp
        }
        e.last_seen_s = now_s;
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
    });
  }

  // Global recent ring (O(1) eviction)
  for (const auto& ta : msg->addresses) {
    recent_addrs_.push_back(ta);
    if (recent_addrs_.size() > RECENT_ADDRS_MAX) {
      recent_addrs_.pop_front();
    }
  }

  return true;
}

bool PeerDiscoveryManager::HandleGetAddr(PeerPtr peer) {
  // Gate GETADDR on post-VERACK (Bitcoin Core parity) - check before other logic
  if (!peer || !peer->successfully_connected()) {
    LOG_NET_TRACE("Ignoring GETADDR from pre-VERACK peer");
    return true;
  }

  if (!addr_manager_) {
    return false;
  }

  stats_getaddr_total_++;

  // Respond only to INBOUND peers (fingerprinting protection)
  // This asymmetric behavior prevents attackers from fingerprinting nodes by:
  // 1. Sending fake addresses to victim's AddrMan
  // 2. Later requesting GETADDR to check if those addresses are returned
  // Reference: Bitcoin Core net_processing.cpp ProcessMessage("getaddr")
  if (!peer->is_inbound()) {
    stats_getaddr_ignored_outbound_++;
    LOG_NET_DEBUG("GETADDR ignored: outbound peer={} (inbound-only policy)", peer->id());
    return true; // Not an error; just ignore
  }

  const int peer_id = peer->id();
  const int64_t now_s = util::GetTime();

  // Once-per-connection gating (reply to GETADDR only once per connection)
  // Use ConnectionManager accessor
  if (peer_manager_ && peer_manager_->HasRepliedToGetAddr(peer_id)) {
    stats_getaddr_ignored_repeat_++;
    LOG_NET_DEBUG("GETADDR ignored: repeat on same connection peer={}", peer_id);
    return true;
  }
  if (peer_manager_) {
    peer_manager_->MarkGetAddrReplied(peer_id);
  }

  // Copy suppression map for this peer while pruning old entries
  // Use ConnectionManager accessor
  LearnedMap suppression_map_copy;
  if (peer_manager_) {
    auto learned_opt = peer_manager_->GetLearnedAddresses(peer_id);
    if (learned_opt) {
      // Prune TTL before copying
      for (auto it = learned_opt->begin(); it != learned_opt->end(); ) {
        const int64_t age = now_s - it->second.last_seen_s;
        if (age > ECHO_SUPPRESS_TTL_SEC) {
          it = learned_opt->erase(it);
        } else {
          ++it;
        }
      }
      suppression_map_copy = *learned_opt; // copy after pruning
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
  for (auto it = recent_addrs_.rbegin(); it != recent_addrs_.rend(); ++it) {
    if (response->addresses.size() >= protocol::MAX_ADDR_SIZE) break;
    AddressKey k = MakeKey(it->address);
    if (is_suppressed(k)) { c_suppressed++; continue; }
    if (!included.insert(k).second) continue;
    response->addresses.push_back(*it);
    c_from_recent++;
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
  // Use ConnectionManager accessor
  if (response->addresses.empty() && peer_manager_) {
    auto all_learned = peer_manager_->GetAllLearnedAddresses();
    for (const auto& [other_peer_id, learned_map] : all_learned) {
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
  last_resp_from_addrman_ = c_from_addrman;
  last_resp_from_recent_ = c_from_recent;
  last_resp_from_learned_ = c_from_learned;
  last_resp_suppressed_ = c_suppressed;
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
  stats_getaddr_served_++;
  return true;
}

PeerDiscoveryManager::GetAddrDebugStats PeerDiscoveryManager::GetGetAddrDebugStats() const {
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

void PeerDiscoveryManager::TestSeedRng(uint64_t seed) {
  rng_.seed(seed);
}

// Helper to build binary key (uses shared AddressKey from peer_state.hpp)
network::AddressKey PeerDiscoveryManager::MakeKey(const protocol::NetworkAddress& a) {
  network::AddressKey k; k.ip = a.ip; k.port = a.port; return k;
}

// === Bootstrap and Discovery Methods ===

void PeerDiscoveryManager::BootstrapFromFixedSeeds(const chain::ChainParams& params) {
  // Bootstrap AddressManager from hardcoded seed nodes
  // (follows Bitcoin's ThreadDNSAddressSeed logic when addrman is empty)

  const auto& fixed_seeds = params.FixedSeeds();

  if (fixed_seeds.empty()) {
    LOG_NET_TRACE("no fixed seeds available for bootstrap");
    return;
  }

  LOG_NET_INFO("Bootstrapping from {} fixed seed nodes", fixed_seeds.size());

  // Use AddressManager's time format (seconds since epoch)
  // Use util::GetTime() for consistency and testability (supports mock time)
  uint32_t current_time = static_cast<uint32_t>(util::GetTime());
  size_t added_count = 0;

  // Parse each "IP:port" string and add to AddressManager
  for (const auto& seed_str : fixed_seeds) {
    // Parse IP:port format (e.g., "178.18.251.16:9590")
    size_t colon_pos = seed_str.find(':');
    if (colon_pos == std::string::npos) {
      LOG_NET_WARN("Invalid seed format (missing port): {}", seed_str);
      continue;
    }

    std::string ip_str = seed_str.substr(0, colon_pos);
    std::string port_str = seed_str.substr(colon_pos + 1);

    // Parse port
    uint16_t port = 0;
    try {
      int port_int = std::stoi(port_str);
      if (port_int <= 0 || port_int > 65535) {
        LOG_NET_WARN("Invalid port in seed: {}", seed_str);
        continue;
      }
      port = static_cast<uint16_t>(port_int);
    } catch (const std::exception& e) {
      LOG_NET_WARN("Failed to parse port in seed {}: {}", seed_str, e.what());
      continue;
    }

    // Parse IP address using boost::asio
    try {
      boost::system::error_code ec;
      auto ip_addr = boost::asio::ip::make_address(ip_str, ec);

      if (ec) {
        LOG_NET_WARN("Failed to parse IP in seed {}: {}", seed_str, ec.message());
        continue;
      }

      // Create NetworkAddress
      protocol::NetworkAddress addr;
      addr.services = protocol::ServiceFlags::NODE_NETWORK;
      addr.port = port;

      // Convert to 16-byte IPv6 format (IPv4-mapped if needed)
      if (ip_addr.is_v4()) {
        // Convert IPv4 to IPv4-mapped IPv6 (::FFFF:x.x.x.x)
        auto v6_mapped = boost::asio::ip::make_address_v6(
            boost::asio::ip::v4_mapped, ip_addr.to_v4());
        auto bytes = v6_mapped.to_bytes();
        std::copy(bytes.begin(), bytes.end(), addr.ip.begin());
      } else {
        // Pure IPv6
        auto bytes = ip_addr.to_v6().to_bytes();
        std::copy(bytes.begin(), bytes.end(), addr.ip.begin());
      }

      // Add to AddressManager with current timestamp
      if (addr_manager_->add(addr, current_time)) {
        added_count++;
        LOG_NET_DEBUG("Added seed node: {}", seed_str);
      }

    } catch (const std::exception& e) {
      LOG_NET_WARN("Exception parsing seed {}: {}", seed_str, e.what());
      continue;
    }
  }

  LOG_NET_INFO("Successfully added {} seed nodes to AddressManager", added_count);
}

// === AddressManager Forwarding Methods ===

bool PeerDiscoveryManager::Add(const protocol::NetworkAddress& addr, uint32_t timestamp) {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::Add: addr_manager_ is null");
    return false;
  }
  return addr_manager_->add(addr, timestamp);
}

size_t PeerDiscoveryManager::AddMultiple(const std::vector<protocol::TimestampedAddress>& addresses) {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::AddMultiple: addr_manager_ is null");
    return 0;
  }
  return addr_manager_->add_multiple(addresses);
}

void PeerDiscoveryManager::Attempt(const protocol::NetworkAddress& addr) {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::Attempt: addr_manager_ is null");
    return;
  }
  addr_manager_->attempt(addr);
}

void PeerDiscoveryManager::Good(const protocol::NetworkAddress& addr) {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::Good: addr_manager_ is null");
    return;
  }
  addr_manager_->good(addr);
}

void PeerDiscoveryManager::Failed(const protocol::NetworkAddress& addr) {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::Failed: addr_manager_ is null");
    return;
  }
  addr_manager_->failed(addr);
}

std::optional<protocol::NetworkAddress> PeerDiscoveryManager::Select() {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::Select: addr_manager_ is null");
    return std::nullopt;
  }
  return addr_manager_->select();
}

std::optional<protocol::NetworkAddress> PeerDiscoveryManager::SelectNewForFeeler() {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::SelectNewForFeeler: addr_manager_ is null");
    return std::nullopt;
  }
  return addr_manager_->select_new_for_feeler();
}

std::vector<protocol::TimestampedAddress> PeerDiscoveryManager::GetAddresses(size_t max_count) {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::GetAddresses: addr_manager_ is null");
    return {};
  }
  return addr_manager_->get_addresses(max_count);
}

size_t PeerDiscoveryManager::Size() const {
  if (!addr_manager_) {
    return 0;
  }
  return addr_manager_->size();
}

size_t PeerDiscoveryManager::TriedCount() const {
  if (!addr_manager_) {
    return 0;
  }
  return addr_manager_->tried_count();
}

size_t PeerDiscoveryManager::NewCount() const {
  if (!addr_manager_) {
    return 0;
  }
  return addr_manager_->new_count();
}

void PeerDiscoveryManager::CleanupStale() {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::CleanupStale: addr_manager_ is null");
    return;
  }
  addr_manager_->cleanup_stale();
}

bool PeerDiscoveryManager::SaveAddresses(const std::string& filepath) {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::SaveAddresses: addr_manager_ is null");
    return false;
  }
  return addr_manager_->Save(filepath);
}

bool PeerDiscoveryManager::LoadAddresses(const std::string& filepath) {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::LoadAddresses: addr_manager_ is null");
    return false;
  }
  return addr_manager_->Load(filepath);
}

// === AnchorManager Forwarding Methods ===

std::vector<protocol::NetworkAddress> PeerDiscoveryManager::GetAnchors() const {
  if (!anchor_manager_) {
    LOG_NET_WARN("PeerDiscoveryManager::GetAnchors: anchor_manager_ is null");
    return {};
  }
  return anchor_manager_->GetAnchors();
}

bool PeerDiscoveryManager::SaveAnchors(const std::string& filepath) {
  if (!anchor_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::SaveAnchors: anchor_manager_ is null");
    return false;
  }
  return anchor_manager_->SaveAnchors(filepath);
}

std::vector<protocol::NetworkAddress> PeerDiscoveryManager::LoadAnchors(const std::string& filepath) {
  if (!anchor_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::LoadAnchors: anchor_manager_ is null");
    return {};
  }
  return anchor_manager_->LoadAnchors(filepath);
}

} // namespace network
} // namespace coinbasechain
