// Copyright (c) 2024 Coinbase Chain
// Peer manager and ban manager tests

#include "network_test_helpers.hpp"

using namespace coinbasechain::test;

// ==============================================================================
// PEER MANAGER & BAN MANAGER TESTS
// ==============================================================================

TEST_CASE("PeerManagerTest - BasicHandshake", "[peermanagertest][network]") {
    SimulatedNetwork network(12345);  // Deterministic seed
    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    // Node 1 connects to Node 2
    CHECK(node1.ConnectTo(2));

    // Process messages (handshake: VERSION -> VERACK)
    network.AdvanceTime(100);

    // Both nodes should have 1 peer
    CHECK(node1.GetPeerCount() == 1);
    CHECK(node2.GetPeerCount() == 1);
}

TEST_CASE("PeerManagerTest - MultipleConnections", "[peermanagertest][network]") {
    SimulatedNetwork network(12345);
    std::vector<std::unique_ptr<SimulatedNode>> nodes;

    // Create 5 nodes
    for (int i = 0; i < 5; i++) {
        nodes.push_back(std::make_unique<SimulatedNode>(i, &network));
    }

    // Node 0 connects to all others
    for (int i = 1; i < 5; i++) {
        CHECK(nodes[0]->ConnectTo(i));
    }

    network.AdvanceTime(100);

    // Node 0 should have 4 outbound connections
    CHECK(nodes[0]->GetOutboundPeerCount() == 4);
    CHECK(nodes[0]->GetPeerCount() == 4);

    // Each other node should have 1 inbound connection
    for (int i = 1; i < 5; i++) {
        CHECK(nodes[i]->GetInboundPeerCount() == 1);
    }
}

TEST_CASE("PeerManagerTest - SelfConnectionPrevention", "[peermanagertest][network]") {
    SimulatedNetwork network(12345);
    SimulatedNode node(1, &network);

    // Try to connect to self - should fail
    CHECK_FALSE(node.ConnectTo(1));
    CHECK(node.GetPeerCount() == 0);
}

TEST_CASE("PeerManagerTest - PeerDisconnection", "[peermanagertest][network]") {
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    node1.ConnectTo(2);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    CHECK(node1.GetPeerCount() == 1);

    // Disconnect
    node1.DisconnectFrom(2);
    time_ms += 500;
    network.AdvanceTime(time_ms);

    CHECK(node1.GetPeerCount() == 0);
    CHECK(node2.GetPeerCount() == 0);
}

TEST_CASE("PeerManagerTest - MaxConnectionLimits", "[peermanagertest][network]") {
    SimulatedNetwork network(12345);
    SimulatedNode server(1, &network);  // Will accept connections

    // Try to create 200 connections (should hit limit)
    std::vector<std::unique_ptr<SimulatedNode>> clients;
    int successful_connections = 0;

    for (int i = 0; i < 200; i++) {
        clients.push_back(std::make_unique<SimulatedNode>(100 + i, &network));
        if (clients.back()->ConnectTo(1)) {
            successful_connections++;
        }
    }

    network.AdvanceTime(1000);

    // Should have hit the max inbound limit (125 by default)
    CHECK(server.GetInboundPeerCount() <= 125);
    CHECK(server.GetInboundPeerCount() > 100);  // Should have some connections
}

TEST_CASE("PeerManagerTest - PeerEviction", "[peermanagertest][network]") {
    SimulatedNetwork network(12345);
    SimulatedNode server(1, &network);

    // Fill up to capacity
    std::vector<std::unique_ptr<SimulatedNode>> clients;
    for (int i = 0; i < 126; i++) {  // One more than limit
        clients.push_back(std::make_unique<SimulatedNode>(100 + i, &network));
        clients.back()->ConnectTo(1);
    }

    network.AdvanceTime(1000);

    // Should have evicted some to make room
    size_t final_count = server.GetInboundPeerCount();
    CHECK(final_count <= 125);
}

// ==============================================================================
// BAN MANAGER TESTS
// ==============================================================================

TEST_CASE("BanManTest - BasicBan", "[banmantest][network]") {
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    // Ban node 2's address
    std::string node2_addr = node2.GetAddress();
    node1.Ban(node2_addr);

    CHECK(node1.IsBanned(node2_addr));

    // Try to connect to banned node - should fail
    CHECK_FALSE(node1.ConnectTo(2));
}

TEST_CASE("BanManTest - UnbanAddress", "[banmantest][network]") {
    SimulatedNetwork network(12345);
    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    std::string node2_addr = node2.GetAddress();

    // Ban then unban
    node1.Ban(node2_addr);
    CHECK(node1.IsBanned(node2_addr));

    node1.Unban(node2_addr);
    CHECK_FALSE(node1.IsBanned(node2_addr));

    // Should now be able to connect
    CHECK(node1.ConnectTo(2));
    network.AdvanceTime(100);
    CHECK(node1.GetPeerCount() == 1);
}

TEST_CASE("BanManTest - MisbehaviorBan", "[banmantest][network]") {
    SimulatedNetwork network(12345);
    SimulatedNode honest(1, &network);
    SimulatedNode attacker(2, &network);

    attacker.ConnectTo(1);
    network.AdvanceTime(100);

    CHECK(honest.GetPeerCount() == 1);

    // Attacker sends invalid data
    // TODO: Need to implement SendInvalidHeaders or similar
    // For now, test that ban system is accessible

    honest.Ban(attacker.GetAddress());
    CHECK(honest.IsBanned(attacker.GetAddress()));
}

TEST_CASE("BanManTest - DiscouragementSystem", "[banmantest][network]") {
    // TODO: Test the discourage system (probabilistic rejection)
    // This tests the grey-listing feature for borderline misbehavior
}

// ==============================================================================
// HEADER SYNC TESTS
// ==============================================================================

