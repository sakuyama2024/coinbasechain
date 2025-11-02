// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#pragma once

#include <memory>
#include <spdlog/spdlog.h>
#include <string>

namespace coinbasechain {
namespace util {

/**
 * Logging utility wrapper around spdlog
 *
 * Provides centralized logging configuration and easy access
 * to loggers throughout the application.
 *
 * Thread-safety: All methods are thread-safe. Initialization is
 * performed exactly once using std::call_once. Logger access is
 * protected by mutex for safe concurrent use.
 */
class LogManager {
public:
  /**
   * Initialize logging system
   * @param log_level Minimum log level (trace, debug, info, warn, error,
   * critical)
   * @param log_to_file If true, also log to file
   * @param log_file_path Path to log file (if log_to_file is true)
   *
   * Thread-safe: Uses std::call_once internally. Multiple calls are safe;
   * only the first call performs initialization.
   */
  static void Initialize(const std::string &log_level = "info",
                         bool log_to_file = false,
                         const std::string &log_file_path = "debug.log");

  /**
   * Shutdown logging system (flushes buffers)
   *
   * Thread-safe: Protected by mutex. Safe to call from any thread.
   * Subsequent logging calls after shutdown will auto-reinitialize.
   */
  static void Shutdown();

  /**
   * Get logger for specific component
   * @param name Component name (e.g., "network", "sync", "chain")
   *
   * Thread-safe: Protected by mutex. Auto-initializes if not initialized.
   * Returns cached logger for performance.
   */
  static std::shared_ptr<spdlog::logger>
  GetLogger(const std::string &name = "default");

  /**
   * Set log level at runtime (all components)
   *
   * Thread-safe: Protected by mutex.
   */
  static void SetLogLevel(const std::string &level);

  /**
   * Set log level for a specific component
   * @param component Component name (network, sync, chain, crypto, app, default)
   * @param level Log level (trace, debug, info, warn, error, critical)
   *
   * Thread-safe: Protected by mutex.
   */
  static void SetComponentLevel(const std::string &component, const std::string &level);

private:
  // No bool flag - using std::call_once for thread-safe initialization
};

} // namespace util
} // namespace coinbasechain

// Convenience macros for logging
#define LOG_TRACE(...)                                                         \
  coinbasechain::util::LogManager::GetLogger()->trace(__VA_ARGS__)
#define LOG_DEBUG(...)                                                         \
  coinbasechain::util::LogManager::GetLogger()->debug(__VA_ARGS__)
#define LOG_INFO(...)                                                          \
  coinbasechain::util::LogManager::GetLogger()->info(__VA_ARGS__)
#define LOG_WARN(...)                                                          \
  coinbasechain::util::LogManager::GetLogger()->warn(__VA_ARGS__)
#define LOG_ERROR(...)                                                         \
  coinbasechain::util::LogManager::GetLogger()->error(__VA_ARGS__)
#define LOG_CRITICAL(...)                                                      \
  coinbasechain::util::LogManager::GetLogger()->critical(__VA_ARGS__)

// Component-specific logging
#define LOG_NET_TRACE(...)                                                     \
  coinbasechain::util::LogManager::GetLogger("network")->trace(__VA_ARGS__)
#define LOG_NET_DEBUG(...)                                                     \
  coinbasechain::util::LogManager::GetLogger("network")->debug(__VA_ARGS__)
#define LOG_NET_INFO(...)                                                      \
  coinbasechain::util::LogManager::GetLogger("network")->info(__VA_ARGS__)
#define LOG_NET_WARN(...)                                                      \
  coinbasechain::util::LogManager::GetLogger("network")->warn(__VA_ARGS__)
#define LOG_NET_ERROR(...)                                                     \
  coinbasechain::util::LogManager::GetLogger("network")->error(__VA_ARGS__)

#define LOG_SYNC_TRACE(...)                                                    \
  coinbasechain::util::LogManager::GetLogger("sync")->trace(__VA_ARGS__)
#define LOG_SYNC_DEBUG(...)                                                    \
  coinbasechain::util::LogManager::GetLogger("sync")->debug(__VA_ARGS__)
#define LOG_SYNC_INFO(...)                                                     \
  coinbasechain::util::LogManager::GetLogger("sync")->info(__VA_ARGS__)
#define LOG_SYNC_WARN(...)                                                     \
  coinbasechain::util::LogManager::GetLogger("sync")->warn(__VA_ARGS__)
#define LOG_SYNC_ERROR(...)                                                    \
  coinbasechain::util::LogManager::GetLogger("sync")->error(__VA_ARGS__)

#define LOG_CHAIN_TRACE(...)                                                   \
  coinbasechain::util::LogManager::GetLogger("chain")->trace(__VA_ARGS__)
#define LOG_CHAIN_DEBUG(...)                                                   \
  coinbasechain::util::LogManager::GetLogger("chain")->debug(__VA_ARGS__)
#define LOG_CHAIN_INFO(...)                                                    \
  coinbasechain::util::LogManager::GetLogger("chain")->info(__VA_ARGS__)
#define LOG_CHAIN_WARN(...)                                                    \
  coinbasechain::util::LogManager::GetLogger("chain")->warn(__VA_ARGS__)
#define LOG_CHAIN_ERROR(...)                                                   \
  coinbasechain::util::LogManager::GetLogger("chain")->error(__VA_ARGS__)

#define LOG_CRYPTO_TRACE(...)                                                  \
  coinbasechain::util::LogManager::GetLogger("crypto")->trace(__VA_ARGS__)
#define LOG_CRYPTO_DEBUG(...)                                                  \
  coinbasechain::util::LogManager::GetLogger("crypto")->debug(__VA_ARGS__)
#define LOG_CRYPTO_INFO(...)                                                   \
  coinbasechain::util::LogManager::GetLogger("crypto")->info(__VA_ARGS__)
#define LOG_CRYPTO_WARN(...)                                                   \
  coinbasechain::util::LogManager::GetLogger("crypto")->warn(__VA_ARGS__)
#define LOG_CRYPTO_ERROR(...)                                                  \
  coinbasechain::util::LogManager::GetLogger("crypto")->error(__VA_ARGS__)


