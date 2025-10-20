// Copyright (c) 2024 Coinbase Chain
// Block Announcement - Edge Case Tests
// Tests for immediate relay, thread safety, and memory management

#include "catch_amalgamated.hpp"
#include "simulated_node.hpp"
#include "simulated_network.hpp"
#include "network/peer.hpp"
#include "network/protocol.hpp"
#include <cstdio>
#include <thread>
#include <vector>

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

TEST_CASE("BlockAnnouncement - Immediate relay vs queued announcement", "[block_announcement][immediate_relay][network]") {
    printf("[BlockAnnouncement] Test: Immediate relay vs queued announcement\n");

    SimulatedNetwork network(77777);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);
    SimulatedNode node3(3, &network);

    // Connect nodes and complete handshakes
    printf("[BlockAnnouncement] Connecting nodes...\n");
    node2.ConnectTo(1);
    node3.ConnectTo(1);
    for (int i = 0; i < 20; i++) {
        network.AdvanceTime(network.GetCurrentTime() + 100);
    }
    CHECK(node1.GetPeerCount() == 2);

    // Test 1: relay_block() - immediate broadcast (bypasses queue)
    printf("[BlockAnnouncement] Testing relay_block() (immediate)...\n");
    uint256 blockA = node1.MineBlock();
    printf("[BlockAnnouncement] node1 mined blockA: %s\n", blockA.GetHex().substr(0, 16).c_str());

    // SimulatedNode::MineBlock() calls relay_block() internally
    // Verify peers' queues are EMPTY (because relay_block bypasses queues)
    size_t queue2_after_relay = GetPeerAnnouncementQueueSize(node1, 2);
    size_t queue3_after_relay = GetPeerAnnouncementQueueSize(node1, 3);
    printf("[BlockAnnouncement] After relay_block(): node2 queue=%zu, node3 queue=%zu\n",
           queue2_after_relay, queue3_after_relay);

    // relay_block() sends immediately, doesn't use queues
    CHECK(queue2_after_relay == 0);
    CHECK(queue3_after_relay == 0);

    // Process events to ensure INV messages are delivered
    network.AdvanceTime(network.GetCurrentTime() + 100);

    // Both peers should have received the block via immediate INV
    // (We can't easily check if INV was received without message interception,
    //  but we verified queues weren't used)

    // Test 2: announce_tip_to_peers() - queued approach
    printf("[BlockAnnouncement] Testing announce_tip_to_peers() (queued)...\n");
    uint256 blockB = node1.MineBlock();
    printf("[BlockAnnouncement] node1 mined blockB: %s\n", blockB.GetHex().substr(0, 16).c_str());

    // Now manually call announce_tip_to_peers() (in addition to relay_block from mining)
    node1.GetNetworkManager().announce_tip_to_peers();

    // Check: Both peers should have blockB in their announcement queues
    size_t queue2_after_announce = GetPeerAnnouncementQueueSize(node1, 2);
    size_t queue3_after_announce = GetPeerAnnouncementQueueSize(node1, 3);
    printf("[BlockAnnouncement] After announce_tip_to_peers(): node2 queue=%zu, node3 queue=%zu\n",
           queue2_after_announce, queue3_after_announce);

    CHECK(queue2_after_announce == 1);
    CHECK(queue3_after_announce == 1);

    // Flush announcements
    node1.GetNetworkManager().flush_block_announcements();

    // Verify queues are now empty after flush
    size_t queue2_after_flush = GetPeerAnnouncementQueueSize(node1, 2);
    size_t queue3_after_flush = GetPeerAnnouncementQueueSize(node1, 3);
    printf("[BlockAnnouncement] After flush: node2 queue=%zu, node3 queue=%zu\n",
           queue2_after_flush, queue3_after_flush);

    CHECK(queue2_after_flush == 0);
    CHECK(queue3_after_flush == 0);

    printf("[BlockAnnouncement] ✓ Immediate relay bypasses queue, queued announcement uses queue\n");
}

