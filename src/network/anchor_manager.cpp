#include "network/anchor_manager.hpp"
#include "network/peer_lifecycle_manager.hpp"
#include "network/peer.hpp"
#include "util/logging.hpp"
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <random>
#include <chrono>
#include <limits>
#include <fcntl.h>
#include <unistd.h>

namespace coinbasechain {
namespace network {

AnchorManager::AnchorManager(PeerLifecycleManager& peer_mgr)
    : peer_manager_(peer_mgr) {}

std::vector<protocol::NetworkAddress> AnchorManager::GetAnchors() const {
  std::vector<protocol::NetworkAddress> anchors;

  // Get all outbound peers
  auto outbound_peers = peer_manager_.get_outbound_peers();

  struct Candidate {
    PeerPtr peer;
    protocol::NetworkAddress addr;
    int64_t age_s{0};
    int64_t ping_ms{std::numeric_limits<int64_t>::max()};
  };

  std::vector<Candidate> candidates;
  const auto now = std::chrono::steady_clock::now();

  for (const auto &peer : outbound_peers) {
    if (!peer) continue;
    if (!peer->is_connected() || peer->state() != PeerState::READY) continue;
    if (peer->is_feeler()) continue; // never anchor a feeler

    protocol::NetworkAddress addr;
    addr.services = peer->services();
    addr.port = peer->port();

    // Convert IP address string to bytes using boost::asio
    std::string ip_str = peer->address();
    try {
      boost::system::error_code ec;
      auto ip_addr = boost::asio::ip::make_address(ip_str, ec);
      if (ec) {
        LOG_NET_WARN("Failed to parse IP address '{}': {}", ip_str, ec.message());
        continue;
      }

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

      Candidate c;
      c.peer = peer;
      c.addr = addr;
      auto age = std::chrono::duration_cast<std::chrono::seconds>(now - peer->stats().connected_time).count();
      c.age_s = std::max<int64_t>(0, age);
      c.ping_ms = (peer->stats().ping_time_ms >= 0) ? peer->stats().ping_time_ms : std::numeric_limits<int64_t>::max();
      candidates.push_back(std::move(c));
    } catch (const std::exception &e) {
      LOG_NET_WARN("Exception parsing IP address '{}': {}", ip_str, e.what());
      continue;
    }
  }

  if (candidates.empty()) {
    LOG_NET_INFO("Selected 0 anchor peers");
    return anchors;
  }

  // Randomize then favor older connections and lower ping
  std::mt19937 rng{std::random_device{}()};
  std::shuffle(candidates.begin(), candidates.end(), rng);
  std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b){
    if (a.age_s != b.age_s) return a.age_s > b.age_s; // older first
    return a.ping_ms < b.ping_ms; // lower ping first
  });

  // Take top 2
  const size_t MAX_ANCHORS = 2;
  const size_t count = std::min(candidates.size(), MAX_ANCHORS);
  for (size_t i = 0; i < count; ++i) {
    anchors.push_back(candidates[i].addr);
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

    // Write atomically via temp file then rename (fsync for durability)
    const std::string tmp = filepath + ".tmp";
    std::string data = root.dump(2);
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      LOG_NET_ERROR("Failed to open anchors temp file for writing: {}", tmp);
      return false;
    }
    size_t total = 0;
    while (total < data.size()) {
      ssize_t n = ::write(fd, data.data() + total, data.size() - total);
      if (n <= 0) {
        LOG_NET_ERROR("Failed to write anchors temp file: {}", tmp);
        ::close(fd);
        std::error_code ec_remove;
        std::filesystem::remove(tmp, ec_remove);
        if (ec_remove) {
          LOG_NET_ERROR("Failed to remove anchors temp file {}: {}", tmp, ec_remove.message());
        }
        return false;
      }
      total += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) {
      LOG_NET_ERROR("fsync failed for anchors temp file: {}", tmp);
      ::close(fd);
      std::error_code ec_remove;
      std::filesystem::remove(tmp, ec_remove);
      if (ec_remove) {
        LOG_NET_ERROR("Failed to remove anchors temp file {} after fsync failure: {}", tmp, ec_remove.message());
      }
      return false;
    }
    ::close(fd);

