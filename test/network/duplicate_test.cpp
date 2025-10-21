#include "catch_amalgamated.hpp"
#include "simulated_network.hpp"
#include "simulated_node.hpp"
#include "network_test_helpers.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;

TEST_CASE("Quick duplicate connection test", "[duplicate]") {
    SimulatedNetwork network(12345);
    SetZeroLatency(network);
    
    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);
    
    uint64_t time_ms = 1000000;
    
    // First connection
    bool first = node1.ConnectTo(2);
    INFO("First connection attempt: " << first);
    
    time_ms += 5000;
    network.AdvanceTime(time_ms);
    
    size_t peers_after_first = node1.GetOutboundPeerCount();
    INFO("Outbound peers after first: " << peers_after_first);
    
    // Second connection (should be rejected)
    bool second = node1.ConnectTo(2);
    INFO("Second connection attempt: " << second);
    
    time_ms += 5000;
    network.AdvanceTime(time_ms);
    
    size_t peers_after_second = node1.GetOutboundPeerCount();
    INFO("Outbound peers after second: " << peers_after_second);
    
    // Should still have only 1 outbound connection
    REQUIRE(peers_after_second == peers_after_first);
}
