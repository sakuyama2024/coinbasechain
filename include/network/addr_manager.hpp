#ifndef COINBASECHAIN_ADDR_MANAGER_HPP
#define COINBASECHAIN_ADDR_MANAGER_HPP

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

  // Get key for this address (IP:port)
  std::string get_key() const;
};

/**
 * AddressManager - Manages peer addresses with tried/new buckets
 *
 * Simplified version inspired by Bitcoin's AddrMan:
 * - "new" addresses: heard about but not yet connected
 * - "tried" addresses: successfully connected at least once
 * - Random selection with preference for tried addresses
 * - Stale address cleanup
 *
 * TODO: Security improvements needed for production:
 * 1. Bucket-based architecture (256 tried × 64, 1024 new × 64) instead of
 * simple maps
 * 2. Cryptographic bucket selection using secret key to prevent targeted
 * attacks
 * 3. NetGroupManager for network group diversity (/16 IPv4, /32 IPv6,
 * ASN-based)
 * 4. Collision handling with FEELER connections for bucket overflow
 * 5. Persistence (serialize/deserialize to disk)
 * 6. Connected() separate from Good() for connection tracking
 * 7. SetServices() to update service flags
 * 8. Network type filtering (IPv4/IPv6/Tor/I2P)
 * 9. Time penalties for addresses from untrusted sources
 * 10. Probabilistic selection based on GetChance() (recency, failure rate)
 *
 * Current implementation is vulnerable to sybil attacks (attacker filling our
 * address table with their nodes). The bucket system provides anti-sybil
 * protection by limiting how many addresses from the same network can be
 * stored.
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

  // Get multiple addresses for ADDR message (limited to max_count)
  std::vector<protocol::TimestampedAddress>
  get_addresses(size_t max_count = 1000);

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
  std::map<std::string, AddrInfo> tried_;

  // "new" table: addresses we've heard about but haven't connected to
  std::map<std::string, AddrInfo> new_;

  // Random number generator for selection
  std::mt19937 rng_;

  // Get current time as unix timestamp
  uint32_t now() const;

  // Internal add (assumes lock is held)
  bool add_internal(const protocol::NetworkAddress &addr, uint32_t timestamp);
};

} // namespace network
} // namespace coinbasechain

#endif // COINBASECHAIN_ADDR_MANAGER_HPP
