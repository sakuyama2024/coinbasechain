// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "chain/logging.hpp"
#include <iostream>
#include <map>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <vector>

namespace coinbasechain {
namespace util {

bool LogManager::initialized_ = false;

static std::map<std::string, std::shared_ptr<spdlog::logger>> s_loggers;

void LogManager::Initialize(const std::string &log_level, bool log_to_file,
                            const std::string &log_file_path) {
  if (initialized_) {
    return;
  }

  try {
    // Create sinks
    std::vector<spdlog::sink_ptr> sinks;

    // File sink (append mode, like Bitcoin Core)
    if (log_to_file) {
      auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
          log_file_path, true); // true = append mode
      file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
      sinks.push_back(file_sink);
    } else {
      // Console sink for tests (colorized stdout)
      auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
      console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
      sinks.push_back(console_sink);
    }

    // Create loggers for different components
    std::vector<std::string> components = {"default", "network", "sync",
                                           "chain",   "crypto",  "app"};

    for (const auto &component : components) {
      auto logger = std::make_shared<spdlog::logger>(component, sinks.begin(),
                                                     sinks.end());
      logger->set_level(spdlog::level::from_str(log_level));
      logger->flush_on(
          spdlog::level::debug); // Flush all logs immediately (including DEBUG)
      spdlog::register_logger(logger);
      s_loggers[component] = logger;
    }

    // Set default logger
    spdlog::set_default_logger(s_loggers["default"]);

    initialized_ = true;

    // Add visual separator for new session
    if (log_to_file) {
      for (int i = 0; i < 10; ++i) {
        spdlog::default_logger()->info("");
      }
    }

    LOG_INFO("Logging system initialized (level: {})", log_level);
  } catch (const spdlog::spdlog_ex &ex) {
    std::cerr << "Log initialization failed: " << ex.what() << std::endl;
  }
}

void LogManager::Shutdown() {
  if (!initialized_) {
    return;
  }

  LOG_INFO("Shutting down logging system");

  spdlog::shutdown();
  s_loggers.clear();
  initialized_ = false;
}

std::shared_ptr<spdlog::logger> LogManager::GetLogger(const std::string &name) {
  if (!initialized_) {
    // Auto-initialize with defaults if not initialized
    Initialize();
  }

  auto it = s_loggers.find(name);
  if (it != s_loggers.end()) {
    return it->second;
  }

  // Return default logger if component not found
  return s_loggers["default"];
}

void LogManager::SetLogLevel(const std::string &level) {
  if (!initialized_) {
    return;
  }

  auto log_level = spdlog::level::from_str(level);
  for (auto &[name, logger] : s_loggers) {
    logger->set_level(log_level);
  }

  LOG_INFO("Log level changed to: {}", level);
}

void LogManager::SetComponentLevel(const std::string &component, const std::string &level) {
  if (!initialized_) {
    return;
  }

  auto it = s_loggers.find(component);
  if (it != s_loggers.end()) {
    auto log_level = spdlog::level::from_str(level);
    it->second->set_level(log_level);
    LOG_INFO("Component '{}' log level set to: {}", component, level);
  } else {
    LOG_WARN("Unknown log component: {}", component);
  }
}

} // namespace util
} // namespace coinbasechain
