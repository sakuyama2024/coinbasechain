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

TEST_CASE("DuplicateHeaders - Resending same valid header does not penalize or disconnect", "[misbehaviortest][network][duplicates]") {
    SimulatedNetwork network(12350); SetZeroLatency(network);
    SimulatedNode victim(90, &network); AttackSimulatedNode attacker(91, &network);

    // Ensure victim has a known tip to attach to
    for (int i = 0; i < 3; ++i) victim.MineBlock();

    // Connect attacker -> victim
    attacker.ConnectTo(90);
    TestOrchestrator orch(&network);
    REQUIRE(orch.WaitForConnection(victim, attacker));

    // Build a header that connects to victim's tip
    CBlockHeader hdr;
    hdr.nVersion = 1;
    hdr.hashPrevBlock = victim.GetTipHash();
    hdr.nTime = static_cast<uint32_t>(network.GetCurrentTime() / 1000);
    hdr.nBits = coinbasechain::chain::GlobalChainParams::Get().GenesisBlock().nBits;
    hdr.nNonce = 42;
    // PoW bypass is enabled by default in tests; non-null value to pass cheap checks if needed
    hdr.hashRandomX.SetHex("0000000000000000000000000000000000000000000000000000000000000001");

    // Serialize HEADERS with single header
    message::HeadersMessage msg; msg.headers = {hdr};
    auto payload = msg.serialize();
    auto mhdr = message::create_header(protocol::magic::REGTEST, protocol::commands::HEADERS, payload);
    auto mhdr_bytes = message::serialize_header(mhdr);
    std::vector<uint8_t> full; full.reserve(mhdr_bytes.size() + payload.size());
    full.insert(full.end(), mhdr_bytes.begin(), mhdr_bytes.end());
    full.insert(full.end(), payload.begin(), payload.end());

    // Send first time
    network.SendMessage(attacker.GetId(), victim.GetId(), full);
    for (int i = 0; i < 5; ++i) network.AdvanceTime(network.GetCurrentTime() + 200);

    // Capture peer_id and initial score
    auto& pm = victim.GetNetworkManager().peer_manager();
    int peer_id = orch.GetPeerId(victim, attacker);
    REQUIRE(peer_id >= 0);
    int score_before = pm.GetMisbehaviorScore(peer_id);

    // Re-send the exact same header
    network.SendMessage(attacker.GetId(), victim.GetId(), full);
    for (int i = 0; i < 5; ++i) network.AdvanceTime(network.GetCurrentTime() + 200);

    // Assert still connected and score unchanged (no penalty for duplicate valid header)
    CHECK(victim.GetPeerCount() == 1);
    int score_after = pm.GetMisbehaviorScore(peer_id);
    CHECK(score_after == score_before);
    CHECK_FALSE(victim.IsBanned(attacker.GetAddress()));
}
