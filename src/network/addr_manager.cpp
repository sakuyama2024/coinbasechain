#include "network/addr_manager.hpp"
#include "chain/logging.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>

namespace coinbasechain {
namespace network {

// Constants
static constexpr uint32_t STALE_AFTER_DAYS = 30;
static constexpr uint32_t MAX_FAILURES = 10;
static constexpr uint32_t SECONDS_PER_DAY = 86400;

// AddrInfo implementation

std::string AddrInfo::get_key() const {
  std::stringstream ss;

  // Convert IP bytes to string
  for (size_t i = 0; i < 16; i++) {
    ss << std::hex << std::setw(2) << std::setfill('0')
       << static_cast<int>(address.ip[i]);
  }
  ss << ":" << std::dec << address.port;

  return ss.str();
}

bool AddrInfo::is_stale(uint32_t now) const {
  return (now - timestamp) > (STALE_AFTER_DAYS * SECONDS_PER_DAY);
}

bool AddrInfo::is_terrible(uint32_t now) const {
  // Too many failed attempts
  if (attempts >= MAX_FAILURES) {
    return true;
  }

  // No success and too old
  if (!tried && is_stale(now)) {
    return true;
  }

  return false;
}

// AddressManager implementation

AddressManager::AddressManager() : rng_(std::random_device{}()) {}

uint32_t AddressManager::now() const {
  return static_cast<uint32_t>(
      std::chrono::system_clock::now().time_since_epoch().count() / 1000000000);
}

bool AddressManager::add(const protocol::NetworkAddress &addr,
                         uint32_t timestamp) {
  std::lock_guard<std::mutex> lock(mutex_);
  return add_internal(addr, timestamp);
}

bool AddressManager::add_internal(const protocol::NetworkAddress &addr,
                                  uint32_t timestamp) {
  AddrInfo info(addr, timestamp == 0 ? now() : timestamp);
  std::string key = info.get_key();

  // Check if already in tried table
  if (tried_.find(key) != tried_.end()) {
    // Update timestamp if newer
    if (timestamp > tried_[key].timestamp) {
      tried_[key].timestamp = timestamp;
    }
    return false; // Already have it
  }

  // Check if already in new table
  if (new_.find(key) != new_.end()) {
    // Update timestamp if newer
    if (timestamp > new_[key].timestamp) {
      new_[key].timestamp = timestamp;
    }
    return false; // Already have it
  }

  // Filter out terrible addresses
  if (info.is_terrible(now())) {
    return false;
  }

  // Add to new table
  new_[key] = info;
  return true;
}

size_t AddressManager::add_multiple(
    const std::vector<protocol::TimestampedAddress> &addresses) {
  std::lock_guard<std::mutex> lock(mutex_);

  size_t added = 0;
  for (const auto &ts_addr : addresses) {
    if (add_internal(ts_addr.address, ts_addr.timestamp)) {
      added++;
    }
  }

  return added;
}

void AddressManager::attempt(const protocol::NetworkAddress &addr) {
  std::lock_guard<std::mutex> lock(mutex_);

  AddrInfo info(addr);
  std::string key = info.get_key();

  // Update in new table
  auto it = new_.find(key);
  if (it != new_.end()) {
    it->second.last_try = now();
    it->second.attempts++;
  }
}

void AddressManager::good(const protocol::NetworkAddress &addr) {
  std::lock_guard<std::mutex> lock(mutex_);

  AddrInfo info(addr);
  std::string key = info.get_key();
  uint32_t current_time = now();

  LOG_NET_TRACE("AddressManager::good() called for address: {}", key);

  // Check if in new table
  auto new_it = new_.find(key);
  if (new_it != new_.end()) {
    // Move from new to tried
    LOG_NET_TRACE("Moving address {} from 'new' to 'tried' table", key);
    new_it->second.tried = true;
    new_it->second.last_success = current_time;
    new_it->second.attempts = 0; // Reset failure count

    tried_[key] = new_it->second;
    new_.erase(new_it);
    LOG_NET_TRACE("Address {} successfully moved to 'tried'. New size: {}, Tried size: {}",
                  key, new_.size(), tried_.size());
    return;
  }

  // Already in tried table
  auto tried_it = tried_.find(key);
  if (tried_it != tried_.end()) {
    LOG_NET_TRACE("Updating existing address {} in 'tried' table", key);
    tried_it->second.last_success = current_time;
    tried_it->second.attempts = 0; // Reset failure count
    return;
  }

  LOG_NET_WARN("AddressManager::good() called for unknown address: {}", key);
}

void AddressManager::failed(const protocol::NetworkAddress &addr) {
  std::lock_guard<std::mutex> lock(mutex_);

  AddrInfo info(addr);
  std::string key = info.get_key();

  // Update in new table
  auto new_it = new_.find(key);
  if (new_it != new_.end()) {
    new_it->second.attempts++;

    // Remove if too many failures
    if (new_it->second.is_terrible(now())) {
      new_.erase(new_it);
    }
    return;
  }

  // Update in tried table
  auto tried_it = tried_.find(key);
  if (tried_it != tried_.end()) {
    tried_it->second.attempts++;

    // Move back to new table if too many failures
    if (tried_it->second.attempts >= MAX_FAILURES) {
      tried_it->second.tried = false;
      new_[key] = tried_it->second;
      tried_.erase(tried_it);
    }
  }
}

std::optional<protocol::NetworkAddress> AddressManager::select() {
  std::lock_guard<std::mutex> lock(mutex_);

  // Prefer tried addresses (80% of the time)
  std::uniform_int_distribution<int> dist(0, 99);
  bool use_tried = !tried_.empty() && (dist(rng_) < 80 || new_.empty());

  const uint32_t now_ts = now();
  const uint32_t COOLDOWN_SEC = 600; // 10 minutes
  const int ATTEMPT_BYPASS = 30;     // After 30 attempts, allow sooner retries

  auto ok = [&](const AddrInfo& info) -> bool {
    if (info.last_try == 0) return true;
    if (now_ts - info.last_try >= COOLDOWN_SEC) return true;
    if (info.attempts >= ATTEMPT_BYPASS) return true;
    return false;
  };

  if (use_tried && !tried_.empty()) {
    std::uniform_int_distribution<size_t> idx_dist(0, tried_.size() - 1);
    const size_t max_checks = std::min<size_t>(tried_.size(), 64);
    for (size_t i = 0; i < max_checks; ++i) {
      auto it = tried_.begin();
      std::advance(it, idx_dist(rng_));
      if (ok(it->second)) {
        return it->second.address;
      }
    }
    auto it = tried_.begin();
    std::advance(it, idx_dist(rng_));
    return it->second.address;
  }

  if (!new_.empty()) {
    std::uniform_int_distribution<size_t> idx_dist(0, new_.size() - 1);
    const size_t max_checks = std::min<size_t>(new_.size(), 64);
    for (size_t i = 0; i < max_checks; ++i) {
      auto it = new_.begin();
      std::advance(it, idx_dist(rng_));
      if (ok(it->second)) {
        return it->second.address;
      }
    }
    auto it = new_.begin();
    std::advance(it, idx_dist(rng_));
    return it->second.address;
  }

  return std::nullopt;
}

std::optional<protocol::NetworkAddress>
AddressManager::select_new_for_feeler() {
  std::lock_guard<std::mutex> lock(mutex_);

  // FEELER connections test addresses from "new" table (never connected before)
  // This helps move working addresses from "new" to "tried"
  if (new_.empty()) {
    return std::nullopt;
  }

  // Select random address from "new" table only
  std::uniform_int_distribution<size_t> idx_dist(0, new_.size() - 1);
  auto it = new_.begin();
  std::advance(it, idx_dist(rng_));
  return it->second.address;
}

std::vector<protocol::TimestampedAddress>
AddressManager::get_addresses(size_t max_count) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<protocol::TimestampedAddress> result;
  result.reserve(std::min(max_count, tried_.size() + new_.size()));

