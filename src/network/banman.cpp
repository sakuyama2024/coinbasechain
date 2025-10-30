// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "network/banman.hpp"
#include "util/logging.hpp"
#include <ctime>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace coinbasechain {
namespace network {

BanMan::BanMan(const std::string &datadir, bool auto_save)
    : m_datadir(datadir), m_auto_save(auto_save) {
  LOG_NET_TRACE("BanMan initialized (datadir: {}, auto_save: {})",
           datadir.empty() ? "<none>" : datadir, auto_save);
}

BanMan::~BanMan() {
  // Save on shutdown to persist bans
  if (!m_datadir.empty()) {
    Save();
  }
}

std::string BanMan::GetBanlistPath() const {
  if (m_datadir.empty()) {
    return "";
  }
  return m_datadir + "/banlist.json";
}

bool BanMan::Load() {
  std::lock_guard<std::mutex> lock(m_banned_mutex);

  std::string path = GetBanlistPath();
  if (path.empty()) {
    LOG_NET_TRACE("BanMan: no datadir specified, skipping load");
    return true;
  }

  std::ifstream file(path);
  if (!file.is_open()) {
    LOG_NET_TRACE("BanMan: no existing banlist found at {}", path);
    return true; // Not an error - first run
  }

  try {
    json j;
    file >> j;

    int64_t now = std::time(nullptr);
    size_t loaded = 0;
    size_t expired = 0;

    for (const auto &[address, ban_data] : j.items()) {
      CBanEntry entry;
      entry.nVersion = ban_data.value("version", CBanEntry::CURRENT_VERSION);
      entry.nCreateTime = ban_data.value("create_time", int64_t(0));
      entry.nBanUntil = ban_data.value("ban_until", int64_t(0));

      // Skip expired bans
      if (entry.IsExpired(now)) {
        expired++;
        continue;
      }

      m_banned[address] = entry;
      loaded++;
    }

    LOG_NET_TRACE("BanMan: loaded {} bans from {} (skipped {} expired)", loaded,
             path, expired);
    return true;

  } catch (const std::exception &e) {
    LOG_NET_ERROR("BanMan: failed to parse {}: {}", path, e.what());
    return false;
  }
}

// Internal save without acquiring lock (lock must already be held)
bool BanMan::SaveInternal() {
  std::string path = GetBanlistPath();
  if (path.empty()) {
    LOG_NET_TRACE("BanMan: no datadir specified, skipping save");
    return true;
  }

  // Sweep expired bans before saving
  int64_t now = std::time(nullptr);
  for (auto it = m_banned.begin(); it != m_banned.end();) {
    if (it->second.IsExpired(now)) {
      it = m_banned.erase(it);
    } else {
      ++it;
    }
  }

  try {
    json j;
    for (const auto &[address, entry] : m_banned) {
      j[address] = {{"version", entry.nVersion},
                    {"create_time", entry.nCreateTime},
                    {"ban_until", entry.nBanUntil}};
    }

    std::ofstream file(path);
    if (!file.is_open()) {
      LOG_NET_ERROR("BanMan: failed to open {} for writing", path);
      return false;
    }

    file << j.dump(2);
    LOG_NET_TRACE("BanMan: saved {} bans to {}", m_banned.size(), path);
    return true;

  } catch (const std::exception &e) {
    LOG_NET_ERROR("BanMan: failed to save {}: {}", path, e.what());
    return false;
  }
}

bool BanMan::Save() {
  std::lock_guard<std::mutex> lock(m_banned_mutex);
  return SaveInternal();
}

void BanMan::Ban(const std::string &address, int64_t ban_time_offset) {
  std::lock_guard<std::mutex> lock(m_banned_mutex);

  int64_t now = std::time(nullptr);
  int64_t ban_until =
      ban_time_offset > 0 ? now + ban_time_offset : 0; // 0 = permanent

  CBanEntry entry(now, ban_until);
  m_banned[address] = entry;

  if (ban_time_offset > 0) {
    LOG_NET_WARN("BanMan: banned {} until {} ({}s)", address, ban_until,
             ban_time_offset);
  } else {
    LOG_NET_WARN("BanMan: permanently banned {}", address);
  }
  // Auto-save
  if (m_auto_save && !m_datadir.empty()) {
      SaveInternal();
  }
}

void BanMan::Unban(const std::string &address) {
  std::lock_guard<std::mutex> lock(m_banned_mutex);

  auto it = m_banned.find(address);
  if (it != m_banned.end()) {
    m_banned.erase(it);
    LOG_NET_INFO("BanMan: unbanned {}", address);

    // Auto-save
    if (!m_datadir.empty()) {
        SaveInternal();
    }
  } else {
    LOG_NET_TRACE("BanMan: address {} was not banned", address);
  }
}

bool BanMan::IsBanned(const std::string &address) const {
  std::lock_guard<std::mutex> lock(m_banned_mutex);

  auto it = m_banned.find(address);
  if (it == m_banned.end()) {
    return false;
  }

  // Check if expired
  int64_t now = std::time(nullptr);
  return !it->second.IsExpired(now);
}

void BanMan::Discourage(const std::string &address) {
  std::lock_guard<std::mutex> lock(m_discouraged_mutex);

  int64_t now = std::time(nullptr);
  int64_t expiry = now + DISCOURAGEMENT_DURATION;

  m_discouraged[address] = expiry;
  LOG_NET_INFO("BanMan: discouraged {} until {} (~24h)", address, expiry);
}

bool BanMan::IsDiscouraged(const std::string &address) const {
  std::lock_guard<std::mutex> lock(m_discouraged_mutex);

  auto it = m_discouraged.find(address);
  if (it == m_discouraged.end()) {
    return false;
  }

  // Check if expired
  int64_t now = std::time(nullptr);
  if (now >= it->second) {
    // Expired - remove from map (const_cast for cleanup)
    const_cast<std::map<std::string, int64_t> &>(m_discouraged).erase(it);
    return false;
  }

  return true;
}

void BanMan::ClearDiscouraged() {
  std::lock_guard<std::mutex> lock(m_discouraged_mutex);
  m_discouraged.clear();
  LOG_NET_TRACE("BanMan: cleared all discouraged addresses");
}

std::map<std::string, CBanEntry> BanMan::GetBanned() const {
  std::lock_guard<std::mutex> lock(m_banned_mutex);
  return m_banned;
}

void BanMan::ClearBanned() {
  std::lock_guard<std::mutex> lock(m_banned_mutex);
  m_banned.clear();
  LOG_NET_TRACE("BanMan: cleared all bans");

  // Auto-save
  if (m_auto_save && !m_datadir.empty()) {
      SaveInternal();
  }
}

void BanMan::SweepBanned() {
  std::lock_guard<std::mutex> lock(m_banned_mutex);

  int64_t now = std::time(nullptr);
  size_t before = m_banned.size();

  for (auto it = m_banned.begin(); it != m_banned.end();) {
    if (it->second.IsExpired(now)) {
    LOG_NET_TRACE("BanMan: sweeping expired ban for {}", it->first);
      it = m_banned.erase(it);
    } else {
      ++it;
    }
  }

  size_t removed = before - m_banned.size();
  if (removed > 0) {
    LOG_NET_TRACE("BanMan: swept {} expired bans", removed);

    // Auto-save
    if (!m_datadir.empty()) {
      Save();
    }
  }
}

} // namespace network
} // namespace coinbasechain
