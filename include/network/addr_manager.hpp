#pragma once

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
 * AddressKey - Efficient binary key for address lookup (18 bytes: 16-byte IP + 2-byte port)
 * Uses lexicographic comparison for std::map compatibility
 */
struct AddressKey {
  std::array<uint8_t, 16> ip;
  uint16_t port;

  // Default comparison operators (lexicographic)
  auto operator<=>(const AddressKey&) const = default;
  bool operator==(const AddressKey&) const = default;
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

  // Get binary key for this address (efficient: no string formatting)
  AddressKey get_key() const;

  // Calculate selection probability based on failure count and recency
  // Bitcoin Core parity: probabilistic selection instead of hard cutoffs
  double GetChance(uint32_t now) const;
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
  std::map<AddressKey, AddrInfo> tried_;

  // "new" table: addresses we've heard about but haven't connected to
  std::map<AddressKey, AddrInfo> new_;

  // Auxiliary vectors for O(1) random access (avoid O(N) std::advance on map)
  // Must be kept in sync with tried_ and new_ maps
  std::vector<AddressKey> tried_keys_;
  std::vector<AddressKey> new_keys_;

  // Random number generator for selection
  std::mt19937 rng_;

  // Get current time as unix timestamp
  uint32_t now() const;

  // Internal add (assumes lock is held)
  bool add_internal(const protocol::NetworkAddress &addr, uint32_t timestamp);
};

} // namespace network
} // namespace coinbasechain


