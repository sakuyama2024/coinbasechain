// Network sync and IBD tests (ported to test2; heavy tests skipped by default)

#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;

static void SetZeroLatency(SimulatedNetwork& network) {
    SimulatedNetwork::NetworkConditions c; c.latency_min=c.latency_max=std::chrono::milliseconds(0); c.jitter_max=std::chrono::milliseconds(0); network.SetNetworkConditions(c);
}

TEST_CASE("NetworkSync - InitialSync", "[networksync][network]") {
    SimulatedNetwork network(24001);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    node2.ConnectTo(1);
    uint64_t t=100; network.AdvanceTime(t);

    for (int i=0;i<100;i++){ (void)node1.MineBlock(); t+=50; network.AdvanceTime(t); }
    CHECK(node1.GetTipHeight()==100);
    CHECK(node2.GetTipHeight()==100);
    CHECK(node2.GetTipHash()==node1.GetTipHash());
}

TEST_CASE("NetworkSync - SyncFromMultiplePeers", "[networksync][network]") {
    SimulatedNetwork network(24002);
    SetZeroLatency(network);

    SimulatedNode a(1,&network); SimulatedNode b(2,&network); SimulatedNode n(3,&network);
    uint64_t t=100;
    for(int i=0;i<50;i++){ (void)a.MineBlock(); t+=50; }

    b.ConnectTo(1); t+=100; network.AdvanceTime(t);
    CHECK(b.GetTipHeight()==50);

    n.ConnectTo(1); n.ConnectTo(2); t+=5000; network.AdvanceTime(t);
    CHECK(n.GetTipHeight()==50);
}

TEST_CASE("NetworkSync - CatchUpAfterMining", "[networksync][network]") {
    SimulatedNetwork network(24003); SetZeroLatency(network);
    SimulatedNode node1(1,&network); SimulatedNode node2(2,&network);
    node1.ConnectTo(2); uint64_t t=100; network.AdvanceTime(t);
    for(int i=0;i<20;i++){ (void)node1.MineBlock(); t+=100; network.AdvanceTime(t);} 
    CHECK(node2.GetTipHeight()==20);
}

TEST_CASE("IBDTest - FreshNodeSyncsFromGenesis", "[ibdtest][network]") {
    SimulatedNetwork network(24004); SetZeroLatency(network);
    SimulatedNode miner(1,&network); SimulatedNode fresh(2,&network);
    for(int i=0;i<200;i++) (void)miner.MineBlock();
    CHECK(miner.GetTipHeight()==200); CHECK(fresh.GetTipHeight()==0);
    fresh.ConnectTo(1); uint64_t t=100; network.AdvanceTime(t);
    for(int i=0;i<50;i++){ t+=200; network.AdvanceTime(t);} 
    CHECK(fresh.GetTipHeight()==200); CHECK(fresh.GetTipHash()==miner.GetTipHash());
}

TEST_CASE("IBDTest - LargeChainSync", "[ibdtest][network][.]") {
    SimulatedNetwork network(24005); SetZeroLatency(network);
    SimulatedNode miner(1,&network); SimulatedNode sync(2,&network);
    uint64_t t=1000; for(int i=0;i<2000;i++){ t+=1000; network.AdvanceTime(t); (void)miner.MineBlock(); }
    t = 10000000; network.AdvanceTime(t);
    sync.ConnectTo(1); t+=100; network.AdvanceTime(t);
    for(int i=0;i<6;i++){ t+=35000; network.AdvanceTime(t); if(sync.GetTipHeight()==miner.GetTipHeight()) break; }
    CHECK(sync.GetTipHeight()==miner.GetTipHeight());
    CHECK(sync.GetTipHash()==miner.GetTipHash());
}