  // Add tried addresses first
  for (const auto &[key, info] : tried_) {
    if (result.size() >= max_count)
      break;
    result.push_back({info.timestamp, info.address});
  }

  // Add new addresses
  for (const auto &[key, info] : new_) {
    if (result.size() >= max_count)
      break;
    result.push_back({info.timestamp, info.address});
  }

  // Shuffle for privacy
  std::shuffle(result.begin(), result.end(), rng_);

  return result;
}

size_t AddressManager::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tried_.size() + new_.size();
}

size_t AddressManager::tried_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tried_.size();
}

size_t AddressManager::new_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return new_.size();
}

void AddressManager::cleanup_stale() {
  std::lock_guard<std::mutex> lock(mutex_);
  uint32_t current_time = now();

  // Remove stale addresses from new table
  for (auto it = new_.begin(); it != new_.end();) {
    if (it->second.is_stale(current_time) ||
        it->second.is_terrible(current_time)) {
      it = new_.erase(it);
    } else {
      ++it;
    }
  }

  // Keep tried addresses even if old (they worked before)
}

bool AddressManager::Save(const std::string &filepath) {
  using json = nlohmann::json;

  std::lock_guard<std::mutex> lock(mutex_);

  try {
    // Calculate size without calling size() to avoid recursive lock
    size_t total_size = tried_.size() + new_.size();
    LOG_NET_TRACE("saving {} peer addresses to {}", total_size, filepath);

    json root;
    root["version"] = 1;
    root["tried_count"] = tried_.size();
    root["new_count"] = new_.size();

    // Save tried addresses
    json tried_array = json::array();
    for (const auto &[key, info] : tried_) {
      json addr;
      addr["ip"] = json::array();
      for (size_t i = 0; i < 16; ++i) {
        addr["ip"].push_back(info.address.ip[i]);
      }
      addr["port"] = info.address.port;
      addr["services"] = info.address.services;
      addr["timestamp"] = info.timestamp;
      addr["last_try"] = info.last_try;
      addr["last_success"] = info.last_success;
      addr["attempts"] = info.attempts;
      tried_array.push_back(addr);
    }
    root["tried"] = tried_array;

    // Save new addresses
    json new_array = json::array();
    for (const auto &[key, info] : new_) {
      json addr;
      addr["ip"] = json::array();
      for (size_t i = 0; i < 16; ++i) {
        addr["ip"].push_back(info.address.ip[i]);
      }
      addr["port"] = info.address.port;
      addr["services"] = info.address.services;
      addr["timestamp"] = info.timestamp;
      addr["last_try"] = info.last_try;
      addr["last_success"] = info.last_success;
      addr["attempts"] = info.attempts;
      new_array.push_back(addr);
    }
    root["new"] = new_array;

    // Write to file
    std::ofstream file(filepath);
    if (!file.is_open()) {
      LOG_NET_ERROR("Failed to open file for writing: {}", filepath);
      return false;
    }

    file << root.dump(2);
    file.close();

    LOG_NET_TRACE("successfully saved {} addresses ({} tried, {} new)",
                  total_size, tried_.size(), new_.size());
    return true;

  } catch (const std::exception &e) {
    LOG_NET_ERROR("Exception during Save: {}", e.what());
    return false;
  }
}

