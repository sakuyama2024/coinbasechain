// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license
// Test logging initialization helpers

#include "chain/logging.hpp"
#include <cstdio>
#include <string>

// Initialize logging for tests (console only, no file)
void InitializeTestLogging(const std::string& level) {
    // Initialize logging system with specified level
    coinbasechain::util::LogManager::Initialize(level, false, "");

    // If level is "trace", also enable TRACE for all components
    // This ensures LOG_CHAIN_TRACE, LOG_NET_TRACE, etc. all work
    if (level == "trace") {
        coinbasechain::util::LogManager::SetComponentLevel("chain", "trace");
        coinbasechain::util::LogManager::SetComponentLevel("network", "trace");
        coinbasechain::util::LogManager::SetComponentLevel("sync", "trace");
        coinbasechain::util::LogManager::SetComponentLevel("crypto", "trace");
        coinbasechain::util::LogManager::SetComponentLevel("app", "trace");
    }
}

// Shutdown logging system after tests complete
void ShutdownTestLogging() {
    coinbasechain::util::LogManager::Shutdown();
}
