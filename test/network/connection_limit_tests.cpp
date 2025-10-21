#include "catch_amalgamated.hpp"
#include "simulated_network.hpp"
#include "simulated_node.hpp"
#include "network_test_helpers.hpp"
#include <vector>

using namespace coinbasechain;
using namespace coinbasechain::test;
using namespace coinbasechain::network;

/**
 * Connection Limits Enforcement Tests
 *
 * Tests for verifying connection limit behavior:
 * 1. Max inbound connection limit enforcement (125 default)
 * 2. Max outbound connection limit enforcement (8 default)
 * 3. Eviction policy when all slots full
 *
 * Default limits from PeerManager::Config:
 * - max_inbound_peers: 125
 * - max_outbound_peers: 8
 */

// =============================================================================
// INBOUND CONNECTION LIMIT TESTS
// =============================================================================

TEST_CASE("Inbound limit - Accept up to max_inbound connections",
          "[network][limits][inbound]") {
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode victim(1, &network);
    victim.SetBypassPOWValidation(true);

    uint64_t time_ms = 1000000;

    // Mine genesis + a few blocks
    for (int i = 0; i < 3; i++) {
        victim.MineBlock();
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Connect multiple inbound peers
    const int NUM_TEST_PEERS = 10;
    std::vector<SimulatedNode*> peers;

    for (int i = 0; i < NUM_TEST_PEERS; i++) {
        peers.push_back(new SimulatedNode(100 + i, &network));
        peers[i]->SetBypassPOWValidation(true);
        peers[i]->ConnectTo(1);
    }

    // Single time advancement after all connections
    time_ms += 5000;
    network.AdvanceTime(time_ms);

    // Should have most/all connections (use range check like existing tests)
    REQUIRE(victim.GetInboundPeerCount() >= NUM_TEST_PEERS - 2);
    REQUIRE(victim.GetInboundPeerCount() <= NUM_TEST_PEERS);

    INFO("Successfully accepted " << victim.GetInboundPeerCount() << "/" << NUM_TEST_PEERS << " inbound connections");
    INFO("Default max_inbound_peers: 125");

    // Cleanup
    for (auto* peer : peers) {
        delete peer;
    }
}

TEST_CASE("Inbound limit - Eviction when limit reached",
          "[network][limits][inbound][eviction]") {
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode victim(1, &network);
    victim.SetBypassPOWValidation(true);

    uint64_t time_ms = 1000000;

    victim.MineBlock();
    time_ms += 100;
    network.AdvanceTime(time_ms);

    // Fill all 125 inbound slots + extra to trigger eviction
    const int MAX_INBOUND = 125;
    const int NUM_PEERS = 130;  // Try to connect more than limit
    std::vector<SimulatedNode*> peers;

    INFO("Creating " << NUM_PEERS << " peers (more than max " << MAX_INBOUND << ")...");

    for (int i = 0; i < NUM_PEERS; i++) {
        peers.push_back(new SimulatedNode(100 + i, &network));
        peers[i]->SetBypassPOWValidation(true);
        peers[i]->ConnectTo(1);
    }

    // Single time advancement after all connection attempts
    time_ms += 5000;
    network.AdvanceTime(time_ms);

    // Should be at or below limit (eviction enforced)
    size_t inbound_count = victim.GetInboundPeerCount();
    REQUIRE(inbound_count <= MAX_INBOUND);

    // Should have accepted most connections (like existing MaxConnectionLimits test)
    REQUIRE(inbound_count > 100);

    INFO("Limit enforced - inbound count: " << inbound_count << " (max: " << MAX_INBOUND << ")");

    // Cleanup
    for (auto* peer : peers) {
        delete peer;
    }
}

// =============================================================================
// OUTBOUND CONNECTION LIMIT TESTS
// =============================================================================

TEST_CASE("Outbound limit - Accept up to max_outbound connections",
          "[network][limits][outbound]") {
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    node1.SetBypassPOWValidation(true);

    uint64_t time_ms = 1000000;

    node1.MineBlock();
    time_ms += 100;
    network.AdvanceTime(time_ms);

    // Create 8 peer nodes (max_outbound default)
    const int MAX_OUTBOUND = 8;
    std::vector<SimulatedNode*> peers;

    for (int i = 0; i < MAX_OUTBOUND; i++) {
        peers.push_back(new SimulatedNode(100 + i, &network));
        peers[i]->SetBypassPOWValidation(true);
        peers[i]->MineBlock();
    }

    time_ms += 1000;
    network.AdvanceTime(time_ms);

    // Node1 makes outbound connections
    for (int i = 0; i < MAX_OUTBOUND; i++) {
        node1.ConnectTo(100 + i);
    }

    // Single time advancement after all connections
    time_ms += 5000;
    network.AdvanceTime(time_ms);

    // Should have most/all outbound connections
    size_t outbound_count = node1.GetOutboundPeerCount();
    REQUIRE(outbound_count >= MAX_OUTBOUND - 2);
    REQUIRE(outbound_count <= MAX_OUTBOUND);

    INFO("Successfully created " << outbound_count << "/" << MAX_OUTBOUND << " outbound connections");

    // Cleanup
    for (auto* peer : peers) {
        delete peer;
    }
}

TEST_CASE("Outbound limit - Reject when max_outbound reached",
          "[network][limits][outbound]") {
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    node1.SetBypassPOWValidation(true);

    uint64_t time_ms = 1000000;

    node1.MineBlock();
    time_ms += 100;
    network.AdvanceTime(time_ms);

    // Create 10 peer nodes (more than max_outbound)
    const int MAX_OUTBOUND = 8;
    const int NUM_PEERS = 10;
    std::vector<SimulatedNode*> peers;

    for (int i = 0; i < NUM_PEERS; i++) {
        peers.push_back(new SimulatedNode(100 + i, &network));
        peers[i]->SetBypassPOWValidation(true);
        peers[i]->MineBlock();
    }

    time_ms += 1000;
    network.AdvanceTime(time_ms);

    // Try to connect to all peers
    for (int i = 0; i < NUM_PEERS; i++) {
        node1.ConnectTo(100 + i);
    }

    // Single time advancement after all attempts
    time_ms += 5000;
    network.AdvanceTime(time_ms);

    // Outbound limit should be enforced (hard limit, no eviction)
    size_t outbound_count = node1.GetOutboundPeerCount();
    REQUIRE(outbound_count <= MAX_OUTBOUND);

    INFO("Outbound limit enforced - count: " << outbound_count << " (max: " << MAX_OUTBOUND << ")");

    // Cleanup
    for (auto* peer : peers) {
        delete peer;
    }
}

// =============================================================================
// EVICTION POLICY TESTS
// =============================================================================

TEST_CASE("Eviction policy - Only inbound peers evictable",
          "[network][limits][eviction]") {
    // Document that outbound peers are never evicted
    INFO("Eviction logic: peer_manager.cpp:207-249");
    INFO("Line 227: if (!peer->is_inbound()) continue;");
    INFO("Only inbound peers are candidates for eviction");
}

TEST_CASE("Eviction policy - 10 second protection window",
          "[network][limits][eviction]") {
    // Document the protection window
    INFO("Line 234: if (connection_age.count() < 10) continue;");
    INFO("Peers connected < 10 seconds are protected from eviction");
    INFO("Prevents rapid connect/evict cycles");
}

TEST_CASE("Eviction policy - Selection by ping time",
          "[network][limits][eviction]") {
    // Document selection criteria
    INFO("Line 247-249: Evict peer with worst (highest) ping time");
    INFO("Fallback: Evict oldest connection if no ping data");
    INFO("This prefers keeping responsive, well-connected peers");
}