bool AddressManager::Load(const std::string &filepath) {
  using json = nlohmann::json;

  std::lock_guard<std::mutex> lock(mutex_);

  try {
    LOG_NET_TRACE("loading peer addresses from {}", filepath);

    // Open file
    std::ifstream file(filepath);
    if (!file.is_open()) {
      LOG_NET_TRACE("peer address file not found: {} (starting fresh)",
                   filepath);
      return false;
    }

    // Parse JSON
    json root;
    file >> root;
    file.close();

    // Validate version
    int version = root.value("version", 0);
    if (version != 1) {
      LOG_NET_ERROR("Unsupported peers file version: {}", version);
      return false;
    }

    // Clear existing data
    tried_.clear();
    new_.clear();

    // Load tried addresses
    if (root.contains("tried")) {
      for (const auto &addr_json : root["tried"]) {
        protocol::NetworkAddress addr;

        // Load IP
        if (addr_json["ip"].size() != 16) {
          LOG_NET_TRACE("invalid IP address in peers file, skipping");
          continue;
        }
        for (size_t i = 0; i < 16; ++i) {
          addr.ip[i] = addr_json["ip"][i].get<uint8_t>();
        }

        addr.port = addr_json["port"].get<uint16_t>();
        addr.services = addr_json["services"].get<uint64_t>();

        AddrInfo info(addr, addr_json["timestamp"].get<uint32_t>());
        info.last_try = addr_json["last_try"].get<uint32_t>();
        info.last_success = addr_json["last_success"].get<uint32_t>();
        info.attempts = addr_json["attempts"].get<int>();
        info.tried = true;

        tried_[info.get_key()] = info;
      }
    }

    // Load new addresses
    if (root.contains("new")) {
      for (const auto &addr_json : root["new"]) {
        protocol::NetworkAddress addr;

        // Load IP
        if (addr_json["ip"].size() != 16) {
          LOG_NET_WARN("Invalid IP address in peers file, skipping");
          continue;
        }
        for (size_t i = 0; i < 16; ++i) {
          addr.ip[i] = addr_json["ip"][i].get<uint8_t>();
        }

        addr.port = addr_json["port"].get<uint16_t>();
        addr.services = addr_json["services"].get<uint64_t>();

        AddrInfo info(addr, addr_json["timestamp"].get<uint32_t>());
        info.last_try = addr_json["last_try"].get<uint32_t>();
        info.last_success = addr_json["last_success"].get<uint32_t>();
        info.attempts = addr_json["attempts"].get<int>();
        info.tried = false;

        new_[info.get_key()] = info;
      }
    }

    // Calculate total size without calling size() to avoid recursive lock
    size_t total_size = tried_.size() + new_.size();
    LOG_NET_INFO("Successfully loaded {} addresses ({} tried, {} new)",
                 total_size, tried_.size(), new_.size());
    return true;

  } catch (const std::exception &e) {
    LOG_NET_ERROR("Exception during Load: {}", e.what());
    tried_.clear();
    new_.clear();
    return false;
  }
}

} // namespace network
} // namespace coinbasechain
