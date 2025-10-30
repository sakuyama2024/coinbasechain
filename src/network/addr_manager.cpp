#include "network/addr_manager.hpp"
#include "util/logging.hpp"
#include "util/time.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>

namespace coinbasechain {
namespace network {

// Constants (Bitcoin Core parity)
static constexpr uint32_t SECONDS_PER_DAY = 86400;
static constexpr uint32_t RECENT_TRY_SEC = 600; // 10 minutes

// Bitcoin Core staleness/terrible constants
static constexpr uint32_t ADDRMAN_HORIZON = 30 * SECONDS_PER_DAY;  // 30 days - how old addresses can maximally be
static constexpr int32_t ADDRMAN_RETRIES = 3;                       // After how many failed attempts we give up on a new node
static constexpr int32_t ADDRMAN_MAX_FAILURES = 10;                 // How many successive failures are allowed...
static constexpr uint32_t ADDRMAN_MIN_FAIL = 7 * SECONDS_PER_DAY;   // ...in at least this duration (7 days)

// AddrInfo implementation

AddressKey AddrInfo::get_key() const {
  AddressKey key;
  // Direct memory copy - no string formatting overhead
  std::copy(std::begin(address.ip), std::end(address.ip), key.ip.begin());
  key.port = address.port;
  return key;
}

bool AddrInfo::is_stale(uint32_t now) const {
  // Simple staleness check: address timestamp is older than HORIZON (30 days)
  return (now - timestamp) > ADDRMAN_HORIZON;
}

bool AddrInfo::is_terrible(uint32_t now) const {
  // Bitcoin Core parity: Full IsTerrible() logic from addrman.cpp lines 71-94

  // Never remove things tried in the last minute (grace period)
  if (last_try > 0 && (now - last_try) <= 60) {
    return false;
  }

  // Time traveler check: timestamp is more than 10 minutes in the future
  if (timestamp > now + 600) {
    return true;
  }

  // Not seen in recent history (older than 30 days)
  if ((now - timestamp) > ADDRMAN_HORIZON) {
    return true;
  }

  // For new addresses: tried N times and never a success
  // (last_success == 0 means never succeeded)
  if (last_success == 0 && attempts >= ADDRMAN_RETRIES) {
    return true;
  }

  // For tried addresses: N successive failures in the last week
  // (Must have succeeded at least once, but has many recent failures)
  if (last_success > 0 && (now - last_success) > ADDRMAN_MIN_FAIL &&
      attempts >= ADDRMAN_MAX_FAILURES) {
    return true;
  }

  return false;
}

double AddrInfo::GetChance(uint32_t now) const {
  double chance = 1.0;

  // Deprioritize very recent attempts (Bitcoin Core: 1% chance if tried < 10min ago)
  if (last_try > 0 && (now - last_try) < RECENT_TRY_SEC) {
    chance *= 0.01;
  }

  // Deprioritize by failure count: 66% per failure, capped at 8 attempts
  // After 8 failures: 0.66^8 = 3.57% chance (never zero!)
  // Formula: chance *= 0.66^min(attempts, 8)
  if (attempts > 0) {
    int capped_attempts = std::min(attempts, 8);
    chance *= std::pow(0.66, capped_attempts);
  }

  return chance;
}

// AddressManager implementation

AddressManager::AddressManager() : rng_(std::random_device{}()) {}

uint32_t AddressManager::now() const {
  // Use portable time utility (supports mock time for testing)
  return static_cast<uint32_t>(util::GetTime());
}

bool AddressManager::add(const protocol::NetworkAddress &addr,
                         uint32_t timestamp) {
  std::lock_guard<std::mutex> lock(mutex_);
  return add_internal(addr, timestamp);
}

bool AddressManager::add_internal(const protocol::NetworkAddress &addr,
                                  uint32_t timestamp) {
  AddrInfo info(addr, timestamp == 0 ? now() : timestamp);
  AddressKey key = info.get_key();

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

  AddrInfo info(addr);
  AddressKey key = info.get_key();

  // Update in tried table (checked first since select() prefers tried 80% of time)
  auto tried_it = tried_.find(key);
  if (tried_it != tried_.end()) {
    tried_it->second.last_try = now();
    tried_it->second.attempts++;
    return;
  }

  // Update in new table
  auto new_it = new_.find(key);
  if (new_it != new_.end()) {
    new_it->second.last_try = now();
    new_it->second.attempts++;
  }
}

void AddressManager::good(const protocol::NetworkAddress &addr) {
  std::lock_guard<std::mutex> lock(mutex_);

  AddrInfo info(addr);
  AddressKey key = info.get_key();
  uint32_t current_time = now();

  LOG_NET_TRACE("AddressManager::good() called for address port={}", addr.port);

  // Check if in new table
  auto new_it = new_.find(key);
  if (new_it != new_.end()) {
    // Move from new to tried
    LOG_NET_TRACE("Moving address port={} from 'new' to 'tried' table", addr.port);
    new_it->second.tried = true;
    new_it->second.last_success = current_time;
    new_it->second.attempts = 0; // Reset failure count

    tried_[key] = new_it->second;
    tried_keys_.push_back(key);
    new_.erase(new_it);

    // Remove key from new_keys_ vector (swap-and-pop for O(1) removal)
    auto vec_it = std::find(new_keys_.begin(), new_keys_.end(), key);
    if (vec_it != new_keys_.end()) {
      *vec_it = new_keys_.back();
      new_keys_.pop_back();
    }

    LOG_NET_TRACE("Address port={} successfully moved to 'tried'. New size: {}, Tried size: {}",
                  addr.port, new_.size(), tried_.size());
    return;
  }

  // Already in tried table
  auto tried_it = tried_.find(key);
  if (tried_it != tried_.end()) {
    LOG_NET_TRACE("Updating existing address port={} in 'tried' table", addr.port);
    tried_it->second.last_success = current_time;
    tried_it->second.attempts = 0; // Reset failure count
    return;
  }

  LOG_NET_WARN("AddressManager::good() called for unknown address port={}", addr.port);
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

      // Remove key from new_keys_ vector (swap-and-pop)
      auto vec_it = std::find(new_keys_.begin(), new_keys_.end(), key);
      if (vec_it != new_keys_.end()) {
        *vec_it = new_keys_.back();
        new_keys_.pop_back();
      }
    }
    return;
  }

