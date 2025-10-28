#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "network/protocol.hpp"
#include "network/message.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;
using namespace coinbasechain::protocol;

static void ZeroLatency(SimulatedNetwork& net){
    SimulatedNetwork::NetworkConditions c; c.latency_min=c.latency_max=std::chrono::milliseconds(0); c.jitter_max=std::chrono::milliseconds(0); net.SetNetworkConditions(c);
}

TEST_CASE("DoS: INV storm bounded GETHEADERS post-IBD", "[dos][inv][throttle]") {
    SimulatedNetwork net(55001);
    ZeroLatency(net);
    net.EnableCommandTracking(true);

    // Miner builds base chain
    SimulatedNode miner(100, &net);
    for (int i=0;i<20;i++) (void)miner.MineBlock();

    // Victim node (will receive INV storms)
    SimulatedNode V(1, &net);

    // Create K peers that will announce new blocks to V
    const int K = 5;
    std::vector<std::unique_ptr<SimulatedNode>> peers;
    for (int i=0;i<K;i++) {
        peers.push_back(std::make_unique<SimulatedNode>(10+i, &net));
        // Peers sync from miner
        REQUIRE(peers.back()->ConnectTo(miner.GetId()));
    }
    uint64_t t=100; net.AdvanceTime(t);

    // Connect V <- peers
    for (auto& p: peers) REQUIRE(V.ConnectTo(p->GetId()));
    t+=200; net.AdvanceTime(t);

    // Wave #1: Miner mines one block; peers will learn it and INV to V
    (void)miner.MineBlock();
    for (int i=0;i<10;i++){ t+=50; net.AdvanceTime(t);} // allow propagation and INV flush

    // Count GETHEADERS V->each peer; should be <= K (one per peer)
    int total_gh = 0;
    for (auto& p: peers) {
        total_gh += net.CountCommandSent(V.GetId(), p->GetId(), commands::GETHEADERS);
    }
    REQUIRE(total_gh <= K);

    // Let V catch up to peers
    for (int i=0;i<20;i++){ t+=50; net.AdvanceTime(t);} 
    REQUIRE(V.GetTipHeight() == miner.GetTipHeight());

    // Wave #2: New block; ensure additional GETHEADERS is again bounded by K
    int pre_total = 0;
    for (auto& p: peers) pre_total += net.CountCommandSent(V.GetId(), p->GetId(), commands::GETHEADERS);

    (void)miner.MineBlock();
    for (int i=0;i<10;i++){ t+=50; net.AdvanceTime(t);} 

    int post_total = 0;
    for (auto& p: peers) post_total += net.CountCommandSent(V.GetId(), p->GetId(), commands::GETHEADERS);

    REQUIRE(post_total - pre_total <= K);
}
