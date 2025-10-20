// Copyright (c) 2024 Coinbase Chain
// Block Announcement - Peer State Tests
// Tests that only READY peers receive block announcements

#include "catch_amalgamated.hpp"
#include "simulated_node.hpp"
#include "simulated_network.hpp"
#include "network/peer.hpp"
#include "network/protocol.hpp"
#include <cstdio>

using namespace coinbasechain;
using namespace coinbasechain::test;

// Helper to set zero latency for deterministic testing
static void SetZeroLatency(SimulatedNetwork& network) {
    SimulatedNetwork::NetworkConditions conditions;
    conditions.latency_min = std::chrono::milliseconds(0);
    conditions.latency_max = std::chrono::milliseconds(0);
    conditions.jitter_max = std::chrono::milliseconds(0);
    network.SetNetworkConditions(conditions);
}

// Helper to get peer's announcement queue size
static size_t GetPeerAnnouncementQueueSize(SimulatedNode& node, int peer_node_id) {
    auto& peer_mgr = node.GetNetworkManager().peer_manager();
    auto all_peers = peer_mgr.get_all_peers();

    for (const auto& peer : all_peers) {
        if (!peer) continue;
        if (peer->port() == coinbasechain::protocol::ports::REGTEST + peer_node_id) {
            std::lock_guard<std::mutex> lock(peer->block_inv_mutex_);
            return peer->blocks_for_inv_relay_.size();
        }
    }
    return 0;
}

// Helper to get blocks in peer's announcement queue
static std::vector<uint256> GetPeerAnnouncementQueue(SimulatedNode& node, int peer_node_id) {
    auto& peer_mgr = node.GetNetworkManager().peer_manager();
    auto all_peers = peer_mgr.get_all_peers();

    for (const auto& peer : all_peers) {
        if (!peer) continue;
        if (peer->port() == coinbasechain::protocol::ports::REGTEST + peer_node_id) {
            std::lock_guard<std::mutex> lock(peer->block_inv_mutex_);
            return peer->blocks_for_inv_relay_;
        }
    }
    return {};
}

TEST_CASE("BlockAnnouncement - Mixed peer states (READY vs non-READY)", "[block_announcement][peer_states][network]") {
    printf("[BlockAnnouncement] Test: Mixed peer states during announcement\n");

    SimulatedNetwork network(66666);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);
    SimulatedNode node3(3, &network);

    // Connect node2 and let it complete handshake (become READY)
    printf("[BlockAnnouncement] Connecting node2 (will become READY)...\n");
    node2.ConnectTo(1);
    for (int i = 0; i < 20; i++) {
        network.AdvanceTime(network.GetCurrentTime() + 100);
    }
    CHECK(node1.GetPeerCount() == 1);

    // Connect node3 but don't process events (handshake won't complete)
    printf("[BlockAnnouncement] Connecting node3 (handshake incomplete)...\n");
    node3.ConnectTo(1);
    // Don't call AdvanceTime/ProcessEvents - handshake won't complete yet

    // Mine a block on node1
    uint256 blockA = node1.MineBlock();
    printf("[BlockAnnouncement] node1 mined blockA: %s\n", blockA.GetHex().substr(0, 16).c_str());

    // Announce tip to all peers
    node1.GetNetworkManager().announce_tip_to_peers();

    // Check: node2 (READY) should have the block in queue
    size_t queue2 = GetPeerAnnouncementQueueSize(node1, 2);
    printf("[BlockAnnouncement] node2 queue size (READY): %zu\n", queue2);
    CHECK(queue2 == 1);

    // Check: node3 (not READY) should NOT have the block in queue
    // Note: node3 might not even be in peer list yet, or might have empty queue
    size_t queue3 = GetPeerAnnouncementQueueSize(node1, 3);
    printf("[BlockAnnouncement] node3 queue size (not READY): %zu\n", queue3);
    CHECK(queue3 == 0);

    // Now complete node3's handshake
    printf("[BlockAnnouncement] Completing node3 handshake...\n");
    for (int i = 0; i < 20; i++) {
        network.AdvanceTime(network.GetCurrentTime() + 100);
    }

    // Verify node3 is now connected
    CHECK(node1.GetPeerCount() == 2);

    // Mine another block
    uint256 blockB = node1.MineBlock();
    printf("[BlockAnnouncement] node1 mined blockB: %s\n", blockB.GetHex().substr(0, 16).c_str());

    // Announce again
    node1.GetNetworkManager().announce_tip_to_peers();

    // Check: node3 (now READY) should have blockB in queue
    queue3 = GetPeerAnnouncementQueueSize(node1, 3);
    printf("[BlockAnnouncement] node3 queue size (now READY): %zu\n", queue3);
    CHECK(queue3 == 1);

    // Verify it's blockB, not blockA
    auto queue3_blocks = GetPeerAnnouncementQueue(node1, 3);
    REQUIRE(queue3_blocks.size() == 1);
    CHECK(queue3_blocks[0] == blockB);

    printf("[BlockAnnouncement] ✓ Only READY peers receive announcements\n");
}