    std::error_code ec;
    std::filesystem::rename(tmp, filepath, ec);
    if (ec) {
      // On cross-device or permission issues, try replace by remove+rename
      std::filesystem::remove(filepath, ec);
      std::filesystem::rename(tmp, filepath, ec);
      if (ec) {
        LOG_NET_ERROR("Failed to atomically replace anchors file: {} -> {}: {}", tmp, filepath, ec.message());
        // Cleanup temp best-effort
        std::error_code ec_remove;
        std::filesystem::remove(tmp, ec_remove);
        if (ec_remove) {
          LOG_NET_ERROR("Failed to remove anchors temp file {} after rename failure: {}", tmp, ec_remove.message());
        }
        return false;
      }
    }

    LOG_NET_DEBUG("Successfully saved {} anchors (atomic)", anchors.size());
    return true;

  } catch (const std::exception &e) {
    LOG_NET_ERROR("Exception during SaveAnchors: {}", e.what());
    return false;
  }
}

std::vector<protocol::NetworkAddress> AnchorManager::LoadAnchors(const std::string &filepath) {
  using json = nlohmann::json;

  try {
    // Check if file exists
    std::ifstream file(filepath);
    if (!file.is_open()) {
      LOG_NET_DEBUG("No anchors file found at {}", filepath);
      return {}; // No anchors to load
    }

    // Parse JSON
    json root;
    try {
      file >> root;
    } catch (const std::exception &e) {
      LOG_NET_WARN("Failed to parse anchors file {}: {}", filepath, e.what());
      file.close();
      std::filesystem::remove(filepath);
      return {};
    }
    file.close();

    // Validate version and structure
    const int version = root.value("version", 0);
    if (version != 1 || !root.contains("anchors") || !root["anchors"].is_array()) {
      LOG_NET_WARN("Invalid anchors file format/version, deleting {}", filepath);
      std::filesystem::remove(filepath);
      return {};
    }

    const json &anchors_array = root["anchors"];
    std::vector<protocol::NetworkAddress> anchors;
    anchors.reserve(std::min<size_t>(2, anchors_array.size()));

    auto valid_ip_array = [](const json &ip) {
      if (!ip.is_array() || ip.size() != 16) return false;
      for (const auto &v : ip) {
        if (!v.is_number_integer()) return false;
        int x = v.get<int>();
        if (x < 0 || x > 255) return false;
      }
      return true;
    };

    for (const auto &anchor_json : anchors_array) {
      if (!anchor_json.is_object()) {
        LOG_NET_WARN("Skipping malformed anchor (not object)");
        continue;
      }

      if (!anchor_json.contains("ip") || !anchor_json.contains("port") || !anchor_json.contains("services")) {
        LOG_NET_WARN("Skipping malformed anchor (missing fields)");
        continue;
      }

      const auto &ip_array = anchor_json["ip"];
      if (!valid_ip_array(ip_array)) {
        LOG_NET_WARN("Skipping anchor with invalid IP array");
        continue;
      }

      if (!anchor_json["port"].is_number_unsigned() || !anchor_json["services"].is_number_unsigned()) {
        LOG_NET_WARN("Skipping anchor with invalid port/services types");
        continue;
      }

      protocol::NetworkAddress addr;
      for (size_t i = 0; i < 16; ++i) {
        addr.ip[i] = static_cast<uint8_t>(ip_array[i].get<int>());
      }
      addr.port = anchor_json["port"].get<uint16_t>();
      addr.services = anchor_json["services"].get<uint64_t>();

      anchors.push_back(addr);
      if (anchors.size() == 2) break; // cap attempts
    }

    LOG_NET_INFO("Loaded {} anchor addresses from {} (passive - caller will connect)", anchors.size(), filepath);

    // Single-use file: delete after reading
    std::error_code ec;
    std::filesystem::remove(filepath, ec);
    if (ec) {
      LOG_NET_WARN("Failed to delete anchors file {}: {}", filepath, ec.message());
    } else {
      LOG_NET_DEBUG("Deleted anchors file after reading");
    }

    return anchors;

  } catch (const std::exception &e) {
    LOG_NET_ERROR("Exception during LoadAnchors: {}", e.what());
    try { std::filesystem::remove(filepath); } catch (...) {}
    return {};
  }
}

} // namespace network
} // namespace coinbasechain
