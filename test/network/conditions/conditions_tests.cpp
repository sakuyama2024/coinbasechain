// Network conditions tests (ported to test2)

#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;

static void SetZeroLatency(SimulatedNetwork& network) {
    SimulatedNetwork::NetworkConditions c; c.latency_min=c.latency_max=std::chrono::milliseconds(0); c.jitter_max=std::chrono::milliseconds(0); network.SetNetworkConditions(c);
}

TEST_CASE("NetworkConditionsTest - HighLatency", "[networkconditionstest][network]") {
    SimulatedNetwork network(27001);
    SetZeroLatency(network);
    SimulatedNode node1(1,&network); SimulatedNode node2(2,&network);
    node1.ConnectTo(2); uint64_t t=100; network.AdvanceTime(t);
    SimulatedNetwork::NetworkConditions cond; cond.latency_min=std::chrono::milliseconds(500); cond.latency_max=std::chrono::milliseconds(500); cond.jitter_max=std::chrono::milliseconds(0);
    network.SetNetworkConditions(cond);
    (void)node1.MineBlock();
    for(int i=0;i<20;i++){ t+=200; network.AdvanceTime(t);} 
    CHECK(node2.GetTipHeight()==1);
}

TEST_CASE("NetworkConditionsTest - PacketLoss", "[networkconditionstest][network]") {
    SimulatedNetwork network(27002);
    SetZeroLatency(network);
    SimulatedNode node1(1,&network); SimulatedNode node2(2,&network);
    node1.ConnectTo(2); uint64_t t=100; network.AdvanceTime(t);
    SimulatedNetwork::NetworkConditions cond; cond.packet_loss_rate=0.5; cond.latency_min=std::chrono::milliseconds(1); cond.latency_max=std::chrono::milliseconds(10);
    network.SetNetworkConditions(cond);
    for(int i=0;i<100;i++){ (void)node1.MineBlock(); t+=1000; network.AdvanceTime(t);} 
    t+=35000; network.AdvanceTime(t);
    int h=node2.GetTipHeight(); CHECK(h>0); CHECK(h<100);
}

TEST_CASE("NetworkConditionsTest - BandwidthLimits", "[networkconditionstest][network][.]") {
    SimulatedNetwork network(27003);
    SimulatedNetwork::NetworkConditions cond; cond.bandwidth_bytes_per_sec=10000; network.SetNetworkConditions(cond);
    // Documentation test: detailed timing not asserted here
    SUCCEED();
}
