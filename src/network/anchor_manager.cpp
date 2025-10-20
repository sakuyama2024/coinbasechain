#include "network/anchor_manager.hpp"
#include "network/peer_manager.hpp"
#include "network/peer.hpp"
#include "chain/logging.hpp"
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace coinbasechain {
namespace network {

AnchorManager::AnchorManager(PeerManager& peer_mgr,
                             AddressToStringCallback addr_to_str_cb,
                             ConnectCallback connect_cb)
    : peer_manager_(peer_mgr),
      addr_to_string_callback_(std::move(addr_to_str_cb)),
      connect_callback_(std::move(connect_cb)) {}

std::vector<protocol::NetworkAddress> AnchorManager::GetAnchors() const {
  std::vector<protocol::NetworkAddress> anchors;

  // Get all connected outbound peers
  auto outbound_peers = peer_manager_.get_outbound_peers();

  // Filter for connected peers only
  std::vector<PeerPtr> connected_peers;
  std::copy_if(outbound_peers.begin(), outbound_peers.end(),
               std::back_inserter(connected_peers), [](const auto &peer) {
                 return peer && peer->is_connected() &&
                        peer->state() == PeerState::READY;
               });

  // Limit to 2 anchors
  const size_t MAX_ANCHORS = 2;
  size_t count = std::min(connected_peers.size(), MAX_ANCHORS);

  // Convert peer information to NetworkAddress
  for (size_t i = 0; i < count; ++i) {
    const auto &peer = connected_peers[i];

    protocol::NetworkAddress addr;
    addr.services = peer->services();
    addr.port = peer->port();

    // Convert IP address string to bytes using boost::asio
    std::string ip_str = peer->address();
    try {
      boost::system::error_code ec;
      auto ip_addr = boost::asio::ip::make_address(ip_str, ec);

      if (ec) {
        LOG_NET_WARN("Failed to parse IP address '{}': {}", ip_str,
                     ec.message());
        continue;
      }

      // Convert to 16-byte format (IPv4-mapped if needed)
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

      anchors.push_back(addr);
    } catch (const std::exception &e) {
      LOG_NET_WARN("Exception parsing IP address '{}': {}", ip_str, e.what());
      continue;
    }
  }

  LOG_NET_INFO("Selected {} anchor peers", anchors.size());
  return anchors;
}

bool AnchorManager::SaveAnchors(const std::string &filepath) {
  using json = nlohmann::json;

  try {
    auto anchors = GetAnchors();

    if (anchors.empty()) {
      LOG_NET_DEBUG("No anchors to save");
      return true; // Not an error
    }

    LOG_NET_INFO("Saving {} anchor addresses to {}", anchors.size(), filepath);

    json root;
    root["version"] = 1;
    root["count"] = anchors.size();

    json anchors_array = json::array();
    for (const auto &addr : anchors) {
      json anchor;
      anchor["ip"] = json::array();
      for (size_t i = 0; i < 16; ++i) {
        anchor["ip"].push_back(addr.ip[i]);
      }
      anchor["port"] = addr.port;
      anchor["services"] = addr.services;
      anchors_array.push_back(anchor);
    }
    root["anchors"] = anchors_array;

    // Write to file
    std::ofstream file(filepath);
    if (!file.is_open()) {
      LOG_NET_ERROR("Failed to open anchors file for writing: {}", filepath);
      return false;
    }

    file << root.dump(2);
    file.close();

    LOG_NET_DEBUG("Successfully saved {} anchors", anchors.size());
    return true;

  } catch (const std::exception &e) {
    LOG_NET_ERROR("Exception during SaveAnchors: {}", e.what());
    return false;
  }
}

bool AnchorManager::LoadAnchors(const std::string &filepath) {
  using json = nlohmann::json;

  try {
    // Check if file exists
    std::ifstream file(filepath);
    if (!file.is_open()) {
      LOG_NET_DEBUG("No anchors file found at {}", filepath);
      return true; // Not an error - first run
    }

    // Parse JSON
    json root;
    file >> root;
    file.close();

    // Validate version
    if (root["version"] != 1) {
      LOG_NET_WARN("Unsupported anchors file version: {}",
                   root["version"].get<int>());

      // Delete the file since it's incompatible
      std::filesystem::remove(filepath);
      return false;
    }

    // Load anchors
    json anchors_array = root["anchors"];
    std::vector<protocol::NetworkAddress> anchors;

    for (const auto &anchor_json : anchors_array) {
      protocol::NetworkAddress addr;

      json ip_array = anchor_json["ip"];
      for (size_t i = 0; i < 16 && i < ip_array.size(); ++i) {
        addr.ip[i] = ip_array[i].get<uint8_t>();
      }

      addr.port = anchor_json["port"].get<uint16_t>();
      addr.services = anchor_json["services"].get<uint64_t>();

      anchors.push_back(addr);
    }

    LOG_NET_INFO("Loaded {} anchor addresses from {}", anchors.size(),
                 filepath);

    // Try to reconnect to anchors using the callback
    for (const auto &addr : anchors) {
      // Convert NetworkAddress to IP string for logging
      auto maybe_ip_str = addr_to_string_callback_(addr);
      if (!maybe_ip_str) {
        LOG_NET_WARN("Failed to convert anchor address to string, skipping");
        continue;
      }

      const std::string &ip_str = *maybe_ip_str;
      LOG_NET_INFO("Reconnecting to anchor: {}:{}", ip_str, addr.port);

      // Attempt to connect (this will be async so we don't block startup)
      connect_callback_(addr);
    }

    // Delete the anchors file after reading
    // This prevents using stale anchors after a crash
    std::filesystem::remove(filepath);
    LOG_NET_DEBUG("Deleted anchors file after reading");

    return true;

  } catch (const std::exception &e) {
    LOG_NET_ERROR("Exception during LoadAnchors: {}", e.what());

    // Try to delete the file if it's corrupted
    try {
      std::filesystem::remove(filepath);
    } catch (...) {
      // Ignore
    }

    return false;
  }
}

} // namespace network
} // namespace coinbasechain
