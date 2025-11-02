#pragma once

/*
 AddressManager (AddrMan) — simplified peer address manager for CoinbaseChain

 Purpose
 - Maintain two tables of peer addresses:
   • "new": learned but never successfully connected
   • "tried": previously successful connections
 - Select addresses for outbound and feeler dials with an 80% "tried" bias
   and a cooldown to avoid immediate re-dials
 - Persist state to JSON (peers.json) with atomic save and optional checksum
 - Apply basic hygiene: minimal address validation, timestamp clamping,
   and stale/"terrible" eviction

 How this differs from Bitcoin Core's addrman
 - No bucketization/source-grouping: this first release does NOT implement Core's
   bucket model. Selection is simpler (tried/new + cooldown) and has lower Sybil
   resistance. A bucketized design is planned for a future release.
 - Discovery policy location: GETADDR/ADDR privacy and echo-suppression policy
   lives in MessageRouter here, not inside AddrMan (Core intertwines more policy
   with addrman callers).
 - Persistence format: human-readable JSON with an optional SHA-256 checksum
   (over tried/new arrays) vs Core's binary peers.dat with checksumming.
 - Time handling: explicit timestamp clamping; future timestamps are not treated
   as stale. Tried entries record last_try so cooldown applies consistently.
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
#include <array>

namespace coinbasechain {
namespace network {

/**
 * AddressKey - Efficient binary key for address lookup (16-byte IP + 2-byte port)
 */
struct AddressKey {
  std::array<uint8_t,16> ip{};
  uint16_t port{0};
  auto operator<=>(const AddressKey&) const = default;
  bool operator==(const AddressKey&) const = default;
};

/**
 * AddrInfo - Extended address information with connection history
 */
struct AddrInfo {
  protocol::NetworkAddress address;
  int64_t timestamp;    // Last time we heard about this address
  int64_t last_try;     // Last connection attempt
  int64_t last_success; // Last successful connection
  int attempts;          // Number of connection attempts
  bool tried;            // Successfully connected at least once

  AddrInfo()
      : timestamp(0), last_try(0), last_success(0), attempts(0), tried(false) {}
  explicit AddrInfo(const protocol::NetworkAddress &addr, int64_t ts = 0)
      : address(addr), timestamp(ts), last_try(0), last_success(0), attempts(0),
        tried(false) {}

  // Check if address is too old to be useful
  bool is_stale(int64_t now) const;

  // Check if address is terrible (too many failed attempts, etc.)
  bool is_terrible(int64_t now) const;

  // Get binary key for this address (IP:port)
  AddressKey get_key() const;
};

/**
 * AddressManager - Manages peer addresses for peer discovery and connection
 */

class AddressManager {
public:
  AddressManager();

  // Add a new address from peer discovery
  bool add(const protocol::NetworkAddress &addr, int64_t timestamp = 0);

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
  std::map<AddressKey, AddrInfo> tried_;

  // "new" table: addresses we've heard about but haven't connected to
  std::map<AddressKey, AddrInfo> new_;

  // Auxiliary vectors for O(1) uniform selection
  std::vector<AddressKey> tried_keys_;
  std::vector<AddressKey> new_keys_;

  // Random number generator for selection
  std::mt19937 rng_;

  // Get current time as unix timestamp
  int64_t now() const;

  // Internal add (assumes lock is held)
  bool add_internal(const protocol::NetworkAddress &addr, int64_t timestamp);
};

} // namespace network
} // namespace coinbasechain


