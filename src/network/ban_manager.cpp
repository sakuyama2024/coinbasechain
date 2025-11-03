#include "network/ban_manager.hpp"
#include "util/logging.hpp"
#include "util/time.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <fcntl.h>
#include <unistd.h>

using json = nlohmann::json;

namespace coinbasechain {
namespace network {

// Policy constants
static constexpr size_t MAX_DISCOURAGED = 10000;

BanManager::BanManager() {
}

std::string BanManager::GetBanlistPath() const {
  if (ban_file_path_.empty()) {
    return "";
  }
  return ban_file_path_;
}

bool BanManager::LoadBans(const std::string& datadir) {
  std::lock_guard<std::mutex> lock(banned_mutex_);

  if (datadir.empty()) {
    LOG_NET_TRACE("BanManager: no datadir specified, skipping ban load");
    return true;
  }

  std::filesystem::path dir(datadir);
  ban_file_path_ = (dir / "banlist.json").string();

  std::ifstream file(ban_file_path_);
  if (!file.is_open()) {
    LOG_NET_TRACE("BanManager: no existing banlist found at {}", ban_file_path_);
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

      banned_[address] = entry;
      loaded++;
    }

    LOG_NET_TRACE("BanManager: loaded {} bans from {} (skipped {} expired)", loaded,
             ban_file_path_, expired);

    // Persist cleaned list if we skipped expired entries and autosave is enabled
    if (expired > 0 && ban_auto_save_ && !ban_file_path_.empty()) {
      SaveBansInternal();
    }
    return true;

  } catch (const std::exception &e) {
    LOG_NET_ERROR("BanManager: failed to parse {}: {}", ban_file_path_, e.what());
    return false;
  }
}

bool BanManager::SaveBansInternal() {
  if (ban_file_path_.empty()) {
    LOG_NET_TRACE("BanManager: no ban file path set, skipping save");
    return true;
  }

  // Sweep expired bans before saving
  int64_t now = util::GetTime();
  for (auto it = banned_.begin(); it != banned_.end();) {
    if (it->second.IsExpired(now)) {
      it = banned_.erase(it);
    } else {
      ++it;
    }
  }

  try {
    json j;
    for (const auto &[address, entry] : banned_) {
      j[address] = {{"version", entry.nVersion},
                    {"create_time", entry.nCreateTime},
                    {"ban_until", entry.nBanUntil}};
    }

    // Write atomically: write to temp file then rename
    std::filesystem::path dest(ban_file_path_);
    std::filesystem::path tmp = dest;
    tmp += ".tmp";

    // Write atomically with durability: write to temp file, fsync, then rename
    std::string data = j.dump(2);

    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      LOG_NET_ERROR("BanManager: failed to open {} for writing", tmp.string());
      return false;
    }
    size_t total = 0;
    while (total < data.size()) {
      ssize_t n = ::write(fd, data.data() + total, data.size() - total);
      if (n <= 0) {
        LOG_NET_ERROR("BanManager: write error to {}", tmp.string());
        ::close(fd);
        std::error_code ec_remove;
        std::filesystem::remove(tmp, ec_remove);
        if (ec_remove) {
          LOG_NET_ERROR("BanManager: failed to remove temporary file {}: {}", tmp.string(), ec_remove.message());
        }
        return false;
      }
      total += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) {
      LOG_NET_ERROR("BanManager: fsync failed for {}", tmp.string());
      ::close(fd);
      std::error_code ec_remove;
      std::filesystem::remove(tmp, ec_remove);
      if (ec_remove) {
        LOG_NET_ERROR("BanManager: failed to remove temporary file {}: {}", tmp.string(), ec_remove.message());
      }
      return false;
    }
    ::close(fd);

    // Rename temp -> dest (best-effort atomic on same filesystem)
    std::error_code ec;
    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
      LOG_NET_ERROR("BanManager: failed to replace {} with {}: {}", dest.string(), tmp.string(), ec.message());
      // Attempt to remove temp file to avoid buildup
      std::error_code ec_remove;
      std::filesystem::remove(tmp, ec_remove);
      if (ec_remove) {
        LOG_NET_ERROR("BanManager: failed to remove temporary file {} after rename failure: {}", tmp.string(), ec_remove.message());
      }
      return false;
    }

