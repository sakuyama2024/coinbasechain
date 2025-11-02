// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "network/banman.hpp"
#include "util/logging.hpp"
#include "util/time.hpp"
#include <fstream>
#include <filesystem>
#include <limits>
#include <nlohmann/json.hpp>
#include <fcntl.h>
#include <unistd.h>

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
  std::filesystem::path dir(m_datadir);
  return (dir / "banlist.json").string();
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

    int64_t now = util::GetTime();
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

    // Persist cleaned list if we skipped expired entries and autosave is enabled
    if (expired > 0 && m_auto_save && !m_datadir.empty()) {
      SaveInternal();
    }
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
  int64_t now = util::GetTime();
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

    // Write atomically: write to temp file then rename
    std::filesystem::path dest(path);
    std::filesystem::path tmp = dest;
    tmp += ".tmp";

    // Write atomically with durability: write to temp file, fsync, then rename
    std::string data = j.dump(2);

    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      LOG_NET_ERROR("BanMan: failed to open {} for writing", tmp.string());
      return false;
    }
    size_t total = 0;
    while (total < data.size()) {
      ssize_t n = ::write(fd, data.data() + total, data.size() - total);
      if (n <= 0) {
        LOG_NET_ERROR("BanMan: write error to {}", tmp.string());
        ::close(fd);
        std::error_code ec_remove;
        std::filesystem::remove(tmp, ec_remove);
        if (ec_remove) {
          LOG_NET_ERROR("BanMan: failed to remove temporary file {}: {}", tmp.string(), ec_remove.message());
        }
        return false;
      }
      total += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) {
      LOG_NET_ERROR("BanMan: fsync failed for {}", tmp.string());
      ::close(fd);
      std::error_code ec_remove;
      std::filesystem::remove(tmp, ec_remove);
      if (ec_remove) {
        LOG_NET_ERROR("BanMan: failed to remove temporary file {}: {}", tmp.string(), ec_remove.message());
      }
      return false;
    }
    ::close(fd);

    // Rename temp -> dest (best-effort atomic on same filesystem)
    std::error_code ec;
    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
      LOG_NET_ERROR("BanMan: failed to replace {} with {}: {}", dest.string(), tmp.string(), ec.message());
      // Attempt to remove temp file to avoid buildup
      std::error_code ec_remove;
      std::filesystem::remove(tmp, ec_remove);
      if (ec_remove) {
        LOG_NET_ERROR("BanMan: failed to remove temporary file {} after rename failure: {}", tmp.string(), ec_remove.message());
      }
      return false;
    }

    LOG_NET_TRACE("BanMan: saved {} bans to {}", m_banned.size(), dest.string());
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
  {
    std::lock_guard<std::mutex> wlock(m_whitelist_mutex);
    if (m_whitelist.find(address) != m_whitelist.end()) {
      LOG_NET_INFO("BanMan: refusing to ban whitelisted address {}", address);
      return;
    }
  }
  std::lock_guard<std::mutex> lock(m_banned_mutex);

  int64_t now = util::GetTime();
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
    if (m_auto_save && !m_datadir.empty()) {
        SaveInternal();
    }
  } else {
    LOG_NET_TRACE("BanMan: address {} was not banned", address);
  }
}

bool BanMan::IsBanned(const std::string &address) const {
  {
    std::lock_guard<std::mutex> wlock(m_whitelist_mutex);
    if (m_whitelist.find(address) != m_whitelist.end()) {
      return false;
    }
  }
  std::lock_guard<std::mutex> lock(m_banned_mutex);

  auto it = m_banned.find(address);
  if (it == m_banned.end()) {
    return false;
  }

  // Check if expired
  int64_t now = util::GetTime();
  return !it->second.IsExpired(now);
}