  // Update in tried table
  auto tried_it = tried_.find(key);
  if (tried_it != tried_.end()) {
    // Just increment attempts - tried addresses stay in tried permanently
    // They become less likely to be selected via GetChance() penalty
    // Bitcoin Core parity: no table movement based on failure count
    tried_it->second.attempts++;
  }
}

std::optional<protocol::NetworkAddress> AddressManager::select() {
  std::lock_guard<std::mutex> lock(mutex_);

  // Return early if no addresses available
  if (tried_.empty() && new_.empty()) {
    return std::nullopt;
  }

  const uint32_t now_ts = now();
  double chance_factor = 1.0;

  // Bitcoin Core parity: Infinite loop with escalating chance_factor
  // Ensures eventual selection even for addresses with low GetChance()
  // (e.g., 8+ failures have 3.57% chance, will be selected after ~28 iterations)
  while (true) {
    // Select tried or new table with 50% probability (Bitcoin Core parity)
    std::uniform_int_distribution<int> dist(0, 99);
    bool use_tried = !tried_.empty() && (dist(rng_) < 50 || new_.empty());

    if (use_tried && !tried_keys_.empty()) {
      // Select random address from tried table (O(1) random access)
      std::uniform_int_distribution<size_t> idx_dist(0, tried_keys_.size() - 1);
      const AddressKey& key = tried_keys_[idx_dist(rng_)];
      auto it = tried_.find(key);

      if (it != tried_.end()) {
        const AddrInfo& info = it->second;

        // Probabilistic selection based on GetChance() * chance_factor
        // GetChance() returns penalty based on attempts and recency
        double selection_chance = chance_factor * info.GetChance(now_ts);

        // Generate random value [0.0, 1.0) and accept if below selection_chance
        std::uniform_real_distribution<double> prob_dist(0.0, 1.0);
        if (prob_dist(rng_) < selection_chance) {
          return info.address;
        }
      }
    } else if (!new_keys_.empty()) {
      // Select random address from new table (O(1) random access)
      std::uniform_int_distribution<size_t> idx_dist(0, new_keys_.size() - 1);
      const AddressKey& key = new_keys_[idx_dist(rng_)];
      auto it = new_.find(key);

      if (it != new_.end()) {
        const AddrInfo& info = it->second;

        // Probabilistic selection based on GetChance() * chance_factor
        double selection_chance = chance_factor * info.GetChance(now_ts);

        std::uniform_real_distribution<double> prob_dist(0.0, 1.0);
        if (prob_dist(rng_) < selection_chance) {
          return info.address;
        }
      }
    }

    // Not selected - escalate chance_factor to ensure eventual selection
    // Bitcoin Core uses 1.2 multiplier per iteration
    chance_factor *= 1.2;

    // Safety check: if chance_factor exceeds 100, something is very wrong
    // (should select within ~28 iterations even for 3.57% chance addresses)
    if (chance_factor > 100.0) {
      LOG_NET_ERROR("AddressManager::select() infinite loop exceeded safety threshold");
      return std::nullopt;
    }
  }
}