    LOG_NET_TRACE("BanManager: saved {} bans to {}", banned_.size(), dest.string());
    return true;

  } catch (const std::exception &e) {
    LOG_NET_ERROR("BanManager: failed to save {}: {}", ban_file_path_, e.what());
    return false;
  }
}

bool BanManager::SaveBans() {
  std::lock_guard<std::mutex> lock(banned_mutex_);
  return SaveBansInternal();
}

void BanManager::Ban(const std::string &address, int64_t ban_time_offset) {
  // Note: Like Bitcoin Core, we allow banning whitelisted addresses.
  // The whitelist is only checked at connection time, not ban time.
  std::lock_guard<std::mutex> lock(banned_mutex_);

  int64_t now = util::GetTime();
  int64_t ban_until =
      ban_time_offset > 0 ? now + ban_time_offset : 0; // 0 = permanent

  CBanEntry entry(now, ban_until);
  banned_[address] = entry;

  if (ban_time_offset > 0) {
    LOG_NET_WARN("BanManager: banned {} until {} ({}s)", address, ban_until,
             ban_time_offset);
  } else {
    LOG_NET_WARN("BanManager: permanently banned {}", address);
  }
  // Auto-save
  if (ban_auto_save_ && !ban_file_path_.empty()) {
      SaveBansInternal();
  }
}

void BanManager::Unban(const std::string &address) {
  std::lock_guard<std::mutex> lock(banned_mutex_);

  auto it = banned_.find(address);
  if (it != banned_.end()) {
    banned_.erase(it);
    LOG_NET_INFO("BanManager: unbanned {}", address);

    // Auto-save
    if (ban_auto_save_ && !ban_file_path_.empty()) {
        SaveBansInternal();
    }
  } else {
    LOG_NET_TRACE("BanManager: address {} was not banned", address);
  }
}

bool BanManager::IsBanned(const std::string &address) const {
  // Note: Like Bitcoin Core, we return the actual ban status regardless of whitelist.
  // The whitelist is checked separately at connection time, not when querying ban status.
  std::lock_guard<std::mutex> lock(banned_mutex_);

  auto it = banned_.find(address);
  if (it == banned_.end()) {
    return false;
  }

  // Check if expired
  int64_t now = util::GetTime();
  return !it->second.IsExpired(now);
}

void BanManager::Discourage(const std::string &address) {
  // Note: Like Bitcoin Core, we allow discouraging whitelisted addresses.
  // The whitelist is only checked at connection time, not at discourage time.
  std::lock_guard<std::mutex> lock(discouraged_mutex_);

  int64_t now = util::GetTime();
  int64_t expiry = now + DISCOURAGE_DURATION_SEC;

  discouraged_[address] = expiry;
  LOG_NET_INFO("BanManager: discouraged {} until {} (~24h)", address, expiry);

  // Enforce upper bound to avoid unbounded growth under attack
  if (discouraged_.size() > MAX_DISCOURAGED) {
    // First sweep expired
    for (auto it = discouraged_.begin(); it != discouraged_.end();) {
      if (now >= it->second) {
        it = discouraged_.erase(it);
      } else {
        ++it;
      }
    }
    // If still too large, evict the entry with the earliest expiry
    if (discouraged_.size() > MAX_DISCOURAGED) {
      auto victim = discouraged_.end();
      int64_t min_expiry = std::numeric_limits<int64_t>::max();
      for (auto it = discouraged_.begin(); it != discouraged_.end(); ++it) {
        if (it->second < min_expiry) {
          min_expiry = it->second;
          victim = it;
        }
      }
      if (victim != discouraged_.end()) {
        LOG_NET_TRACE("BanManager: evicting discouraged entry {} to enforce size cap ({} > {})",
                      victim->first, discouraged_.size(), MAX_DISCOURAGED);
        discouraged_.erase(victim);
      }
    }
  }
}

