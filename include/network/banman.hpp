// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>

namespace coinbasechain {
namespace network {

// CBanEntry - Represents a single ban entry (stored persistently on disk)
struct CBanEntry {
  static constexpr int CURRENT_VERSION = 1;

  int nVersion{CURRENT_VERSION};
  int64_t nCreateTime{0}; // Unix timestamp when ban was created
  int64_t nBanUntil{0};   // Unix timestamp when ban expires (0 = permanent)

  CBanEntry() = default;
  CBanEntry(int64_t create_time, int64_t ban_until)
      : nCreateTime(create_time), nBanUntil(ban_until) {}

  bool IsExpired(int64_t now) const {
    // nBanUntil == 0 means permanent ban
    return nBanUntil > 0 && now >= nBanUntil;
  }
};

// BanMan - Manages persistent bans and temporary discouragement (from Bitcoin
// Core) Two-tier system:
// 1. Manual bans: Persistent, stored on disk, permanent or timed
// 2. Discouragement: Temporary, in-memory

class BanMan {
public:
  explicit BanMan(const std::string &datadir = "", bool auto_save = true);
  ~BanMan();

  bool Load();
  bool Save();

  // Manually ban address (0 = permanent, otherwise seconds until expiry)
  void Ban(const std::string &address, int64_t ban_time_offset = 0);
  void Unban(const std::string &address);
  bool IsBanned(const std::string &address) const;

  // Discourage address (automatic, temporary ~24 hours for misbehavior)
  void Discourage(const std::string &address);
  bool IsDiscouraged(const std::string &address) const;
  void ClearDiscouraged();
  // Prune expired discouraged entries
  void SweepDiscouraged();

  // Public constants for policy/testing
  static constexpr int64_t DISCOURAGEMENT_DURATION = 24 * 60 * 60; // 24h
  static constexpr size_t MAX_DISCOURAGED = 10000;

  // Whitelist (NoBan) support
  void AddToWhitelist(const std::string& address);
  void RemoveFromWhitelist(const std::string& address);
  bool IsWhitelisted(const std::string& address) const;

  std::map<std::string, CBanEntry> GetBanned() const;
  void ClearBanned();
  void SweepBanned();

private:
  // Data directory path
  std::string m_datadir;

  // Auto-save on modifications (disabled for tests to avoid race conditions)
  bool m_auto_save;

  // Banned addresses (persistent)
  mutable std::mutex m_banned_mutex;
  std::map<std::string, CBanEntry> m_banned;

  // Discouraged addresses (temporary, in-memory)
  // use a simple map with expiry times
  mutable std::mutex m_discouraged_mutex;
  std::map<std::string, int64_t> m_discouraged; // address -> expiry time

  std::string GetBanlistPath() const;
  bool SaveInternal(); // Internal save without lock (lock must already be held)

  // Whitelist (NoBan) state
  mutable std::mutex m_whitelist_mutex;
  std::unordered_set<std::string> m_whitelist;
};

} // namespace network
} // namespace coinbasechain


