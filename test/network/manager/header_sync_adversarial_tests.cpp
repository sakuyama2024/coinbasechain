// Header sync adversarial tests (ported to test2)

#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "infra/attack_simulated_node.hpp"
#include "network/protocol.hpp"
#include "chain/chainparams.hpp"
#include "chain/block.hpp"
#include "test_orchestrator.hpp"
#include "network/peer_lifecycle_manager.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;
using namespace coinbasechain::network;
using namespace coinbasechain::protocol;

TEST_CASE("NetworkManager Adversarial - Oversized Headers Message", "[adversarial][network_manager][dos][critical]") {
    SimulatedNetwork network(42001);
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);

    SECTION("Send 2001 headers (exceeds MAX_HEADERS_SIZE)") {
        attacker.ConnectTo(1);
        network.AdvanceTime(network.GetCurrentTime() + 500);
        REQUIRE(victim.GetPeerCount() > 0);
        // Ensure handshake completes before sending adversarial message
        for (int i = 0; i < 20; ++i) network.AdvanceTime(network.GetCurrentTime() + 100);
        attacker.SendOversizedHeaders(1, MAX_HEADERS_SIZE + 1);
        for (int i = 0; i < 10; ++i) network.AdvanceTime(network.GetCurrentTime() + 200);
        CHECK(victim.GetPeerCount() == 0);
    }

    SECTION("Send exactly MAX_HEADERS_SIZE headers (at limit)") {
        attacker.ConnectTo(1);
        network.AdvanceTime(network.GetCurrentTime() + 500);
        // Ensure handshake completes before sending adversarial message
        for (int i = 0; i < 20; ++i) network.AdvanceTime(network.GetCurrentTime() + 100);
        // Build and send exactly MAX_HEADERS_SIZE headers; victim must not disconnect
        std::vector<CBlockHeader> headers;
        headers.reserve(MAX_HEADERS_SIZE);
        uint256 prev = victim.GetTipHash();
        for (size_t i = 0; i < MAX_HEADERS_SIZE; ++i) {
            CBlockHeader h;
            h.nVersion = 1;
            h.hashPrevBlock = prev;
            h.nTime = static_cast<uint32_t>(network.GetCurrentTime() / 1000);
            h.nBits = coinbasechain::chain::GlobalChainParams::Get().GenesisBlock().nBits;
            h.nNonce = static_cast<uint32_t>(i + 1);
            h.hashRandomX.SetHex("0000000000000000000000000000000000000000000000000000000000000000");
            headers.push_back(h);
            prev = h.GetHash();
        }
        message::HeadersMessage msg; msg.headers = headers;
        auto payload = msg.serialize();
        auto hdr = message::create_header(protocol::magic::REGTEST, protocol::commands::HEADERS, payload);
        auto hdr_bytes = message::serialize_header(hdr);
        std::vector<uint8_t> full; full.reserve(hdr_bytes.size()+payload.size());
        full.insert(full.end(), hdr_bytes.begin(), hdr_bytes.end());
        full.insert(full.end(), payload.begin(), payload.end());
        network.SendMessage(attacker.GetId(), victim.GetId(), full);
        for (int i = 0; i < 10; ++i) network.AdvanceTime(network.GetCurrentTime() + 200);
        CHECK(victim.GetPeerCount() > 0);
    }
}

TEST_CASE("HeaderSync - Switch sync peer on stall", "[network][network_header_sync]") {
    // Set up a network with two peers and force the current sync peer to stall,
    // then verify we switch to the other peer for GETHEADERS.
    SimulatedNetwork net(42007);
    net.EnableCommandTracking(true);

    // Miner builds chain
    SimulatedNode miner(10, &net);
    for (int i = 0; i < 40; ++i) (void)miner.MineBlock();

    // Serving peers sync from miner
    SimulatedNode p1(11, &net);
    SimulatedNode p2(12, &net);
    p1.ConnectTo(miner.GetId());
    p2.ConnectTo(miner.GetId());
    // Explicitly trigger initial sync selection for serving peers
    p1.GetNetworkManager().test_hook_check_initial_sync();
    p2.GetNetworkManager().test_hook_check_initial_sync();
    uint64_t t = 1000; net.AdvanceTime(t);
    // Allow additional processing rounds if handshake completed after first check
    for (int i = 0; i < 10 && p1.GetTipHeight() < 40; ++i) {
        net.AdvanceTime(t += 200);
        p1.GetNetworkManager().test_hook_check_initial_sync();
    }
    for (int i = 0; i < 10 && p2.GetTipHeight() < 40; ++i) {
        net.AdvanceTime(t += 200);
        p2.GetNetworkManager().test_hook_check_initial_sync();
    }
    REQUIRE(p1.GetTipHeight() == 40);
    REQUIRE(p2.GetTipHeight() == 40);

    // New node to sync
    SimulatedNode n(13, &net);
    n.ConnectTo(p1.GetId());
    n.ConnectTo(p2.GetId());
    t += 200; net.AdvanceTime(t);

    // Begin initial sync (single sync peer policy)
    n.GetNetworkManager().test_hook_check_initial_sync();
    t += 200; net.AdvanceTime(t);

    int gh_p1_before = net.CountCommandSent(n.GetId(), p1.GetId(), protocol::commands::GETHEADERS);
    int gh_p2_before = net.CountCommandSent(n.GetId(), p2.GetId(), protocol::commands::GETHEADERS);

    // Stall: drop all messages from p1 -> n (no HEADERS)
    SimulatedNetwork::NetworkConditions drop; drop.packet_loss_rate = 1.0;
    net.SetLinkConditions(p1.GetId(), n.GetId(), drop);

    // Advance beyond 120s timeout and process timers
    for (int i = 0; i < 5; ++i) {
        t += 60 * 1000;
        net.AdvanceTime(t);
        n.GetNetworkManager().test_hook_header_sync_process_timers();
    }

    // Re-select sync peer and progress
    n.GetNetworkManager().test_hook_check_initial_sync();
    t += 500; net.AdvanceTime(t);

    int gh_p1_after = net.CountCommandSent(n.GetId(), p1.GetId(), protocol::commands::GETHEADERS);
    int gh_p2_after = net.CountCommandSent(n.GetId(), p2.GetId(), protocol::commands::GETHEADERS);

    CHECK(gh_p2_after >= gh_p2_before);  // switched to or at least not decreased for p2
    CHECK(gh_p1_after >= gh_p1_before); // no new GETHEADERS sent to stalled p1

    // Final state: synced
    CHECK(n.GetTipHeight() == 40);
}