TEST_CASE("BlockAnnouncement - Thread safety with concurrent queue access", "[block_announcement][thread_safety][network]") {
    printf("[BlockAnnouncement] Test: Thread safety with concurrent queue access\n");

    SimulatedNetwork network(88888);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    // Connect nodes
    printf("[BlockAnnouncement] Connecting nodes...\n");
    node2.ConnectTo(1);
    for (int i = 0; i < 20; i++) {
        network.AdvanceTime(network.GetCurrentTime() + 100);
    }
    CHECK(node1.GetPeerCount() == 1);

    // Mine several blocks
    printf("[BlockAnnouncement] Mining blocks for concurrent test...\n");
    std::vector<uint256> mined_blocks;
    for (int i = 0; i < 5; i++) {
        uint256 block = node1.MineBlock();
        mined_blocks.push_back(block);
    }

    // Concurrent test: Multiple threads trying to announce and flush simultaneously
    printf("[BlockAnnouncement] Testing concurrent announce + flush operations...\n");

    std::atomic<int> announce_count{0};
    std::atomic<int> flush_count{0};
    std::atomic<bool> test_failed{false};

    auto announce_worker = [&]() {
        for (int i = 0; i < 10; i++) {
            try {
                node1.GetNetworkManager().announce_tip_to_peers();
                announce_count++;
            } catch (...) {
                test_failed = true;
            }
        }
    };

    auto flush_worker = [&]() {
        for (int i = 0; i < 10; i++) {
            try {
                node1.GetNetworkManager().flush_block_announcements();
                flush_count++;
            } catch (...) {
                test_failed = true;
            }
        }
    };

    // Launch 4 threads (2 announcing, 2 flushing)
    std::vector<std::thread> threads;
    threads.emplace_back(announce_worker);
    threads.emplace_back(announce_worker);
    threads.emplace_back(flush_worker);
    threads.emplace_back(flush_worker);

    // Wait for all threads
    for (auto& t : threads) {
        t.join();
    }

    printf("[BlockAnnouncement] Completed: %d announces, %d flushes\n",
           announce_count.load(), flush_count.load());

    // Verify no crashes or data corruption
    CHECK(test_failed == false);
    CHECK(announce_count == 20);  // 2 threads * 10 iterations
    CHECK(flush_count == 20);     // 2 threads * 10 iterations

    // Final state: queue should be either empty (flushed) or have tip (announced after last flush)
    size_t final_queue_size = GetPeerAnnouncementQueueSize(node1, 2);
    printf("[BlockAnnouncement] Final queue size: %zu (valid: 0 or 1)\n", final_queue_size);
    CHECK((final_queue_size == 0 || final_queue_size == 1));

    printf("[BlockAnnouncement] ✓ Thread safety: No crashes or corruption during concurrent access\n");
}

TEST_CASE("BlockAnnouncement - Memory management with disconnect", "[block_announcement][memory][network]") {
    printf("[BlockAnnouncement] Test: Memory management when peer disconnects with queued blocks\n");

    SimulatedNetwork network(99999);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    // Connect nodes
    printf("[BlockAnnouncement] Connecting nodes...\n");
    node2.ConnectTo(1);
    for (int i = 0; i < 20; i++) {
        network.AdvanceTime(network.GetCurrentTime() + 100);
    }
    CHECK(node1.GetPeerCount() == 1);

    // Mine a block and add to announcement queue (but don't flush yet)
    printf("[BlockAnnouncement] Mining block and adding to queue...\n");
    uint256 blockA = node1.MineBlock();
    // Manually add to queue without ProcessEvents() (which would flush)
    node1.GetNetworkManager().announce_tip_to_peers();

    // Verify queue has the block
    size_t queue_before_disconnect = GetPeerAnnouncementQueueSize(node1, 2);
    printf("[BlockAnnouncement] Queue size before disconnect: %zu\n", queue_before_disconnect);
    CHECK(queue_before_disconnect >= 1);  // At least one block queued

    // Disconnect node2 (with queued blocks)
    printf("[BlockAnnouncement] Disconnecting node2 with queued blocks...\n");
    node1.DisconnectFrom(2);

    // Process disconnect events
    for (int i = 0; i < 10; i++) {
        network.AdvanceTime(network.GetCurrentTime() + 100);
    }

    CHECK(node1.GetPeerCount() == 0);

    // Try to flush (should handle disconnected peer gracefully)
    printf("[BlockAnnouncement] Attempting flush after disconnect...\n");
    node1.GetNetworkManager().flush_block_announcements();  // Should not crash

    // Verify no crashes or memory issues
    printf("[BlockAnnouncement] Verifying clean state after disconnect...\n");
    CHECK(node1.GetPeerCount() == 0);

    // Try to announce again (should handle having no peers gracefully)
    node1.GetNetworkManager().announce_tip_to_peers();  // Should not crash
    node1.GetNetworkManager().flush_block_announcements();  // Should not crash

    printf("[BlockAnnouncement] ✓ Memory management: Clean disconnect with queued blocks\n");
    printf("[BlockAnnouncement] Note: Run with ASAN (--sanitize=address) for memory leak verification\n");
}