void BanMan::Discourage(const std::string &address) {
  {
    std::lock_guard<std::mutex> wlock(m_whitelist_mutex);
    if (m_whitelist.find(address) != m_whitelist.end()) {
      LOG_NET_TRACE("BanMan: skip discouraging whitelisted {}", address);
      return;
    }
  }
  std::lock_guard<std::mutex> lock(m_discouraged_mutex);

  int64_t now = util::GetTime();
  int64_t expiry = now + DISCOURAGEMENT_DURATION;

  m_discouraged[address] = expiry;
  LOG_NET_INFO("BanMan: discouraged {} until {} (~24h)", address, expiry);

  // Enforce upper bound to avoid unbounded growth under attack
  if (m_discouraged.size() > MAX_DISCOURAGED) {
    // First sweep expired
    for (auto it = m_discouraged.begin(); it != m_discouraged.end();) {
      if (now >= it->second) {
        it = m_discouraged.erase(it);
      } else {
        ++it;
      }
    }
    // If still too large, evict the entry with the earliest expiry
    if (m_discouraged.size() > MAX_DISCOURAGED) {
      auto victim = m_discouraged.end();
      int64_t min_expiry = std::numeric_limits<int64_t>::max();
      for (auto it = m_discouraged.begin(); it != m_discouraged.end(); ++it) {
        if (it->second < min_expiry) {
          min_expiry = it->second;
          victim = it;
        }
      }
      if (victim != m_discouraged.end()) {
        LOG_NET_TRACE("BanMan: evicting discouraged entry {} to enforce size cap ({} > {})",
                      victim->first, m_discouraged.size(), MAX_DISCOURAGED);
        m_discouraged.erase(victim);
      }
    }
  }
}

bool BanMan::IsDiscouraged(const std::string &address) const {
  {
    std::lock_guard<std::mutex> wlock(m_whitelist_mutex);
    if (m_whitelist.find(address) != m_whitelist.end()) {
      return false;
    }
  }
  std::lock_guard<std::mutex> lock(m_discouraged_mutex);

  auto it = m_discouraged.find(address);
  if (it == m_discouraged.end()) {
    return false;
  }

  // Check if expired (do not mutate here; cleanup is done in SweepDiscouraged())
  int64_t now = util::GetTime();
  if (now >= it->second) {
    return false;
  }

  return true;
}

void BanMan::ClearDiscouraged() {
  std::lock_guard<std::mutex> lock(m_discouraged_mutex);
  m_discouraged.clear();
  LOG_NET_TRACE("BanMan: cleared all discouraged addresses");
}

void BanMan::SweepDiscouraged() {
  std::lock_guard<std::mutex> lock(m_discouraged_mutex);
  const int64_t now = util::GetTime();
  size_t before = m_discouraged.size();
  for (auto it = m_discouraged.begin(); it != m_discouraged.end();) {
    if (now >= it->second) {
      it = m_discouraged.erase(it);
    } else {
      ++it;
    }
  }
  size_t removed = before - m_discouraged.size();
  if (removed > 0) {
    LOG_NET_TRACE("BanMan: swept {} expired discouraged entries", removed);
  }
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

  int64_t now = util::GetTime();
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
    if (m_auto_save && !m_datadir.empty()) {
      SaveInternal();
    }
  }
}


void BanMan::AddToWhitelist(const std::string& address) {
  // Lock all related structures in a strict global order to avoid deadlocks:
  // m_whitelist_mutex -> m_banned_mutex -> m_discouraged_mutex
  std::scoped_lock guard(m_whitelist_mutex, m_banned_mutex, m_discouraged_mutex);
  m_whitelist.insert(address);
  // Remove any existing ban or discouragement for this address
  auto itb = m_banned.find(address);
  if (itb != m_banned.end()) {
    m_banned.erase(itb);
  }
  auto itd = m_discouraged.find(address);
  if (itd != m_discouraged.end()) {
    m_discouraged.erase(itd);
  }
  // Persist ban removal if needed
  if (m_auto_save && !m_datadir.empty()) {
    SaveInternal();
  }
  LOG_NET_INFO("BanMan: whitelisted {} (removed any bans/discouragement)", address);
}

void BanMan::RemoveFromWhitelist(const std::string& address) {
  std::lock_guard<std::mutex> lock(m_whitelist_mutex);
  m_whitelist.erase(address);
  LOG_NET_TRACE("BanMan: removed {} from whitelist", address);
}

bool BanMan::IsWhitelisted(const std::string& address) const {
  std::lock_guard<std::mutex> lock(m_whitelist_mutex);
  return m_whitelist.find(address) != m_whitelist.end();
}

} // namespace network
} // namespace coinbasechain