std::optional<protocol::NetworkAddress>
AddressManager::select_new_for_feeler() {
  std::lock_guard<std::mutex> lock(mutex_);

  // FEELER connections test addresses from "new" table (never connected before)
  // This helps move working addresses from "new" to "tried"
  if (new_keys_.empty()) {
    return std::nullopt;
  }

  // Select random address from "new" table only (O(1) random access)
  std::uniform_int_distribution<size_t> idx_dist(0, new_keys_.size() - 1);
  const AddressKey& key = new_keys_[idx_dist(rng_)];
  auto it = new_.find(key);
  if (it != new_.end()) {
    return it->second.address;
  }
  return std::nullopt;
}

std::vector<protocol::TimestampedAddress>
AddressManager::get_addresses(size_t max_count) {
  std::lock_guard<std::mutex> lock(mutex_);

  const uint32_t now_ts = now();
  std::vector<protocol::TimestampedAddress> result;
  result.reserve(std::min(max_count, tried_.size() + new_.size()));

  // Add tried addresses first, filtering terrible ones (Bitcoin Core parity)
  for (const auto &[key, info] : tried_) {
    if (result.size() >= max_count)
      break;

    // Bitcoin Core: Filter terrible addresses (addrman.cpp line 838)
    // Don't share addresses that are too old, have too many failures, etc.
    if (info.is_terrible(now_ts)) {
      continue;
    }

    result.push_back({info.timestamp, info.address});
  }

  // Add new addresses, filtering terrible ones
  for (const auto &[key, info] : new_) {
    if (result.size() >= max_count)
      break;

    // Filter terrible addresses
    if (info.is_terrible(now_ts)) {
      continue;
    }

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

  // Rebuild new_keys_ vector to match new_ map after removals
  new_keys_.clear();
  new_keys_.reserve(new_.size());
  for (const auto& [key, info] : new_) {
    new_keys_.push_back(key);
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

    // Rebuild key vectors for O(1) random access
    tried_keys_.clear();
    tried_keys_.reserve(tried_.size());
    for (const auto& [key, info] : tried_) {
      tried_keys_.push_back(key);
    }

    new_keys_.clear();
    new_keys_.reserve(new_.size());
    for (const auto& [key, info] : new_) {
      new_keys_.push_back(key);
    }

    // Calculate total size without calling size() to avoid recursive lock
    size_t total_size = tried_.size() + new_.size();
    LOG_NET_INFO("Successfully loaded {} addresses ({} tried, {} new)",
                 total_size, tried_.size(), new_.size());
    return true;

  } catch (const std::exception &e) {
    LOG_NET_ERROR("Exception during Load: {}", e.what());
    tried_.clear();
    tried_keys_.clear();
    new_.clear();
    new_keys_.clear();
    return false;
  }
}

} // namespace network
} // namespace coinbasechain