bool BanManager::IsDiscouraged(const std::string &address) const {
  // Note: Like Bitcoin Core, we return the actual discouragement status regardless of whitelist.
  // The whitelist is checked separately at connection time, not when querying discouragement status.
  std::lock_guard<std::mutex> lock(discouraged_mutex_);

  auto it = discouraged_.find(address);
  if (it == discouraged_.end()) {
    return false;
  }

  // Check if expired (do not mutate here; cleanup is done in SweepDiscouraged())
  int64_t now = util::GetTime();
  if (now >= it->second) {
    return false;
  }

  return true;
}

void BanManager::ClearDiscouraged() {
  std::lock_guard<std::mutex> lock(discouraged_mutex_);
  discouraged_.clear();
  LOG_NET_TRACE("BanManager: cleared all discouraged addresses");
}

void BanManager::SweepDiscouraged() {
  std::lock_guard<std::mutex> lock(discouraged_mutex_);
  const int64_t now = util::GetTime();
  size_t before = discouraged_.size();
  for (auto it = discouraged_.begin(); it != discouraged_.end();) {
    if (now >= it->second) {
      it = discouraged_.erase(it);
    } else {
      ++it;
    }
  }
  size_t removed = before - discouraged_.size();
  if (removed > 0) {
    LOG_NET_TRACE("BanManager: swept {} expired discouraged entries", removed);
  }
}

std::map<std::string, BanManager::CBanEntry> BanManager::GetBanned() const {
  std::lock_guard<std::mutex> lock(banned_mutex_);
  return banned_;
}

void BanManager::ClearBanned() {
  std::lock_guard<std::mutex> lock(banned_mutex_);
  banned_.clear();
  LOG_NET_TRACE("BanManager: cleared all bans");

  // Auto-save
  if (ban_auto_save_ && !ban_file_path_.empty()) {
      SaveBansInternal();
  }
}

void BanManager::SweepBanned() {
  std::lock_guard<std::mutex> lock(banned_mutex_);

  int64_t now = util::GetTime();
  size_t before = banned_.size();

  for (auto it = banned_.begin(); it != banned_.end();) {
    if (it->second.IsExpired(now)) {
      LOG_NET_TRACE("BanManager: sweeping expired ban for {}", it->first);
      it = banned_.erase(it);
    } else {
      ++it;
    }
  }

  size_t removed = before - banned_.size();
  if (removed > 0) {
    LOG_NET_TRACE("BanManager: swept {} expired bans", removed);

    // Auto-save
    if (ban_auto_save_ && !ban_file_path_.empty()) {
      SaveBansInternal();
    }
  }
}

void BanManager::AddToWhitelist(const std::string& address) {
  // Note: Like Bitcoin Core, whitelist and ban/discourage are independent.
  // We allow whitelisted addresses to also be banned/discouraged.
  // The whitelist overrides the ban only at connection acceptance time.
  std::lock_guard<std::mutex> guard(whitelist_mutex_);
  whitelist_.insert(address);
  LOG_NET_INFO("BanManager: whitelisted {}", address);
}

void BanManager::RemoveFromWhitelist(const std::string& address) {
  std::lock_guard<std::mutex> lock(whitelist_mutex_);
  whitelist_.erase(address);
  LOG_NET_TRACE("BanManager: removed {} from whitelist", address);
}

bool BanManager::IsWhitelisted(const std::string& address) const {
  std::lock_guard<std::mutex> lock(whitelist_mutex_);
  return whitelist_.find(address) != whitelist_.end();
}

} // namespace network
} // namespace coinbasechain