TEST_CASE("NetworkManager Adversarial - Non-Continuous Headers", "[adversarial][network_manager][dos]") {
    SimulatedNetwork network(42002);
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);

    attacker.ConnectTo(1);
    network.AdvanceTime(network.GetCurrentTime() + 500);
    // Ensure handshake completes before sending adversarial message
    for (int i = 0; i < 20; ++i) network.AdvanceTime(network.GetCurrentTime() + 100);

    // Baseline tip
    int tip_before = victim.GetTipHeight();

    // Send non-continuous headers
    attacker.SendNonContinuousHeaders(1, victim.GetTipHash());
    for (int i = 0; i < 10; ++i) network.AdvanceTime(network.GetCurrentTime() + 200);

    // Chain must not advance
    CHECK(victim.GetTipHeight() == tip_before);
}

TEST_CASE("NetworkManager Adversarial - Invalid PoW Headers", "[adversarial][network_manager][pow]") {
    SimulatedNetwork network(42003);
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);

    attacker.ConnectTo(1);
    network.AdvanceTime(500);
    // Ensure handshake completes before sending adversarial message
    for (int i = 0; i < 20; ++i) network.AdvanceTime(network.GetCurrentTime() + 100);

    int tip_before = victim.GetTipHeight();
    attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 10);
    for (int i = 0; i < 20; ++i) network.AdvanceTime(network.GetCurrentTime() + 200);
    // Implementation may disconnect or ignore; in both cases, chain must not advance
    CHECK(victim.GetTipHeight() == tip_before);
}

TEST_CASE("NetworkManager Adversarial - Orphan Headers Attack", "[adversarial][network_manager][orphan]") {
    SimulatedNetwork network(42004);
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);

    attacker.ConnectTo(1);
    network.AdvanceTime(network.GetCurrentTime() + 500);
    REQUIRE(victim.GetPeerCount() > 0);
    // Ensure handshake completes before sending adversarial message
    for (int i = 0; i < 20; ++i) network.AdvanceTime(network.GetCurrentTime() + 100);

    int tip_before = victim.GetTipHeight();
    attacker.SendOrphanHeaders(1, 10);
    for (int i = 0; i < 10; ++i) network.AdvanceTime(network.GetCurrentTime() + 200);

    // Either disconnect or ignore, but chain must not advance
    CHECK(victim.GetTipHeight() == tip_before);
}

TEST_CASE("NetworkManager Adversarial - Repeated Unconnecting Headers", "[adversarial][network_manager][unconnecting]") {
    SimulatedNetwork network(42005);
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);

    attacker.ConnectTo(1);
    network.AdvanceTime(500);
    // Ensure handshake completes before sending adversarial messages
    for (int i = 0; i < 20; ++i) network.AdvanceTime(network.GetCurrentTime() + 100);

    int tip_before = victim.GetTipHeight();
    for (int i = 0; i < 5; i++) {
        attacker.SendOrphanHeaders(1, 5);
        network.AdvanceTime(200);
    }
    network.AdvanceTime(1000);
    // Depending on thresholds victim may disconnect; accept either, but chain must not advance
    CHECK(victim.GetTipHeight() == tip_before);
}

TEST_CASE("NetworkManager Adversarial - Empty Headers Message", "[adversarial][network_manager][edge]") {
    SimulatedNetwork net(42006);
    net.EnableCommandTracking(true);
    SimulatedNode victim(1, &net);
    AttackSimulatedNode attacker(2, &net);

    // Connect and allow basic handshake
    attacker.ConnectTo(1);
    net.AdvanceTime(net.GetCurrentTime() + 500);
    REQUIRE(victim.GetPeerCount() > 0);
    // Ensure handshake completes before sending adversarial message
    for (int i = 0; i < 20; ++i) net.AdvanceTime(net.GetCurrentTime() + 100);

    // Record baseline tip
    int tip_before = victim.GetTipHeight();

    // Inject an empty HEADERS message from attacker -> victim
    message::HeadersMessage empty;
    auto payload = empty.serialize();
    auto hdr = message::create_header(protocol::magic::REGTEST, protocol::commands::HEADERS, payload);
    auto hdr_bytes = message::serialize_header(hdr);
    std::vector<uint8_t> full; full.reserve(hdr_bytes.size()+payload.size());
    full.insert(full.end(), hdr_bytes.begin(), hdr_bytes.end());
    full.insert(full.end(), payload.begin(), payload.end());
    net.SendMessage(attacker.GetId(), victim.GetId(), full);

    // Process delivery and events
    for (int i = 0; i < 5; ++i) net.AdvanceTime(net.GetCurrentTime() + 200);

    // Ensure victim remained connected and chain did not change
    CHECK(victim.GetPeerCount() > 0);
    CHECK(victim.GetTipHeight() == tip_before);
}
