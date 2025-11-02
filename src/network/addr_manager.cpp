#include "network/addr_manager.hpp"
#include "util/logging.hpp"
#include "util/time.hpp"
#include "util/sha256.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <limits>

namespace coinbasechain {
namespace network {

// Constants
// An address in the NEW table is considered "stale" if we haven't heard about it for this many days.
// Stale NEW entries are removed by cleanup_stale(); TRIED entries are retained even if old (they worked before).
static constexpr int64_t STALE_AFTER_DAYS = 30;

// After this many consecutive failed connection attempts:
// - NEW: entry is considered "terrible" and may be removed
// - TRIED: entry is demoted back to NEW; further failures there may remove it
static constexpr uint32_t MAX_FAILURES = 10;

static constexpr int64_t SECONDS_PER_DAY = 86400; // Seconds in one day (utility for time math)
// Selection tuning constants:
// - SELECT_MAX_CHECKS: number of random probes into a table (TRIED/NEW) to find an eligible
//   address before falling back; prevents O(N) scans in large tables.
static constexpr size_t SELECT_MAX_CHECKS = 64;
// - SELECT_TRIED_BIAS_PERCENT: initial probability (0..100) to draw from TRIED vs NEW,
//   preferring known-good peers while still exploring NEW.
static constexpr int SELECT_TRIED_BIAS_PERCENT = 80;
// - SELECT_COOLDOWN_SEC: minimum time since last_try before an address is eligible; avoids
//   tight re-dial loops to the same peer.
static constexpr uint32_t SELECT_COOLDOWN_SEC = 600;      // 10 minutes
// - SELECT_ATTEMPT_BYPASS: after this many attempts, allow selection even if still in cooldown,
//   so flakier addresses are not starved forever.
static constexpr int SELECT_ATTEMPT_BYPASS = 30;

// AddrInfo implementation
AddressKey AddrInfo::get_key() const {
  AddressKey k;
  std::copy(address.ip.begin(), address.ip.end(), k.ip.begin());
  k.port = address.port;
  return k;
}

bool AddrInfo::is_stale(int64_t now) const {
  if (timestamp == 0 || timestamp > now) return false; // avoid underflow and treat future/zero as not stale
  return (now - timestamp) > (STALE_AFTER_DAYS * SECONDS_PER_DAY);
}

bool AddrInfo::is_terrible(int64_t now) const {
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

int64_t AddressManager::now() const {
  return util::GetTime();
}

bool AddressManager::add(const protocol::NetworkAddress &addr,
                         int64_t timestamp) {
  std::lock_guard<std::mutex> lock(mutex_);
  return add_internal(addr, timestamp);
}

bool AddressManager::add_internal(const protocol::NetworkAddress &addr,
                                  int64_t timestamp) {
  // Minimal validation: non-zero port and non-zero IP
  if (addr.port == 0) return false;
  bool all_zero = true; for (auto b : addr.ip) { if (b != 0) { all_zero = false; break; } }
  if (all_zero) return false;

  const int64_t now_s = now();
  // Clamp future or absurdly old timestamps to now
  const int64_t TEN_YEARS = 10ll * 365ll * 24ll * 60ll * 60ll;
  int64_t eff_ts = (timestamp == 0 ? now_s : timestamp);
  if (eff_ts > now_s || now_s - eff_ts > TEN_YEARS) eff_ts = now_s;

  AddrInfo info(addr, eff_ts);
  AddressKey key = info.get_key();

  // Check if already in tried table
  if (auto it = tried_.find(key); it != tried_.end()) {
    // Update timestamp if newer
    if (eff_ts > it->second.timestamp) {
      it->second.timestamp = eff_ts;
    }
    return false; // Already have it
  }

  // Check if already in new table
  if (auto it = new_.find(key); it != new_.end()) {
    // Update timestamp if newer
    if (eff_ts > it->second.timestamp) {
      it->second.timestamp = eff_ts;
    }
    return false; // Already have it
  }

  // Filter out terrible addresses
  if (info.is_terrible(now_s)) {
    return false;
  }

  // Add to new table
  new_[key] = info;
  new_keys_.push_back(key);
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

  AddrInfo probe(addr);
  AddressKey key = probe.get_key();
  int64_t t = now();

  // Update in new table
  if (auto it = new_.find(key); it != new_.end()) {
    it->second.last_try = t;
    it->second.attempts++;
    return;
  }

  // Update cooldown marker for tried entries as well
  if (auto it = tried_.find(key); it != tried_.end()) {
    it->second.last_try = t;
  }
}

void AddressManager::good(const protocol::NetworkAddress &addr) {
  std::lock_guard<std::mutex> lock(mutex_);

  AddrInfo info(addr);
  AddressKey key = info.get_key();
  int64_t current_time = now();

  // Logging: avoid formatting AddressKey directly
  LOG_NET_TRACE("AddressManager::good() called (port={})", addr.port);

  // Check if in new table
  auto new_it = new_.find(key);
  if (new_it != new_.end()) {
    // Move from new to tried
    LOG_NET_TRACE("Moving address (port={}) from 'new' to 'tried' table", addr.port);
    new_it->second.tried = true;
    new_it->second.last_success = current_time;
    new_it->second.attempts = 0; // Reset failure count

    tried_[key] = new_it->second;
    tried_keys_.push_back(key);
    new_.erase(new_it);
    // Remove from new_keys_
    auto vec_it = std::find(new_keys_.begin(), new_keys_.end(), key);
    if (vec_it != new_keys_.end()) { *vec_it = new_keys_.back(); new_keys_.pop_back(); }
    LOG_NET_TRACE("Address moved to 'tried'. New size: {}, Tried size: {}",
                  new_.size(), tried_.size());
    return;
  }

  // Already in tried table
  auto tried_it = tried_.find(key);
  if (tried_it != tried_.end()) {
    LOG_NET_TRACE("Updating existing address (port={}) in 'tried' table", addr.port);
    tried_it->second.last_success = current_time;
    tried_it->second.attempts = 0; // Reset failure count
    return;
  }

  LOG_NET_WARN("AddressManager::good() called for unknown address (port={})", addr.port);
}

void AddressManager::failed(const protocol::NetworkAddress &addr) {
  std::lock_guard<std::mutex> lock(mutex_);

  AddrInfo info(addr);
  AddressKey key = info.get_key();

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
      new_keys_.push_back(key);
      // remove from tried_keys_
      auto tv = std::find(tried_keys_.begin(), tried_keys_.end(), key);
      if (tv != tried_keys_.end()) { *tv = tried_keys_.back(); tried_keys_.pop_back(); }
      tried_.erase(tried_it);
    }
  }
}

std::optional<protocol::NetworkAddress> AddressManager::select() {
  std::lock_guard<std::mutex> lock(mutex_);

  // Prefer tried addresses (SELECT_TRIED_BIAS_PERCENT% of the time)
  std::uniform_int_distribution<int> dist(0, 99);
  bool use_tried = !tried_.empty() && (dist(rng_) < SELECT_TRIED_BIAS_PERCENT || new_.empty());

  const int64_t now_ts = now();

  auto ok = [&](const AddrInfo& info) -> bool {
    if (info.last_try == 0) return true;
    if (now_ts - info.last_try >= SELECT_COOLDOWN_SEC) return true;
    if (info.attempts >= SELECT_ATTEMPT_BYPASS) return true;
    return false;
  };

  if (use_tried && !tried_keys_.empty()) {
    std::uniform_int_distribution<size_t> idx_dist(0, tried_keys_.size() - 1);
    const size_t max_checks = std::min<size_t>(tried_keys_.size(), SELECT_MAX_CHECKS);
    for (size_t i = 0; i < max_checks; ++i) {
      const AddressKey& k = tried_keys_[idx_dist(rng_)];
      auto it = tried_.find(k);
      if (it != tried_.end() && ok(it->second)) {
        return it->second.address;
      }
    }
    // None in tried passed cooldown; try NEW table instead before falling back
    if (!new_keys_.empty()) {
      std::uniform_int_distribution<size_t> n_idx(0, new_keys_.size() - 1);
      const size_t n_checks = std::min<size_t>(new_keys_.size(), SELECT_MAX_CHECKS);
      for (size_t i = 0; i < n_checks; ++i) {
        const AddressKey& k = new_keys_[n_idx(rng_)];
        auto itn = new_.find(k);
        if (itn != new_.end() && ok(itn->second)) {
          return itn->second.address;
        }
      }
      // Fallback to any NEW if all failed ok()
      const AddressKey& k = new_keys_[n_idx(rng_)];
      auto itn2 = new_.find(k);
      if (itn2 != new_.end()) return itn2->second.address;
    }
    // As last resort, pick any tried (even if under cooldown)
    auto it = tried_.begin();
    std::advance(it, idx_dist(rng_));
    return it->second.address;
  }

  if (!new_keys_.empty()) {
    std::uniform_int_distribution<size_t> idx_dist(0, new_keys_.size() - 1);
    const size_t max_checks = std::min<size_t>(new_keys_.size(), SELECT_MAX_CHECKS);
    for (size_t i = 0; i < max_checks; ++i) {
      const AddressKey& k = new_keys_[idx_dist(rng_)];
      auto it = new_.find(k);
      if (it != new_.end() && ok(it->second)) {
        return it->second.address;
      }
    }
    // Try TRIED as alternative if NEW had none eligible
    if (!tried_keys_.empty()) {
      std::uniform_int_distribution<size_t> t_idx(0, tried_keys_.size() - 1);
      const size_t t_checks = std::min<size_t>(tried_keys_.size(), SELECT_MAX_CHECKS);
      for (size_t i = 0; i < t_checks; ++i) {
        const AddressKey& k = tried_keys_[t_idx(rng_)];
        auto itt = tried_.find(k);
        if (itt != tried_.end() && ok(itt->second)) {
          return itt->second.address;
        }
      }
      // Fallback to any TRIED if all failed ok()
      const AddressKey& k = tried_keys_[t_idx(rng_)];
      auto itt2 = tried_.find(k);
      if (itt2 != tried_.end()) return itt2->second.address;
    }
    // Finally fallback to any NEW
    const AddressKey& k = new_keys_[idx_dist(rng_)];
    auto it2 = new_.find(k);
    if (it2 != new_.end()) return it2->second.address;
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

  const int64_t now_s = now();

  // Add tried addresses first (filter invalid/terrible defensively)
  for (const auto &[key, info] : tried_) {
    if (result.size() >= max_count) break;
    if (info.address.port == 0) continue;
    bool all_zero = true; for (auto b : info.address.ip) { if (b != 0) { all_zero = false; break; } }
    if (all_zero) continue;
    if (info.is_terrible(now_s)) continue;
    {
      uint32_t ts = static_cast<uint32_t>(std::clamp<int64_t>(info.timestamp, 0, std::numeric_limits<uint32_t>::max()));
      result.emplace_back(ts, info.address);
    }
  }

  // Add new addresses (skip invalid/terrible)
  for (const auto &[key, info] : new_) {
    if (result.size() >= max_count) break;
    if (info.address.port == 0) continue;
    bool all_zero = true; for (auto b : info.address.ip) { if (b != 0) { all_zero = false; break; } }
    if (all_zero) continue;
    if (info.is_terrible(now_s)) continue;
    {
      uint32_t ts = static_cast<uint32_t>(std::clamp<int64_t>(info.timestamp, 0, std::numeric_limits<uint32_t>::max()));
      result.emplace_back(ts, info.address);
    }
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
  int64_t current_time = now();

  // Remove stale/terrible addresses from new table and keep keys in sync
  for (auto it = new_.begin(); it != new_.end();) {
    if (it->second.is_stale(current_time) || it->second.is_terrible(current_time)) {
      // remove key from vector
      auto vec_it = std::find(new_keys_.begin(), new_keys_.end(), it->first);
      if (vec_it != new_keys_.end()) { *vec_it = new_keys_.back(); new_keys_.pop_back(); }
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

    // Optional integrity checksum over tried+new arrays
    try {
      std::string payload = tried_array.dump() + new_array.dump();
      unsigned char hash[CSHA256::OUTPUT_SIZE];
      CSHA256().Write(reinterpret_cast<const unsigned char*>(payload.data()), payload.size()).Finalize(hash);
      nlohmann::json checksum_arr = nlohmann::json::array();
      for (size_t i = 0; i < CSHA256::OUTPUT_SIZE; ++i) checksum_arr.push_back(hash[i]);
      root["checksum"] = checksum_arr;
    } catch (...) {
      // If checksum calculation fails, proceed without it
    }

    // Atomic write: write to temp then rename (with fsync for durability)
    const std::string tmp = filepath + ".tmp";
    std::string data = root.dump(2);

    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      LOG_NET_ERROR("Failed to open temp peers file for writing: {}", tmp);
      return false;
    }

    size_t total = 0;
    while (total < data.size()) {
      ssize_t n = ::write(fd, data.data() + total, data.size() - total);
      if (n <= 0) {
        LOG_NET_ERROR("Failed to write temp peers file: {}", tmp);
        ::close(fd);
        std::error_code ec_remove;
        std::filesystem::remove(tmp, ec_remove);
        if (ec_remove) {
          LOG_NET_ERROR("Failed to remove temp peers file {}: {}", tmp, ec_remove.message());
        }
        return false;
      }
      total += static_cast<size_t>(n);
    }

    if (::fsync(fd) != 0) {
      LOG_NET_ERROR("fsync failed for temp peers file: {}", tmp);
      ::close(fd);
      std::error_code ec_remove;
      std::filesystem::remove(tmp, ec_remove);
      if (ec_remove) {
        LOG_NET_ERROR("Failed to remove temp peers file {} after fsync failure: {}", tmp, ec_remove.message());
      }
      return false;
    }

    ::close(fd);

    std::error_code ec;
    std::filesystem::rename(tmp, filepath, ec);
    if (ec) {
      // Try replace by removing destination first
      std::filesystem::remove(filepath, ec);
      std::filesystem::rename(tmp, filepath, ec);
      if (ec) {
        LOG_NET_ERROR("Failed to atomically replace peers file: {} -> {}: {}", tmp, filepath, ec.message());
        // Cleanup temp best-effort
        std::error_code ec_remove;
        std::filesystem::remove(tmp, ec_remove);
        if (ec_remove) {
          LOG_NET_ERROR("Failed to remove temp peers file {} after rename failure: {}", tmp, ec_remove.message());
        }
        return false;
      }
    }

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

    // Verify optional checksum
    bool checksum_ok = true;
    if (root.contains("checksum") && root["checksum"].is_array()) {
      try {
        std::string payload = (root.contains("tried") ? root["tried"].dump() : std::string()) +
                              (root.contains("new") ? root["new"].dump() : std::string());
        unsigned char hash[CSHA256::OUTPUT_SIZE];
        CSHA256().Write(reinterpret_cast<const unsigned char*>(payload.data()), payload.size()).Finalize(hash);
        const auto& arr = root["checksum"];
        if (arr.size() != CSHA256::OUTPUT_SIZE) {
          checksum_ok = false;
        } else {
          for (size_t i = 0; i < CSHA256::OUTPUT_SIZE; ++i) {
            uint32_t v = arr[i].get<uint32_t>();
            if (v > 255 || static_cast<unsigned char>(v) != hash[i]) { checksum_ok = false; break; }
          }
        }
      } catch (...) {
        checksum_ok = false;
      }
      if (!checksum_ok) {
        LOG_NET_ERROR("Peer address file checksum mismatch; refusing to load {}");
        tried_.clear();
        new_.clear();
        return false;
      }
    } else {
      LOG_NET_DEBUG("Peers file has no checksum (accepting for backwards compatibility)");
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

        AddrInfo info(addr, addr_json["timestamp"].get<int64_t>());
        info.last_try = addr_json["last_try"].get<int64_t>();
        info.last_success = addr_json["last_success"].get<int64_t>();
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

        AddrInfo info(addr, addr_json["timestamp"].get<int64_t>());
        info.last_try = addr_json["last_try"].get<int64_t>();
        info.last_success = addr_json["last_success"].get<int64_t>();
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
