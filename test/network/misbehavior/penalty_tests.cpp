// Misbehavior penalty tests (ported to test2)

#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "infra/attack_simulated_node.hpp"
#include "test_orchestrator.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;

static void SetZeroLatency(SimulatedNetwork& network){ SimulatedNetwork::NetworkConditions c; c.latency_min=c.latency_max=std::chrono::milliseconds(0); c.jitter_max=std::chrono::milliseconds(0); network.SetNetworkConditions(c);} 

TEST_CASE("MisbehaviorTest - InvalidPoWPenalty", "[misbehaviortest][network]") {
    SimulatedNetwork network(12345); SetZeroLatency(network);
    SimulatedNode victim(1,&network); AttackSimulatedNode attacker(2,&network);
    for(int i=0;i<5;i++) victim.MineBlock();
    victim.SetBypassPOWValidation(false);
attacker.ConnectTo(1);
    TestOrchestrator orch(&network);
    REQUIRE(orch.WaitForConnection(victim, attacker));
    attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 10);
    REQUIRE(orch.WaitForPeerCount(victim, 0, std::chrono::seconds(3)));
}

TEST_CASE("MisbehaviorTest - OversizedMessagePenalty", "[misbehaviortest][network]") {
    SimulatedNetwork network(12346); SetZeroLatency(network);
    SimulatedNode victim(10,&network); AttackSimulatedNode attacker(20,&network);
    for(int i=0;i<5;i++) victim.MineBlock();
attacker.ConnectTo(10);
    TestOrchestrator orch(&network);
    REQUIRE(orch.WaitForConnection(victim, attacker));
    for(int j=0;j<5;j++){ attacker.SendOversizedHeaders(10,3000);} 
    REQUIRE(orch.WaitForPeerCount(victim, 0, std::chrono::seconds(3)));
}

TEST_CASE("MisbehaviorTest - NonContinuousHeadersPenalty", "[misbehaviortest][network]") {
    SimulatedNetwork network(12347); SetZeroLatency(network);
    SimulatedNode victim(30,&network); AttackSimulatedNode attacker(40,&network);
    for(int i=0;i<5;i++) victim.MineBlock();
attacker.ConnectTo(30);
    TestOrchestrator orch(&network);
    REQUIRE(orch.WaitForConnection(victim, attacker));
    for(int j=0;j<5;j++){ attacker.SendNonContinuousHeaders(30,victim.GetTipHash()); }
    REQUIRE(orch.WaitForPeerCount(victim, 0, std::chrono::seconds(3)));
}

TEST_CASE("MisbehaviorTest - TooManyOrphansPenalty", "[misbehaviortest][network]") {
    SimulatedNetwork network(12348); SetZeroLatency(network);
    SimulatedNode victim(50,&network); AttackSimulatedNode attacker(60,&network);
    for(int i=0;i<5;i++) victim.MineBlock();
    victim.SetBypassPOWValidation(false);
attacker.ConnectTo(50);
    TestOrchestrator orch(&network);
    REQUIRE(orch.WaitForConnection(victim, attacker));
    attacker.SendOrphanHeaders(50,1000);
    REQUIRE(orch.WaitForPeerCount(victim, 0, std::chrono::seconds(5)));
}

TEST_CASE("MisbehaviorTest - ScoreAccumulation", "[misbehaviortest][network]") {
    SimulatedNetwork network(12349); SetZeroLatency(network);
    SimulatedNode victim(70,&network); AttackSimulatedNode attacker(80,&network);
    for(int i=0;i<5;i++) victim.MineBlock();
attacker.ConnectTo(70);
    TestOrchestrator orch(&network);
    REQUIRE(orch.WaitForConnection(victim, attacker));
    for(int j=0;j<4;j++){ attacker.SendNonContinuousHeaders(70,victim.GetTipHash()); }
    CHECK(victim.GetPeerCount()==1);
    attacker.SendNonContinuousHeaders(70,victim.GetTipHash());
    REQUIRE(orch.WaitForPeerCount(victim, 0, std::chrono::seconds(3)));
}
