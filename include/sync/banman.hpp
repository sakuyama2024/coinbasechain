// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_SYNC_BANMAN_HPP
#define COINBASECHAIN_SYNC_BANMAN_HPP

#include <string>
#include <map>
#include <mutex>
#include <cstdint>

namespace coinbasechain {
namespace sync {

/**
 * CBanEntry - Represents a single ban entry
 * Stored persistently on disk
 */
struct CBanEntry {
    static constexpr int CURRENT_VERSION = 1;

    int nVersion{CURRENT_VERSION};
    int64_t nCreateTime{0};      // Unix timestamp when ban was created
    int64_t nBanUntil{0};        // Unix timestamp when ban expires (0 = permanent)

    CBanEntry() = default;
    CBanEntry(int64_t create_time, int64_t ban_until)
        : nCreateTime(create_time), nBanUntil(ban_until) {}

    /**
     * Check if ban has expired
     */
    bool IsExpired(int64_t now) const {
        // nBanUntil == 0 means permanent ban
        return nBanUntil > 0 && now >= nBanUntil;
    }
};

/**
 * BanMan - Manages persistent bans and temporary discouragement
 *
 * Two-tier system:
 * 1. Manual bans: Persistent, stored on disk, permanent or timed
 * 2. Discouragement: Temporary, in-memory, probabilistic (bloom filter simulation)
 *
 * Based on Bitcoin Core's BanMan design.
 */
class BanMan {
public:
    /**
     * Constructor
     * @param datadir Path to data directory (for banlist.json)
     */
    explicit BanMan(const std::string& datadir = "");
    ~BanMan();

    /**
     * Load bans from disk
     */
    bool Load();

    /**
     * Save bans to disk
     */
    bool Save();

    /**
     * Manually ban an address
     * @param address IP address or hostname
     * @param ban_time_offset Seconds until ban expires (0 = permanent)
     */
    void Ban(const std::string& address, int64_t ban_time_offset = 0);

    /**
     * Manually unban an address
     */
    void Unban(const std::string& address);

    /**
     * Check if address is banned
     * @return true if banned and not expired
     */
    bool IsBanned(const std::string& address) const;

    /**
     * Discourage an address (automatic, temporary)
     * Used when peer misbehaves - soft ban for ~24 hours
     */
    void Discourage(const std::string& address);

    /**
     * Check if address is discouraged
     * @return true if discouraged (probabilistic check)
     */
    bool IsDiscouraged(const std::string& address) const;

    /**
     * Clear all discouragement (for testing/debug)
     */
    void ClearDiscouraged();

    /**
     * Get all banned addresses
     */
    std::map<std::string, CBanEntry> GetBanned() const;

    /**
     * Clear all bans (for testing/debug)
     */
    void ClearBanned();

    /**
     * Sweep expired bans
     */
    void SweepBanned();

private:
    // Data directory path
    std::string m_datadir;

    // Banned addresses (persistent)
    mutable std::mutex m_banned_mutex;
    std::map<std::string, CBanEntry> m_banned;

    // Discouraged addresses (temporary, in-memory)
    // In production, this would be a rolling bloom filter
    // For now, we use a simple map with expiry times
    mutable std::mutex m_discouraged_mutex;
    std::map<std::string, int64_t> m_discouraged;  // address -> expiry time

    // Discouragement duration (24 hours)
    static constexpr int64_t DISCOURAGEMENT_DURATION = 24 * 60 * 60;

    /**
     * Get banlist file path
     */
    std::string GetBanlistPath() const;
};

} // namespace sync
} // namespace coinbasechain

#endif // COINBASECHAIN_SYNC_BANMAN_HPP
