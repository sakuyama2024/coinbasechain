// Copyright (c) 2024 Coinbase Chain
// Shared test helpers for network tests

#ifndef COINBASECHAIN_TEST_NETWORK_HELPERS_HPP
#define COINBASECHAIN_TEST_NETWORK_HELPERS_HPP

#include "simulated_network.hpp"
#include "simulated_node.hpp"
#include "attack_simulated_node.hpp"
#include "chain/chainparams.hpp"
#include <catch_amalgamated.hpp>
#include <vector>
#include <chrono>
#include <iostream>

namespace coinbasechain {
namespace test {

// Helper function to set zero latency for deterministic testing
static inline void SetZeroLatency(SimulatedNetwork& network) {
    SimulatedNetwork::NetworkConditions conditions;
    conditions.latency_min = std::chrono::milliseconds(0);
    conditions.latency_max = std::chrono::milliseconds(0);
    conditions.jitter_max = std::chrono::milliseconds(0);
    network.SetNetworkConditions(conditions);
}

// Global test setup - Catch2 style
// This runs once before all tests
struct NetworkTestGlobalSetup {
    NetworkTestGlobalSetup() {
        // Initialize global chain params for REGTEST
        chain::GlobalChainParams::Select(chain::ChainType::REGTEST);
    }
};
static NetworkTestGlobalSetup network_test_global_setup;

} // namespace test
} // namespace coinbasechain

#endif // COINBASECHAIN_TEST_NETWORK_HELPERS_HPP
