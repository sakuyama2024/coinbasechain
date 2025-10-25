#ifndef COINBASECHAIN_TEST_NETWORK_TEST_HELPERS_HPP
#define COINBASECHAIN_TEST_NETWORK_TEST_HELPERS_HPP

#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "test_orchestrator.hpp"

namespace coinbasechain {
namespace test {

inline void SetZeroLatency(SimulatedNetwork& network) {
    SimulatedNetwork::NetworkConditions c;
    c.latency_min = std::chrono::milliseconds(0);
    c.latency_max = std::chrono::milliseconds(0);
    c.jitter_max = std::chrono::milliseconds(0);
    network.SetNetworkConditions(c);
}

} // namespace test
} // namespace coinbasechain

#endif // COINBASECHAIN_TEST_NETWORK_TEST_HELPERS_HPP
