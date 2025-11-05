#pragma once

/*
 AddressManager (AddrMan) — simplified peer address manager for CoinbaseChain

 Purpose
 - Maintain two tables of peer addresses:
   • "new": learned but never successfully connected
   • "tried": previously successful connections
 - Select addresses for outbound and feeler dials with an 80% "tried" bias
   and a cooldown to avoid immediate re-dials
 - Persist state to JSON (peers.json) with atomic save (fsync + rename)
 - Apply basic hygiene: minimal address validation, timestamp clamping,
   and stale/"terrible" eviction

 How this differs from Bitcoin Core's addrman
 - No bucketization/source-grouping: this first release does NOT implement Core's
   bucket model. Selection is simpler (tried/new + cooldown) 
 - Persistence format: human-readable JSON vs Core's binary peers.dat. Corruption
   detection relies on nlohmann::json parser error handling.
 - Simpler scoring: no per-entry chance weighting or privacy scoring; limits like
   STALE_AFTER_DAYS and MAX_FAILURES are compile-time constants.

 Notes
 - Selection prefers entries passing cooldown; if no TRIED entry is eligible it
   falls back to NEW before choosing any TRIED under cooldown.
 - get_addresses() filters invalid and "terrible" entries.
 - Future work: add bucketization and stronger per-network-group diversity to
   better match Core's behavior.
*/

#include "network/protocol.hpp"
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <vector>

namespace coinbasechain {
namespace network {

/**
 * AddrKey - Binary key for address lookups (16-byte IP + 2-byte port, big-endian)
 *
 * SECURITY: Zero-allocation key structure prevents heap fragmentation and timing attacks.
 * Using binary representation instead of hex strings eliminates:
 * - String allocation overhead (36 bytes/key → stringstream heap allocations)
 * - Collision risk from string conversion
 * Total size: 18 bytes (16 IP + 2 port big-endian)
 */
struct AddrKey {
  std::array<uint8_t, 18> data;  // 16-byte IPv6 + 2-byte port (big-endian)

  // Construct from NetworkAddress
  explicit AddrKey(const protocol::NetworkAddress &addr) {
    // Copy 16-byte IP
    std::copy(addr.ip.begin(), addr.ip.end(), data.begin());
    // Copy 2-byte port (big-endian)
    data[16] = static_cast<uint8_t>((addr.port >> 8) & 0xFF);
    data[17] = static_cast<uint8_t>(addr.port & 0xFF);
  }

  // Comparison for std::map
  bool operator<(const AddrKey &other) const { return data < other.data; }

  // Equality for testing
  bool operator==(const AddrKey &other) const { return data == other.data; }
};

/**
 * AddrInfo - Extended address information with connection history
 */
struct AddrInfo {
  protocol::NetworkAddress address;
  uint32_t timestamp;    // Last time we heard about this address
  uint32_t last_try;     // Last connection attempt
  uint32_t last_success; // Last successful connection
  int attempts;          // Number of connection attempts
  bool tried;            // Successfully connected at least once

  AddrInfo()
      : timestamp(0), last_try(0), last_success(0), attempts(0), tried(false) {}
  explicit AddrInfo(const protocol::NetworkAddress &addr, uint32_t ts = 0)
      : address(addr), timestamp(ts), last_try(0), last_success(0), attempts(0),
        tried(false) {}

  // Check if address is too old to be useful
  bool is_stale(uint32_t now) const;

  // Check if address is terrible (too many failed attempts, etc.)
  bool is_terrible(uint32_t now) const;
};

/**
 * AddressManager - Manages peer addresses for peer discovery and connection
 */

class AddressManager {
public:
  AddressManager();

  // Add a new address from peer discovery
  bool add(const protocol::NetworkAddress &addr, uint32_t timestamp = 0);

  // Add multiple addresses (e.g., from ADDR message)
  size_t
  add_multiple(const std::vector<protocol::TimestampedAddress> &addresses);

  // Mark address as a connection attempt
  void attempt(const protocol::NetworkAddress &addr);

  // Mark address as successfully connected
  void good(const protocol::NetworkAddress &addr);

  // Mark address as connection failure
  void failed(const protocol::NetworkAddress &addr);

  // Get a random address to connect to
  std::optional<protocol::NetworkAddress> select();

  // Select address from "new" table for feeler connection
  std::optional<protocol::NetworkAddress> select_new_for_feeler();

  // Get multiple addresses for ADDR message (limited to max_count)
  std::vector<protocol::TimestampedAddress>
  get_addresses(size_t max_count = protocol::MAX_ADDR_SIZE);

  // Get statistics
  size_t size() const;
  size_t tried_count() const;
  size_t new_count() const;

  // Remove stale addresses
  void cleanup_stale();

  // Persistence
  bool Save(const std::string &filepath);
  bool Load(const std::string &filepath);

private:
  mutable std::mutex mutex_;

  // "tried" table: addresses we've successfully connected to
  std::map<AddrKey, AddrInfo> tried_;

  // "new" table: addresses we've heard about but haven't connected to
  std::map<AddrKey, AddrInfo> new_;

  // Random number generator for selection (base entropy source)
  std::mt19937 rng_;

  // Create RNG with per-request entropy to prevent seed prediction attacks
  // Bitcoin Core pattern: mix base RNG state with time to prevent offline seed brute-force
  std::mt19937 make_request_rng();

  // Get current time as unix timestamp
  uint32_t now() const;

  // Internal add (assumes lock is held)
  bool add_internal(const protocol::NetworkAddress &addr, uint32_t timestamp);
};

} // namespace network
} // namespace coinbasechain


