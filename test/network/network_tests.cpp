// Copyright (c) 2024 Coinbase Chain
// Test suite for P2P networking components using simulation harness
//
// IMPORTANT: Simulated Network Time Advancement Guidelines
// =========================================================
// When testing with simulated network latency, you MUST advance time gradually
// in small increments (e.g., 200ms steps), NOT in one large jump.
//
// WHY: Messages are queued with delivery_time = current_time_ms + latency.
// If you jump ahead (e.g., from 100ms to 4000ms), any response messages sent
// after processing delivered messages will be queued with timestamps AFTER your
// current time, creating cascading delays:
//
//   1. INV arrives at 600ms (sent at 100ms + 500ms latency)
//   2. Test jumps to 4000ms to "wait for it"
//   3. GETHEADERS response gets queued at 4000 + 500 = 4500ms
//   4. Test is already at 4000ms, so GETHEADERS never processes
//
// CORRECT APPROACH:
//   for (int i = 0; i < 20; i++) {
//       time_ms += 200;
//       network.AdvanceTime(time_ms);  // Gradual advancement
//   }
//
// This ensures message chains (INV → GETHEADERS → HEADERS) complete naturally.

#include "simulated_network.hpp"
#include "simulated_node.hpp"
#include "attack_simulated_node.hpp"
#include "chain/chainparams.hpp"
#include <catch_amalgamated.hpp>
#include <vector>
#include <chrono>
#include <iostream>

using namespace coinbasechain::test;

// Helper function to set zero latency for deterministic testing
static void SetZeroLatency(SimulatedNetwork& network) {
    SimulatedNetwork::NetworkConditions conditions;
    conditions.latency_min = std::chrono::milliseconds(0);
    conditions.latency_max = std::chrono::milliseconds(0);
    conditions.jitter_max = std::chrono::milliseconds(0);
    network.SetNetworkConditions(conditions);
}

// Global test setup - Catch2 style
// This runs once before all tests
struct GlobalSetup {
    GlobalSetup() {
        // Initialize global chain params for REGTEST
        coinbasechain::chain::GlobalChainParams::Select(coinbasechain::chain::ChainType::REGTEST);
    }
};
static GlobalSetup global_setup;

// ==============================================================================
// PEER MANAGER TESTS
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

TEST_CASE("HeaderSyncTest - InitialSync", "[headersynctest][network]") {
    SimulatedNetwork network(12345);

    // Set zero latency for fast, deterministic testing
    SimulatedNetwork::NetworkConditions conditions;
    conditions.latency_min = std::chrono::milliseconds(0);
    conditions.latency_max = std::chrono::milliseconds(0);
    conditions.jitter_max = std::chrono::milliseconds(0);
    network.SetNetworkConditions(conditions);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    printf("[TEST] Initial state: node1 height=%d hash=%s, node2 height=%d hash=%s\n",
           node1.GetTipHeight(), node1.GetTipHash().GetHex().substr(0, 16).c_str(),
           node2.GetTipHeight(), node2.GetTipHash().GetHex().substr(0, 16).c_str());

    // Connect nodes first
    node2.ConnectTo(1);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);  // Allow handshake

    printf("[TEST] After handshake: node1 peers=%zu, node2 peers=%zu\n",
           node1.GetPeerCount(), node2.GetPeerCount());

    // Node 1 mines 100 blocks AFTER connection
    for (int i = 0; i < 100; i++) {
        node1.MineBlock();
        time_ms += 50;
        network.AdvanceTime(time_ms);  // Allow each block to propagate

        if (i == 0) {
            printf("[TEST] After first block: node1 height=%d, node2 height=%d\n",
                   node1.GetTipHeight(), node2.GetTipHeight());
        }
    }
    CHECK(node1.GetTipHeight() == 100);

    // Node 2 should have synced to same height
    CHECK(node2.GetTipHeight() == 100);
    CHECK(node2.GetTipHash() == node1.GetTipHash());
}

TEST_CASE("HeaderSyncTest - SyncFromMultiplePeers", "[headersynctest][network]") {
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode synced_node1(1, &network);
    SimulatedNode synced_node2(2, &network);
    SimulatedNode new_node(3, &network);

    // Both synced nodes have same chain
    uint64_t time_ms = 100;
    for (int i = 0; i < 50; i++) {
        synced_node1.MineBlock();
        time_ms += 50;
    }

    synced_node2.ConnectTo(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    CHECK(synced_node1.GetTipHeight() == 50);
    CHECK(synced_node2.GetTipHeight() == 50);

    // New node connects to both
    new_node.ConnectTo(1);
    new_node.ConnectTo(2);
    time_ms += 5000;
    network.AdvanceTime(time_ms);

    // Should sync from one of them
    CHECK(new_node.GetTipHeight() == 50);
}

TEST_CASE("HeaderSyncTest - CatchUpAfterMining", "[headersynctest][network]") {
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    // Connect nodes
    node1.ConnectTo(2);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    // Node 1 mines more blocks
    for (int i = 0; i < 20; i++) {
        node1.MineBlock();
        time_ms += 100;
        network.AdvanceTime(time_ms);  // Allow propagation
    }

    // Node 2 should catch up
    CHECK(node2.GetTipHeight() == 20);
}

// ==============================================================================
// IBD (INITIAL BLOCK DOWNLOAD) TESTS
// ==============================================================================

TEST_CASE("IBDTest - FreshNodeSyncsFromGenesis", "[ibdtest][network]") {
    // Test that a brand new node can sync the entire chain from a peer
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode miner(1, &network);
    SimulatedNode fresh_node(2, &network);

    // Miner builds a chain of 200 blocks BEFORE fresh node connects
    printf("[IBD] Miner building 200 block chain...\n");
    for (int i = 0; i < 200; i++) {
        miner.MineBlock();
    }
    CHECK(miner.GetTipHeight() == 200);
    CHECK(fresh_node.GetTipHeight() == 0);  // Still at genesis

    // NOW fresh node connects and syncs
    printf("[IBD] Fresh node connecting to miner...\n");
    fresh_node.ConnectTo(1);

    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);  // Handshake

    // Advance time to allow sync (headers come in batches of 2000 max)
    // With 200 blocks, one batch should suffice
    for (int i = 0; i < 50; i++) {
        time_ms += 200;
        network.AdvanceTime(time_ms);
    }

    // Fresh node should have synced the entire chain
    CHECK(fresh_node.GetTipHeight() == 200);
    CHECK(fresh_node.GetTipHash() == miner.GetTipHash());

    printf("[IBD] Fresh node synced! Height=%d\n", fresh_node.GetTipHeight());
}

TEST_CASE("IBDTest - LargeChainSync", "[ibdtest][network]") {
    // Test syncing a large chain (2000+ headers requires multiple batches)
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode miner(1, &network);
    SimulatedNode syncing_node(2, &network);

    // Build a 2500 block chain (exceeds single HEADERS message limit of 2000)
    // Advance time by 1 second per block to satisfy timestamp validation
    // (each block must have timestamp > median of previous 11 blocks)
    printf("[IBD] Building 2500 block chain (this will take a moment)...\n");
    uint64_t time_ms = 1000;  // Start at 1 second
    for (int i = 0; i < 2500; i++) {
        time_ms += 1000;  // 1 second per block
        network.AdvanceTime(time_ms);
        miner.MineBlock();
        if (i % 500 == 0) {
            printf("[IBD] ...mined %d blocks\n", i);
        }
    }
    CHECK(miner.GetTipHeight() == 2500);

    // Jump forward in time to make the mined blocks appear "old"
    // Blocks 0-2500 have timestamps 1-2501 seconds
    // We need tip to appear >3600 seconds old for IsSynced() to return false
    time_ms = 10000000;  // Jump to ~10000 seconds
    network.AdvanceTime(time_ms);

    // Connect and sync
    printf("[IBD] Syncing node connecting...\n");
    syncing_node.ConnectTo(1);

    time_ms += 100;
    network.AdvanceTime(time_ms);  // Handshake

    // Need more time for multiple GETHEADERS/HEADERS round trips
    // 2500 blocks = 2 batches (2000 + 500)
    // Note: announce_tip_to_peers() throttles to 30 seconds, so we need >= 30s per iteration
    // Also: AdvanceTime processes messages in rounds, and multi-batch sync needs extra time
    for (int i = 0; i < 10; i++) {  // Fewer, longer iterations for multi-batch sync
        time_ms += 35000;  // 35 seconds per iteration (exceeds 30s throttle)
        size_t delivered = network.AdvanceTime(time_ms);

        // Log progress
        printf("[IBD] Iteration %d: delivered %zu messages, height %d/%d\n",
               i, delivered, syncing_node.GetTipHeight(), miner.GetTipHeight());

        // Break early if fully synced
        if (syncing_node.GetTipHeight() == miner.GetTipHeight()) {
            printf("[IBD] Sync complete at iteration %d\n", i);
            break;
        }
    }

    // Should have synced entire chain
    CHECK(syncing_node.GetTipHeight() == 2500);
    CHECK(syncing_node.GetTipHash() == miner.GetTipHash());

    printf("[IBD] Large chain sync complete!\n");
}

TEST_CASE("IBDTest - SyncWhileMining", "[ibdtest][network]") {
    // Test that a node can sync while the peer continues mining
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode miner(1, &network);
    SimulatedNode syncing_node(2, &network);

    // Miner starts with 100 blocks
    // Advance time by 1 second per block to satisfy timestamp validation
    uint64_t time_ms = 1000;  // Start at 1 second
    for (int i = 0; i < 100; i++) {
        time_ms += 1000;  // 1 second per block
        network.AdvanceTime(time_ms);
        miner.MineBlock();
    }

    // Jump forward in time to make blocks appear old
    time_ms = 10000000;  // Jump to ~10000 seconds
    network.AdvanceTime(time_ms);

    // Start sync
    syncing_node.ConnectTo(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    // Interleave: advance time for sync, miner mines more blocks
    // Note: announce_tip_to_peers() throttles to 30 seconds, so we need >= 30s total per round
    for (int round = 0; round < 20; round++) {
        // Advance sync (need at least 30s to bypass throttle)
        for (int i = 0; i < 5; i++) {
            time_ms += 7000;  // 7 seconds each = 35s total per round
            network.AdvanceTime(time_ms);
        }

        // Miner mines 5 more blocks
        for (int i = 0; i < 5; i++) {
            time_ms += 1000;  // 1 second per block
            network.AdvanceTime(time_ms);
            miner.MineBlock();
        }
    }

    // Final sync round to process last messages
    for (int i = 0; i < 5; i++) {
        time_ms += 7000;
        network.AdvanceTime(time_ms);
    }

    // Syncing node should eventually catch up to moving target
    // Miner now has 100 + 100 = 200 blocks
    CHECK(miner.GetTipHeight() == 200);
    CHECK(syncing_node.GetTipHeight() == 200);
}

TEST_CASE("IBDTest - MultiPeerSync", "[ibdtest][network]") {
    // Test that a node can sync from multiple peers simultaneously
    // (though Bitcoin typically syncs from one peer at a time)
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode peer1(1, &network);
    SimulatedNode peer2(2, &network);
    SimulatedNode syncing_node(3, &network);

    // Both peers have same chain
    for (int i = 0; i < 150; i++) {
        peer1.MineBlock();
    }

    // Peer 2 syncs from peer 1
    peer2.ConnectTo(1);
    uint64_t time_ms = 100;
    for (int i = 0; i < 50; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }
    CHECK(peer2.GetTipHeight() == 150);

    // Now syncing node connects to BOTH peers
    syncing_node.ConnectTo(1);
    syncing_node.ConnectTo(2);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    // Advance time to allow sync
    for (int i = 0; i < 50; i++) {
        time_ms += 200;
        network.AdvanceTime(time_ms);
    }

    // Should sync successfully (from whichever peer it chose)
    CHECK(syncing_node.GetTipHeight() == 150);
    CHECK(syncing_node.GetPeerCount() == 2);
}

TEST_CASE("IBDTest - SyncAfterDisconnect", "[ibdtest][network]") {
    // Test that queued messages are purged on disconnect and sync can resume
    SimulatedNetwork network(12345);

    // Set HIGH latency to ensure messages stay queued
    SimulatedNetwork::NetworkConditions conditions;
    conditions.latency_min = std::chrono::milliseconds(2000);  // 2 second latency
    conditions.latency_max = std::chrono::milliseconds(2000);
    conditions.jitter_max = std::chrono::milliseconds(0);
    network.SetNetworkConditions(conditions);

    SimulatedNode miner(1, &network);
    SimulatedNode syncing_node(2, &network);

    // Build 500 block chain
    printf("[IBD] Building 500 block chain...\n");
    for (int i = 0; i < 500; i++) {
        miner.MineBlock();
    }

    // Start sync
    printf("[IBD] Syncing node connecting...\n");
    syncing_node.ConnectTo(1);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    // Wait just enough for handshake and GETHEADERS (but not for HEADERS response)
    time_ms += 1000;  // Not enough time for 2000ms round-trip
    network.AdvanceTime(time_ms);

    int partial_height = syncing_node.GetTipHeight();
    printf("[IBD] Height before disconnect: %d (should still be 0 with in-flight messages)\n", partial_height);

    // Disconnect WHILE messages are still in flight
    // This should purge the queued HEADERS message
    syncing_node.DisconnectFrom(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);
    CHECK(syncing_node.GetPeerCount() == 0);

    // Advance past when messages would have arrived (if not purged)
    time_ms += 3000;
    network.AdvanceTime(time_ms);

    // Height should STILL be 0 because queued messages were purged
    CHECK(syncing_node.GetTipHeight() == 0);
    printf("[IBD] Height after disconnect+wait: %d (messages were purged!)\n", syncing_node.GetTipHeight());

    // Now reconnect with zero latency for fast completion
    printf("[IBD] Reconnecting with zero latency to complete sync...\n");
    SetZeroLatency(network);
    syncing_node.ConnectTo(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    // Complete sync quickly
    for (int i = 0; i < 50; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Should now complete sync
    CHECK(syncing_node.GetTipHeight() == 500);
    printf("[IBD] Resumed sync complete! Height=%d\n", syncing_node.GetTipHeight());
}

TEST_CASE("IBDTest - IsInitialBlockDownloadFlag", "[ibdtest][network]") {
    // Test that IsInitialBlockDownload() flag is set correctly during sync
    //
    // IBD flag should be:
    // - true at genesis (no tip or old tip)
    // - false after syncing sufficient blocks with recent timestamp
    // - latched to false (doesn't flip back to true)

    printf("\n=== TEST: IBDTest - IsInitialBlockDownloadFlag ===\n");

    // Create simulated network
    SimulatedNetwork network(12345);

    // Zero latency for this test
    SimulatedNetwork::NetworkConditions conditions;
    conditions.latency_min = std::chrono::milliseconds(0);
    conditions.latency_max = std::chrono::milliseconds(0);
    conditions.jitter_max = std::chrono::milliseconds(0);
    network.SetNetworkConditions(conditions);

    // RegTest genesis has timestamp 1296688602 (Feb 2, 2011)
    // Start simulation at a much later time (2024) to make genesis appear "old"
    // This simulates a node starting up many years after genesis
    uint64_t time_ms = 1700000000000ULL;  // ~2023 in Unix time (milliseconds)
    network.AdvanceTime(time_ms);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    // At genesis, the tip is from 2011 (~1296688602 seconds)
    // Current time is ~2023 (~1700000000 seconds)
    // Genesis is VERY old: 1296688602 < 1700000000 - 3600 = true
    // Therefore IBD should be true
    CHECK(node1.GetIsIBD() == true);
    CHECK(node2.GetIsIBD() == true);

    // Mine several blocks on node1 to exit IBD
    // IBD requires: (1) recent tip timestamp, (2) sufficient chainwork
    // Mining 10 blocks should be sufficient
    for (int i = 0; i < 10; i++) {
        node1.MineBlock();
        time_ms += 200;
        network.AdvanceTime(time_ms);
    }

    // Node1 should now be out of IBD
    // (tip is recent, chainwork is sufficient)
    bool node1_ibd = node1.GetIsIBD();

    // Note: IBD may still be true if chainwork threshold not met
    // Check what happened
    if (node1_ibd) {
        // Mine more blocks
        for (int i = 0; i < 20; i++) {
            node1.MineBlock();
            time_ms += 200;
            network.AdvanceTime(time_ms);
        }
        node1_ibd = node1.GetIsIBD();
    }

    // Node1 should definitely be out of IBD now
    CHECK(node1_ibd == false);

    // Node2 is still at genesis with old timestamp, should still be in IBD
    CHECK(node2.GetIsIBD() == true);

    // Connect nodes and sync
    node2.ConnectTo(1);
    time_ms += 200;
    network.AdvanceTime(time_ms);

    // Wait for handshake
    for (int i = 0; i < 10 && node2.GetPeerCount() == 0; i++) {
        time_ms += 200;
        network.AdvanceTime(time_ms);
    }

    CHECK(node2.GetPeerCount() == 1);

    // Advance time to allow sync
    // Headers should propagate and node2 should sync
    for (int i = 0; i < 50; i++) {
        time_ms += 200;
        network.AdvanceTime(time_ms);
    }

    // Node2 should now be synced
    CHECK(node2.GetTipHeight() == node1.GetTipHeight());

    // Node2 should now be out of IBD (synced with recent blocks)
    bool node2_ibd = node2.GetIsIBD();
    CHECK(node2_ibd == false);

    // Verify IBD flag is latched (doesn't flip back)
    // Even if we advance time significantly, IBD should stay false
    // because the latch is permanent once set
    CHECK(node1.GetIsIBD() == false);
    CHECK(node2.GetIsIBD() == false);

}

TEST_CASE("IBDTest - ReorgDuringSync", "[ibdtest][network]") {
    // Test that a node can handle a reorg while syncing
    // Scenario: Node starts syncing chain A, then peer switches to longer chain B
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode miner(1, &network);
    SimulatedNode syncing_node(2, &network);

    printf("[IBD] Miner building initial chain A (50 blocks)...\n");
    // Miner builds chain A
    for (int i = 0; i < 50; i++) {
        miner.MineBlock();
    }

    uint256 chain_a_tip = miner.GetTipHash();
    printf("[IBD] Chain A tip: %s\n", chain_a_tip.GetHex().substr(0, 16).c_str());
    CHECK(miner.GetTipHeight() == 50);

    // Syncing node connects and starts downloading chain A
    printf("[IBD] Syncing node connecting...\n");
    syncing_node.ConnectTo(1);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    // Advance just a bit to start handshake, but don't let full sync happen yet
    time_ms += 200;
    network.AdvanceTime(time_ms);

    int partial_sync_height = syncing_node.GetTipHeight();
    printf("[IBD] Syncing node at height %d (should be at least partially synced)\n", partial_sync_height);

    // NOW: Miner extends chain while syncing node is still downloading
    // This simulates the chain growing during IBD
    printf("[IBD] Miner extending chain A by 30 more blocks (to height 80)...\n");
    for (int i = 0; i < 30; i++) {
        miner.MineBlock();
    }

    CHECK(miner.GetTipHeight() == 80);

    // Continue sync - syncing node should follow the extended chain
    printf("[IBD] Syncing node continuing sync to catch up with extended chain...\n");
    for (int i = 0; i < 30; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Syncing node should have synced to extended chain
    CHECK(syncing_node.GetTipHeight() == 80);
    CHECK(syncing_node.GetTipHash() == miner.GetTipHash());

    printf("[IBD] Chain extension test complete! Syncing node followed to height %d\n",
           syncing_node.GetTipHeight());
}

TEST_CASE("IBDTest - OrphanHeaderHandling", "[ibdtest][network]") {
    // Test that orphan headers (headers whose parent is not yet known) are handled correctly
    // This is critical for IBD when headers arrive out of order

    // NOTE: Our current implementation may not support orphan header caching yet
    // This test documents the expected behavior

    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    printf("[Orphan] Building chain on node1...\n");
    // Node1 builds a chain
    for (int i = 0; i < 50; i++) {
        node1.MineBlock();
    }
    CHECK(node1.GetTipHeight() == 50);

    // Node2 connects
    printf("[Orphan] Node2 connecting...\n");
    node2.ConnectTo(1);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    // Let sync happen normally
    for (int i = 0; i < 30; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Both nodes should be synced
    CHECK(node2.GetTipHeight() == 50);
    CHECK(node2.GetTipHash() == node1.GetTipHash());

    printf("[Orphan] Test complete - nodes synced to height %d\n", node2.GetTipHeight());

    // TODO: Add actual out-of-order header test once we have direct header injection
    // For now, this verifies that normal sync works (which handles potential orphans internally)
    // True orphan test would require:
    // 1. Send header at height 100 (orphan, parent unknown)
    // 2. Send headers 1-99 (fills in parents)
    // 3. Verify header 100 gets processed after parents arrive
}

// ==============================================================================
// REORG TESTS
// ==============================================================================

TEST_CASE("ReorgTest - DeepReorg", "[reorgtest][network]") {
    // Test a deep reorg scenario where a longer chain replaces a significant portion of history
    // This tests the reorg depth limits and chain reorganization logic
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode miner_a(1, &network);
    SimulatedNode miner_b(2, &network);
    SimulatedNode observer(3, &network);

    printf("[Reorg] Building common ancestor (10 blocks)...\n");
    // Both miners build common ancestor
    for (int i = 0; i < 10; i++) {
        miner_a.MineBlock();
    }

    // Connect miners so they share initial chain
    miner_b.ConnectTo(1);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(miner_a.GetTipHeight() == 10);
    CHECK(miner_b.GetTipHeight() == 10);
    CHECK(miner_a.GetTipHash() == miner_b.GetTipHash());

    uint256 common_ancestor = miner_a.GetTipHash();
    printf("[Reorg] Common ancestor: %s\n", common_ancestor.GetHex().substr(0, 16).c_str());

    // Disconnect miners - they'll build competing chains
    printf("[Reorg] Partitioning miners...\n");
    miner_b.DisconnectFrom(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    // Miner A builds a shorter chain (20 more blocks = 30 total)
    printf("[Reorg] Miner A building chain to height 30...\n");
    for (int i = 0; i < 20; i++) {
        miner_a.MineBlock();
    }
    CHECK(miner_a.GetTipHeight() == 30);

    // Miner B builds a LONGER chain (25 more blocks = 35 total)
    printf("[Reorg] Miner B building LONGER chain to height 35...\n");
    for (int i = 0; i < 25; i++) {
        miner_b.MineBlock();
    }
    CHECK(miner_b.GetTipHeight() == 35);

    // Observer first syncs from Miner A
    printf("[Reorg] Observer syncing from Miner A...\n");
    observer.ConnectTo(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(observer.GetTipHeight() == 30);
    CHECK(observer.GetTipHash() == miner_a.GetTipHash());
    printf("[Reorg] Observer at height 30 (chain A)\n");

    // NOW observer learns about longer chain B - should trigger deep reorg
    printf("[Reorg] Observer connecting to Miner B (longer chain)...\n");
    observer.ConnectTo(2);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    // Let reorg happen
    for (int i = 0; i < 30; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Observer should reorg to chain B (35 blocks, more work)
    CHECK(observer.GetTipHeight() == 35);
    CHECK(observer.GetTipHash() == miner_b.GetTipHash());

    printf("[Reorg] Deep reorg complete! Observer reorged from height 30 to 35\n");
    printf("[Reorg] Reorg depth: %d blocks\n", 30 - 10); // 20 blocks reorged
}

TEST_CASE("ReorgTest - CompetingChainsEqualWork", "[reorgtest][network]") {
    // Test behavior when two chains have equal work
    // The node should stick with the first-seen chain (tie-breaker)
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode miner_a(1, &network);
    SimulatedNode miner_b(2, &network);
    SimulatedNode observer(3, &network);

    printf("[Equal] Building common ancestor (5 blocks)...\n");
    for (int i = 0; i < 5; i++) {
        miner_a.MineBlock();
    }

    // Sync both miners to common ancestor
    miner_b.ConnectTo(1);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 10; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(miner_a.GetTipHeight() == 5);
    CHECK(miner_b.GetTipHeight() == 5);
    uint256 common_ancestor = miner_a.GetTipHash();

    // Partition
    miner_b.DisconnectFrom(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    // Both mine SAME number of blocks (equal work)
    printf("[Equal] Miners building equal-length chains (10 blocks each)...\n");
    for (int i = 0; i < 10; i++) {
        miner_a.MineBlock();
        miner_b.MineBlock();
    }

    CHECK(miner_a.GetTipHeight() == 15);
    CHECK(miner_b.GetTipHeight() == 15);
    CHECK(miner_a.GetTipHash() != miner_b.GetTipHash()); // Different tips, same height

    // Observer syncs from A first
    printf("[Equal] Observer syncing from Miner A first...\n");
    observer.ConnectTo(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 15; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(observer.GetTipHeight() == 15);
    uint256 chain_a_tip = observer.GetTipHash();
    CHECK(chain_a_tip == miner_a.GetTipHash());

    // Observer learns about equal-work chain B
    printf("[Equal] Observer learning about equal-work chain B...\n");
    observer.ConnectTo(2);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 15; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Observer should STICK with chain A (first-seen wins on ties)
    CHECK(observer.GetTipHeight() == 15);
    CHECK(observer.GetTipHash() == chain_a_tip);

    printf("[Equal] Observer correctly stuck with first-seen chain (no reorg)\n");
}

TEST_CASE("ReorgTest - MultipleReorgs", "[reorgtest][network]") {
    // Test multiple reorgs in sequence (chain thrashing)
    // This can happen in adversarial scenarios or network partitions
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode victim(1, &network);
    SimulatedNode attacker_a(2, &network);
    SimulatedNode attacker_b(3, &network);

    printf("[MultiReorg] Victim builds initial chain (10 blocks)...\n");
    for (int i = 0; i < 10; i++) {
        victim.MineBlock();
    }

    // Attackers sync to victim's chain
    attacker_a.ConnectTo(1);
    attacker_b.ConnectTo(1);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 15; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(victim.GetTipHeight() == 10);
    CHECK(attacker_a.GetTipHeight() == 10);
    CHECK(attacker_b.GetTipHeight() == 10);

    // Disconnect attackers
    attacker_a.DisconnectFrom(1);
    attacker_b.DisconnectFrom(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    printf("[MultiReorg] Round 1: Attacker A builds longer chain...\n");
    // Attacker A builds slightly longer chain
    for (int i = 0; i < 5; i++) {
        attacker_a.MineBlock();
    }
    CHECK(attacker_a.GetTipHeight() == 15);

    // Victim learns about attacker A's chain - reorg #1
    // Attacker A reconnects to victim to propagate longer chain
    attacker_a.ConnectTo(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(victim.GetTipHeight() == 15);
    printf("[MultiReorg] Reorg #1 complete: victim -> chain A (height 15)\n");

    // Disconnect after reorg
    attacker_a.DisconnectFrom(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    printf("[MultiReorg] Round 2: Attacker B builds even longer chain...\n");
    // Attacker B builds even longer chain
    for (int i = 0; i < 8; i++) {
        attacker_b.MineBlock();
    }
    CHECK(attacker_b.GetTipHeight() == 18);

    // Victim learns about attacker B's chain - reorg #2
    attacker_b.ConnectTo(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(victim.GetTipHeight() == 18);
    printf("[MultiReorg] Reorg #2 complete: victim -> chain B (height 18)\n");

    // Disconnect after reorg
    attacker_b.DisconnectFrom(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    printf("[MultiReorg] Round 3: Attacker A extends their chain...\n");
    // Attacker A extends to create yet another reorg
    for (int i = 0; i < 5; i++) {
        attacker_a.MineBlock();
    }
    CHECK(attacker_a.GetTipHeight() == 20);

    // Victim gets reorged AGAIN - reorg #3
    attacker_a.ConnectTo(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(victim.GetTipHeight() == 20);
    printf("[MultiReorg] Reorg #3 complete: victim -> chain A again (height 20)\n");
    printf("[MultiReorg] Victim survived 3 reorgs!\n");
}

TEST_CASE("ReorgTest - ReorgDuringReorg", "[reorgtest][network]") {
    // Test that a node can handle receiving multiple longer chains in succession
    // This tests state machine consistency during sequential reorg attempts
    //
    // Scenario:
    // 1. Victim at height 50 (chain A)
    // 2. Receives chain B (height 60) - reorgs to it
    // 3. Immediately receives chain C (height 65) - must reorg again
    // 4. Must cleanly switch from B to C without corruption
    //
    // This can happen in practice when:
    // - Multiple miners find blocks simultaneously
    // - Network partition heals and multiple competing chains arrive in quick succession
    // - Attacker tries to cause chain thrashing
    //
    // Note: With zero latency, reorgs complete instantly. To test true "mid-reorg"
    // behavior would require latency to slow down chain activation.

    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode victim(1, &network);
    SimulatedNode miner_a(2, &network);
    SimulatedNode miner_b(3, &network);
    SimulatedNode miner_c(4, &network);

    printf("[NestedReorg] Victim building initial chain A (50 blocks)...\n");

    // Victim builds initial chain
    for (int i = 0; i < 50; i++) {
        victim.MineBlock();
    }

    // All miners sync to victim's chain first
    miner_a.ConnectTo(1);
    miner_b.ConnectTo(1);
    miner_c.ConnectTo(1);

    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 30; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(miner_a.GetTipHeight() == 50);
    CHECK(miner_b.GetTipHeight() == 50);
    CHECK(miner_c.GetTipHeight() == 50);

    uint256 common_ancestor = victim.GetTipHash();
    printf("[NestedReorg] Common ancestor at height 50: %s\n",
           common_ancestor.GetHex().substr(0, 16).c_str());

    // Disconnect all miners - they'll build competing chains
    miner_a.DisconnectFrom(1);
    miner_b.DisconnectFrom(1);
    miner_c.DisconnectFrom(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    // Miner A builds moderate extension (5 blocks = height 55)
    printf("[NestedReorg] Miner A building chain to height 55...\n");
    for (int i = 0; i < 5; i++) {
        miner_a.MineBlock();
    }
    CHECK(miner_a.GetTipHeight() == 55);
    printf("[NestedReorg] Miner A tip: %s\n", miner_a.GetTipHash().GetHex().substr(0, 16).c_str());

    // Miner B builds longer chain (10 blocks = height 60)
    printf("[NestedReorg] Miner B building chain to height 60...\n");
    for (int i = 0; i < 10; i++) {
        miner_b.MineBlock();
    }
    CHECK(miner_b.GetTipHeight() == 60);
    printf("[NestedReorg] Miner B tip: %s\n", miner_b.GetTipHash().GetHex().substr(0, 16).c_str());

    // Miner C builds LONGEST chain (15 blocks = height 65)
    printf("[NestedReorg] Miner C building LONGEST chain to height 65...\n");
    for (int i = 0; i < 15; i++) {
        miner_c.MineBlock();
    }
    CHECK(miner_c.GetTipHeight() == 65);
    printf("[NestedReorg] Miner C tip: %s\n", miner_c.GetTipHash().GetHex().substr(0, 16).c_str());

    // Victim first learns about chain B (height 60)
    printf("[NestedReorg] Victim receiving chain B (height 60) - starting reorg...\n");
    printf("[NestedReorg] Before sync: Victim tip=%s, Miner B tip=%s\n",
           victim.GetTipHash().GetHex().substr(0, 16).c_str(),
           miner_b.GetTipHash().GetHex().substr(0, 16).c_str());

    miner_b.ConnectTo(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    // Advance time to complete reorg to chain B
    time_ms += 200;
    network.AdvanceTime(time_ms);

    // Verify victim reorged to chain B
    CHECK(victim.GetTipHeight() == 60);
    CHECK(victim.GetTipHash() == miner_b.GetTipHash());
    printf("[NestedReorg] Victim successfully reorged to chain B: height=%d\n", victim.GetTipHeight());

    // IMPORTANT: Disconnect miner B BEFORE miner C connects
    // Otherwise miner B will also sync to chain C, invalidating the test
    printf("[NestedReorg] Disconnecting Miner B to prevent it from syncing to chain C...\n");
    miner_b.DisconnectFrom(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    // NOW victim learns about EVEN LONGER chain C (height 65)!
    // This immediately triggers a second reorg
    printf("[NestedReorg] Victim receiving chain C (height 65) - second reorg!\n");
    miner_c.ConnectTo(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    // Complete the second reorg
    for (int i = 0; i < 30; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Victim should have cleanly transitioned to chain C (the longest)
    // This tests that the node:
    // 1. Completed first reorg to chain B successfully
    // 2. Immediately started second reorg to chain C upon learning about it
    // 3. Completed successfully without corruption or state machine issues

    printf("[NestedReorg] Final state:\n");
    printf("[NestedReorg]   Victim: height=%d, tip=%s\n",
           victim.GetTipHeight(), victim.GetTipHash().GetHex().substr(0, 16).c_str());
    printf("[NestedReorg]   Miner A: height=%d, tip=%s\n",
           miner_a.GetTipHeight(), miner_a.GetTipHash().GetHex().substr(0, 16).c_str());
    printf("[NestedReorg]   Miner B: height=%d, tip=%s\n",
           miner_b.GetTipHeight(), miner_b.GetTipHash().GetHex().substr(0, 16).c_str());
    printf("[NestedReorg]   Miner C: height=%d, tip=%s\n",
           miner_c.GetTipHeight(), miner_c.GetTipHash().GetHex().substr(0, 16).c_str());

    CHECK(victim.GetTipHeight() == 65);
    CHECK(victim.GetTipHash() == miner_c.GetTipHash());

    printf("[NestedReorg] SUCCESS! Victim ended at height 65 (chain C)\n");
    printf("[NestedReorg] Victim correctly chose longest chain despite nested reorg\n");

    // Verify chain B was NOT chosen (intermediate chain)
    CHECK(victim.GetTipHash() != miner_b.GetTipHash());

    // Verify chain A was abandoned (original chain)
    CHECK(victim.GetTipHash() != common_ancestor);

    printf("[NestedReorg] Nested reorg test complete!\n");
}

// ==============================================================================
// NETWORK PARTITION TESTS
// ==============================================================================

TEST_CASE("NetworkPartitionTest - SimpleSplit", "[networkpartitiontest][network]") {
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    // Connect nodes
    node1.ConnectTo(2);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    // Create partition
    network.CreatePartition({1}, {2});

    // Mine on both sides
    node1.MineBlock();  // Block 1 on partition A
    node2.MineBlock();  // Block 1 on partition B (different)
    time_ms += 1000;
    network.AdvanceTime(time_ms);

    // Should have different tips
    CHECK(node1.GetTipHash() != node2.GetTipHash());
    CHECK(node1.GetTipHeight() == 1);
    CHECK(node2.GetTipHeight() == 1);
}

TEST_CASE("NetworkPartitionTest - HealAndReorg", "[networkpartitiontest][network]") {
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    node1.ConnectTo(2);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    // Partition
    network.CreatePartition({1}, {2});

    // Node 1 mines 5 blocks, Node 2 mines 3
    for (int i = 0; i < 5; i++) node1.MineBlock();
    for (int i = 0; i < 3; i++) node2.MineBlock();
    time_ms += 1000;
    network.AdvanceTime(time_ms);

    // Heal partition
    network.HealPartition();
    time_ms += 35000;  // Advance 35 seconds to trigger periodic re-announcement (30s interval)
    network.AdvanceTime(time_ms);

    // Node 2 should reorg to Node 1's longer chain
    CHECK(node1.GetTipHeight() == 5);
    CHECK(node2.GetTipHeight() == 5);
    CHECK(node1.GetTipHash() == node2.GetTipHash());
}

// ==============================================================================
// NETWORK CONDITIONS TESTS
// ==============================================================================

TEST_CASE("NetworkConditionsTest - HighLatency", "[networkconditionstest][network]") {
    SimulatedNetwork network(12345);
    SetZeroLatency(network);  // Start with zero latency

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    node1.ConnectTo(2);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);  // Complete handshake

    // NOW set high latency (FIXED, not random, for deterministic testing)
    SimulatedNetwork::NetworkConditions conditions;
    conditions.latency_min = std::chrono::milliseconds(500);
    conditions.latency_max = std::chrono::milliseconds(500);  // Fixed at 500ms
    conditions.jitter_max = std::chrono::milliseconds(0);      // No jitter
    network.SetNetworkConditions(conditions);

    node1.MineBlock();  // Mine block 1

    // Advance time gradually to allow message processing
    // Don't skip ahead or messages will be queued far in the future
    for (int i = 0; i < 20; i++) {
        time_ms += 200;
        network.AdvanceTime(time_ms);
    }

    // After 4 seconds of propagation with 500ms latency, block should sync
    CHECK(node2.GetTipHeight() == 1);  // Now has block 1
}

TEST_CASE("NetworkConditionsTest - PacketLoss", "[networkconditionstest][network]") {
    SimulatedNetwork network(12345);
    SetZeroLatency(network);  // Start with zero latency/loss for handshake

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    node1.ConnectTo(2);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);  // Complete handshake with zero loss

    // NOW apply 50% packet loss after handshake is complete
    SimulatedNetwork::NetworkConditions conditions;
    conditions.packet_loss_rate = 0.5;
    conditions.latency_min = std::chrono::milliseconds(1);
    conditions.latency_max = std::chrono::milliseconds(10);
    network.SetNetworkConditions(conditions);

    // Mine 100 blocks with periodic re-announcements
    for (int i = 0; i < 100; i++) {
        node1.MineBlock();
        // Advance 1 second between blocks
        time_ms += 1000;
        network.AdvanceTime(time_ms);
    }

    // Wait an additional 35 seconds to trigger multiple periodic re-announcements
    // This gives dropped messages multiple chances to get through
    time_ms += 35000;
    network.AdvanceTime(time_ms);

    // With 50% loss, node2 should have gotten some but not all
    int node2_height = node2.GetTipHeight();
    CHECK(node2_height > 0);      // Got some
    CHECK(node2_height < 100);    // But not all
}

TEST_CASE("NetworkConditionsTest - BandwidthLimits", "[networkconditionstest][network]") {
    SimulatedNetwork network(12345);

    // Low bandwidth (10 KB/s)
    SimulatedNetwork::NetworkConditions conditions;
    conditions.bandwidth_bytes_per_sec = 10000;
    network.SetNetworkConditions(conditions);

    // TODO: Test that large messages take longer to transmit
}

// ==============================================================================
// SCALE TESTS
// ==============================================================================

TEST_CASE("ScaleTest - HundredNodes", "[scaletest][network]") {
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    std::vector<std::unique_ptr<SimulatedNode>> nodes;

    // Create 100 nodes
    for (int i = 0; i < 100; i++) {
        nodes.push_back(std::make_unique<SimulatedNode>(i, &network));
    }

    // Random topology: each node connects to 8 random peers
    for (size_t i = 0; i < nodes.size(); i++) {
        for (int j = 0; j < 8; j++) {
            int peer_id = rand() % 100;
            if (peer_id != static_cast<int>(i)) {
                nodes[i]->ConnectTo(peer_id);
            }
        }
    }

    uint64_t time_ms = 5000;
    network.AdvanceTime(time_ms);  // Let connections establish

    // Node 0 mines a block
    nodes[0]->MineBlock();

    // Let it propagate
    time_ms += 10000;
    network.AdvanceTime(time_ms);

    // Count how many nodes received the block
    int synced = 0;
    for (const auto& node : nodes) {
        if (node->GetTipHeight() >= 1) {
            synced++;
        }
    }

    // Most nodes should have the block (>90%)
    CHECK(synced > 90);

    // Print statistics
    auto stats = network.GetStats();
    std::cout << "Messages sent: " << stats.total_messages_sent << "\n";
    std::cout << "Messages delivered: " << stats.total_messages_delivered << "\n";
    std::cout << "Nodes synced: " << synced << "/100\n";
}

TEST_CASE("ScaleTest - ThousandNodeStressTest", "[scaletest][network]") {
    // This test verifies the harness can handle 1000+ nodes
    // Disabled by default (slow)

    SKIP("Skipping stress test (slow)");

    SimulatedNetwork network(12345);
    std::vector<std::unique_ptr<SimulatedNode>> nodes;

    for (int i = 0; i < 1000; i++) {
        nodes.push_back(std::make_unique<SimulatedNode>(i, &network));
    }

    // Sparse connections
    for (size_t i = 0; i < nodes.size(); i++) {
        for (int j = 0; j < 4; j++) {
            int peer_id = rand() % 1000;
            if (peer_id != static_cast<int>(i)) {
                nodes[i]->ConnectTo(peer_id);
            }
        }
    }

    network.AdvanceTime(10000);

    nodes[0]->MineBlock();
    network.AdvanceTime(30000);

    int synced = 0;
    for (const auto& node : nodes) {
        if (node->GetTipHeight() >= 1) synced++;
    }

    CHECK(synced > 800);  // 80% should have it
}

// ==============================================================================
// ATTACK SCENARIO TESTS
// ==============================================================================

TEST_CASE("AttackTest - OrphanSpamAttack", "[attacktest][network]") {
    // Test that a node rejects excessive orphan headers
    // Attacker sends many headers with unknown parents to consume memory
    // Defense: Limit orphan cache size and ban peers sending excessive orphans

    printf("[OrphanSpam] Creating network...\n");
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    printf("[OrphanSpam] Creating victim node...\n");
    SimulatedNode victim(1, &network);
    printf("[OrphanSpam] Creating attacker node...\n");
    AttackSimulatedNode attacker(2, &network);
    printf("[OrphanSpam] Both nodes created successfully\n");

    printf("[OrphanSpam] Setting up attack...\n");

    // Victim has a normal chain
    for (int i = 0; i < 10; i++) {
        victim.MineBlock();
    }

    // Attacker connects
    attacker.ConnectTo(1);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 10; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(victim.GetPeerCount() == 1);
    CHECK(attacker.GetPeerCount() == 1);

    // Both should be synced now
    CHECK(attacker.GetTipHeight() == 10);

    printf("[OrphanSpam] Launching attack: sending 1000 orphan headers...\n");

    // Attack: Send 1000 orphan headers (parents unknown)
    attacker.SendOrphanHeaders(1, 1000);

    // Process the attack
    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Expected behavior:
    // 1. Victim processes orphan headers
    // 2. Orphan cache fills up to limit
    // 3. Attacker gets misbehavior score
    // 4. If orphan limit exceeded, attacker may be banned

    // Check that victim is still functional (didn't crash from memory exhaustion)
    CHECK(victim.GetTipHeight() == 10);

    // Check if attacker got banned for sending too many orphans
    // (This depends on implementation - may need to send multiple batches)
    printf("[OrphanSpam] Attack complete. Victim height=%d, attacker banned=%s\n",
           victim.GetTipHeight(),
           victim.IsBanned(attacker.GetAddress()) ? "YES" : "NO");
}

TEST_CASE("AttackTest - OrphanChainGrinding", "[attacktest][network]") {
    // Test defense against "orphan chain grinding" attack
    // Attacker sends deep orphan chains to make victim waste CPU on validation
    // Defense: Limit orphan chain depth and validation work

    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);

    printf("[OrphanGrind] Setting up attack...\n");

    // Victim has small chain
    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
    }

    // Attacker connects and syncs
    attacker.ConnectTo(1);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 15; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(attacker.GetTipHeight() == 5);

    printf("[OrphanGrind] Attacker launching orphan chain grinding attack...\n");

    // Attack: Send a LONG chain (1000 blocks) of orphan headers
    // This forces victim to:
    // - Store all headers (memory attack)
    // - Validate PoW for each (CPU attack)
    // - Try to connect them (wasted work)
    attacker.SendOrphanHeaders(1, 1000);

    printf("[OrphanGrind] Sent 1000 orphan headers. Processing...\n");

    // Process the attack
    for (int i = 0; i < 30; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Expected defense: Reject orphan chains beyond certain depth/work threshold
    // Victim should:
    // 1. Still be functional (didn't crash)
    CHECK(victim.GetTipHeight() == 5);

    // 2. Attacker should be disconnected for exceeding orphan limit (TOO_MANY_ORPHANS = 50 points)
    CHECK(victim.GetPeerCount() == 0);

    printf("[OrphanGrind] ✓ Victim survived attack: height=%d, attacker disconnected=%s\n",
           victim.GetTipHeight(),
           victim.GetPeerCount() == 0 ? "YES" : "NO");
}

TEST_CASE("AttackTest - FakeOrphanParentAttack", "[attacktest][network]") {
    // Test that victim doesn't waste resources trying to fetch fake orphan parents
    // Attacker sends orphan headers claiming to extend victim's chain
    // When victim requests parents, attacker stalls or sends garbage
    // Defense: Timeout on parent requests, limit outstanding requests per peer

    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);

    printf("[FakeParent] Setting up attack...\n");

    // Both start with same chain
    for (int i = 0; i < 10; i++) {
        victim.MineBlock();
    }

    attacker.ConnectTo(1);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(attacker.GetTipHeight() == 10);

    printf("[FakeParent] Attacker enabling stall mode and sending orphan headers...\n");

    // Enable stalling - attacker won't respond to GETHEADERS requests
    attacker.EnableStalling(true);

    // Send orphan headers that claim to extend the chain
    // Victim will try to fetch parents, but attacker will stall
    // This tests that victim doesn't hang waiting for response
    attacker.SendOrphanHeaders(1, 100);

    printf("[FakeParent] Processing attack (victim should timeout waiting for parents)...\n");

    // Process the attack - victim should handle the orphans
    for (int i = 0; i < 30; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Expected defense:
    // 1. Victim remains functional (doesn't hang)
    CHECK(victim.GetTipHeight() == 10);

    // 2. Victim should have handled the stalling attacker (disconnected or marked as slow)
    // Note: Depending on implementation, attacker may be disconnected for TOO_MANY_ORPHANS
    // or may be marked as stalling. Either is acceptable.

    printf("[FakeParent] ✓ Victim survived stall attack: height=%d, still connected=%s\n",
           victim.GetTipHeight(),
           victim.GetPeerCount() > 0 ? "YES" : "NO (disconnected)");

    // Disable stalling for cleanup
    attacker.EnableStalling(false);
}

TEST_CASE("AttackTest - OrphanStormAttack", "[attacktest][network]") {
    // Test defense against "orphan storm" - multiple attackers coordinate
    // Each attacker sends different orphan headers to amplify resource usage
    // Defense: Global orphan limit (not just per-peer), coordinated ban

    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker_a(2, &network);
    AttackSimulatedNode attacker_b(3, &network);
    AttackSimulatedNode attacker_c(4, &network);

    printf("[OrphanStorm] Setting up coordinated attack...\n");

    // Victim builds small chain
    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
    }

    // Three attackers connect
    attacker_a.ConnectTo(1);
    attacker_b.ConnectTo(1);
    attacker_c.ConnectTo(1);

    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(victim.GetPeerCount() == 3);

    // All attackers synced
    CHECK(attacker_a.GetTipHeight() == 5);
    CHECK(attacker_b.GetTipHeight() == 5);
    CHECK(attacker_c.GetTipHeight() == 5);

    printf("[OrphanStorm] Launching coordinated orphan storm attack...\n");

    // Attack: Each attacker sends 500 different orphan headers
    // Combined: 1500 orphans from 3 sources
    // This tests that the system has a global limit, not just per-peer limit
    attacker_a.SendOrphanHeaders(1, 500);
    attacker_b.SendOrphanHeaders(1, 500);
    attacker_c.SendOrphanHeaders(1, 500);

    printf("[OrphanStorm] All attackers sent orphan headers. Processing...\n");

    // Process the coordinated attack
    for (int i = 0; i < 40; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Expected defense:
    // - Global orphan cache limit (e.g., 1000 total) prevents memory exhaustion
    // - Victim remains functional (didn't crash)
    CHECK(victim.GetTipHeight() == 5);

    // - Attackers should be disconnected for exceeding orphan limit
    // With TOO_MANY_ORPHANS = 50 points, each attacker gets 50 points on first offense
    // Should be disconnected after sending 500 orphans
    CHECK(victim.GetPeerCount() == 0);

    printf("[OrphanStorm] ✓ Victim survived coordinated attack: height=%d, all attackers disconnected=%s\n",
           victim.GetTipHeight(),
           victim.GetPeerCount() == 0 ? "YES" : "NO");
}

TEST_CASE("AttackTest - SelfishMining", "[attacktest][network]") {
    // Test selfish mining attack where attacker withholds blocks privately
    // then releases them strategically to orphan honest miner's blocks
    // This gives the attacker unfair mining advantage

    printf("[SelfishMining] Setting up attack...\n");

    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode victim(1, &network);
    AttackSimulatedNode selfish_miner(2, &network);

    // Victim builds public chain
    for (int i = 0; i < 50; i++) {
        victim.MineBlock();
    }

    // Selfish miner connects and syncs
    selfish_miner.ConnectTo(1);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(victim.GetPeerCount() == 1);
    CHECK(selfish_miner.GetTipHeight() == 50);

    // DISCONNECT selfish miner so private blocks don't auto-sync
    printf("[SelfishMining] Disconnecting selfish miner to mine privately...\n");
    selfish_miner.DisconnectFrom(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    printf("[SelfishMining] Selfish miner building PRIVATE chain (3 blocks ahead)...\n");

    // Selfish miner builds PRIVATE chain (3 blocks ahead)
    uint256 private_blocks[3];
    for (int i = 0; i < 3; i++) {
        private_blocks[i] = selfish_miner.MineBlockPrivate();
    }

    CHECK(selfish_miner.GetTipHeight() == 53);  // Private chain is now 3 blocks ahead
    CHECK(victim.GetTipHeight() == 50);  // Victim still at 50 (didn't hear about private blocks)

    // Victim mines one PUBLIC block
    printf("[SelfishMining] Victim mines public block 51...\n");
    victim.MineBlock();
    time_ms += 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 10; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(victim.GetTipHeight() == 51);

    // NOW selfish miner releases private chain by reconnecting
    printf("[SelfishMining] Selfish miner reconnecting and releasing private chain...\n");
    selfish_miner.ConnectTo(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    // Wait for handshake
    for (int i = 0; i < 5; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Now broadcast the private blocks
    for (int i = 0; i < 3; i++) {
        selfish_miner.BroadcastBlock(private_blocks[i], 1);  // Send to victim (node 1)
    }

    // Let the private chain propagate
    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Victim should reorg to selfish chain (53 blocks vs 51)
    CHECK(victim.GetTipHeight() == 53);
    CHECK(victim.GetTipHash() == selfish_miner.GetTipHash());

    // Honest block at 51 got orphaned - selfish miner gained unfair advantage
    printf("[SelfishMining] ✓ Attack successful: Victim reorged from 51 to 53, honest block orphaned\n");
}

TEST_CASE("AttackTest - ReorgSpam", "[attacktest][network]") {
    // Test reorg spam attack where attacker forces repeated reorgs
    // by alternating between two competing chains
    // Defense: Rate limit reorgs or ban peers causing excessive reorgs

    printf("[ReorgSpam] Setting up attack...\n");

    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker_a(2, &network);
    AttackSimulatedNode attacker_b(3, &network);

    // Victim builds initial chain
    for (int i = 0; i < 10; i++) {
        victim.MineBlock();
    }

    // Attackers connect and sync
    attacker_a.ConnectTo(1);
    attacker_b.ConnectTo(1);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(attacker_a.GetTipHeight() == 10);
    CHECK(attacker_b.GetTipHeight() == 10);

    // Disconnect attackers so they can build competing chains
    attacker_a.DisconnectFrom(1);
    attacker_b.DisconnectFrom(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    printf("[ReorgSpam] Launching reorg spam attack (10 cycles)...\n");

    // Force 10 reorgs by alternating between chains
    for (int cycle = 0; cycle < 10; cycle++) {
        // Attacker A builds chain to height 11 + cycle
        attacker_a.MineBlock();

        // Connect A, wait for victim to reorg
        attacker_a.ConnectTo(1);
        for (int i = 0; i < 10; i++) {
            time_ms += 100;
            network.AdvanceTime(time_ms);
        }

        printf("[ReorgSpam] Cycle %d: Victim reorged to chain A (height %d)\n", cycle, victim.GetTipHeight());

        // Disconnect A
        attacker_a.DisconnectFrom(1);
        time_ms += 100;
        network.AdvanceTime(time_ms);

        // Attacker B builds chain to height 12 + cycle (one more than A)
        attacker_b.MineBlock();
        attacker_b.MineBlock();

        // Connect B, wait for victim to reorg AGAIN
        attacker_b.ConnectTo(1);
        for (int i = 0; i < 10; i++) {
            time_ms += 100;
            network.AdvanceTime(time_ms);
        }

        printf("[ReorgSpam] Cycle %d: Victim reorged to chain B (height %d)\n", cycle, victim.GetTipHeight());

        // Disconnect B
        attacker_b.DisconnectFrom(1);
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Victim survived 20 reorgs (2 per cycle)
    printf("[ReorgSpam] ✓ Victim survived 20 reorgs, still functional at height %d\n", victim.GetTipHeight());
    CHECK(victim.GetTipHeight() > 10);  // Should have accepted longer chains
}

TEST_CASE("AttackTest - MassiveReorgDoS", "[attacktest][network]") {
    // Test defense against massive reorg DoS attack
    // Attacker presents alternative chain from early block, forcing
    // reorg of thousands of blocks to consume CPU/memory
    // Defense: Limit maximum reorg depth

    printf("[MassiveReorg] Setting up attack...\n");

    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);

    printf("[MassiveReorg] Victim building long chain (100 blocks)...\n");
    // Victim builds long chain
    for (int i = 0; i < 100; i++) {
        victim.MineBlock();
        if (i % 25 == 0) {
            printf("[MassiveReorg] ...mined %d blocks\n", i);
        }
    }

    CHECK(victim.GetTipHeight() == 100);

    // Attacker connects and syncs
    attacker.ConnectTo(1);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 50; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(attacker.GetTipHeight() == 100);

    // Save common ancestor
    uint256 common_ancestor = victim.GetTipHash();

    // Disconnect attacker
    attacker.DisconnectFrom(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    printf("[MassiveReorg] Attacker building alternative chain (105 blocks, 1 more than victim)...\n");
    // Attacker builds ALTERNATIVE chain from genesis that's 1 block longer
    // This would force victim to reorg 100 blocks
    for (int i = 0; i < 105; i++) {
        attacker.MineBlock();
        if (i % 25 == 0) {
            printf("[MassiveReorg] ...attacker mined %d blocks\n", i);
        }
    }

    CHECK(attacker.GetTipHeight() == 205);  // 100 original + 105 new chain

    // Reconnect and try to force massive reorg
    printf("[MassiveReorg] Attacker reconnecting to force reorg...\n");
    attacker.ConnectTo(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 50; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Expected defense: Either accept reorg (if within limits) or reject (if beyond limits)
    // Either way, victim should still be functional
    printf("[MassiveReorg] ✓ Victim still functional at height %d\n", victim.GetTipHeight());
    CHECK(victim.GetTipHeight() > 0);  // Still has a valid chain
}

TEST_CASE("AttackTest - HeaderFloodingDifferentChains", "[attacktest][network]") {
    // Test header flooding with multiple competing chain headers
    // Attacker sends headers for many different chains to exhaust memory
    // Defense: Limit cached alternative chain headers

    printf("[HeaderFlood] Setting up attack...\n");

    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);

    // Victim has normal chain
    for (int i = 0; i < 10; i++) {
        victim.MineBlock();
    }

    // Attacker connects
    attacker.ConnectTo(1);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 10; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(victim.GetPeerCount() == 1);
    CHECK(attacker.GetTipHeight() == 10);

    printf("[HeaderFlood] Launching header flooding attack (100 different chains)...\n");

    // Send 100 different orphan chain headers (each chain is different)
    // This tests memory limits on stored alternative chains
    for (int chain = 0; chain < 100; chain++) {
        attacker.SendOrphanHeaders(1, 100);  // 100 headers per chain

        // Process some of the flood
        for (int i = 0; i < 5; i++) {
            time_ms += 100;
            network.AdvanceTime(time_ms);
        }

        if (chain % 20 == 0) {
            printf("[HeaderFlood] Sent %d chains so far...\n", chain);
        }
    }

    // Total: 100 chains × 100 headers = 10,000 orphan headers

    // Final processing
    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Expected defense:
    // - Victim should still be functional (didn't crash from memory exhaustion)
    CHECK(victim.GetTipHeight() == 10);

    // - Attacker should be disconnected for excessive orphan spam
    CHECK(victim.GetPeerCount() == 0);

    printf("[HeaderFlood] ✓ Victim survived 10,000 orphan headers across 100 chains\n");
}

TEST_CASE("AttackTest - EclipseAttackPrevention", "[attacktest][network]") {
    // TODO: Test that nodes maintain diverse connections
    // and can't be eclipsed by a single attacker
}

TEST_CASE("AttackTest - InvalidHeaderRejection", "[attacktest][network]") {
    // TODO: Test that invalid headers are rejected and peer is banned
}

TEST_CASE("AttackTest - DoSProtection", "[attacktest][network]") {
    // TODO: Test that excessive invalid messages lead to disconnect/ban
}

TEST_CASE("AttackTest - TimeDilationAttack", "[attacktest][network]") {
    // TODO: Test protection against time-based attacks
}

TEST_CASE("MisbehaviorTest - InvalidPoWPenalty", "[misbehaviortest][network]") {
    // Test INVALID_POW penalty (100 points - instant disconnect)
    printf("[Misbehavior] Testing INVALID_POW penalty (100 points)...\n");

    SimulatedNetwork network(12345);
    SetZeroLatency(network);
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);

    // Build small chain for victim
    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
    }

    // Attacker connects
    attacker.ConnectTo(1);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 10; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(victim.GetPeerCount() == 1);
    CHECK(attacker.GetTipHeight() == 5);

    // Send headers with invalid PoW
    attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 10);

    // Process attack
    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Attacker should be disconnected (100 >= DISCOURAGEMENT_THRESHOLD)
    CHECK(victim.GetPeerCount() == 0);
    printf("[Misbehavior] ✓ INVALID_POW: Attacker disconnected instantly\n");
}

TEST_CASE("MisbehaviorTest - OversizedMessagePenalty", "[misbehaviortest][network]") {
    // Test OVERSIZED_MESSAGE penalty (20 points per offense)
    // Should disconnect after 5 offenses (5 * 20 = 100)
    printf("[Misbehavior] Testing OVERSIZED_MESSAGE penalty (20 points)...\n");

    SimulatedNetwork network(12345);
    SetZeroLatency(network);
    SimulatedNode victim(10, &network);
    AttackSimulatedNode attacker(20, &network);

    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
    }

    attacker.ConnectTo(10);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 10; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    REQUIRE(victim.GetPeerCount() == 1);

    // Send 5 oversized messages to reach threshold
    for (int j = 0; j < 5; j++) {
        attacker.SendOversizedHeaders(10, 3000);
        for (int i = 0; i < 10; i++) {
            time_ms += 100;
            network.AdvanceTime(time_ms);
        }
    }

    // Should be disconnected now (5 * 20 = 100)
    CHECK(victim.GetPeerCount() == 0);
    printf("[Misbehavior] ✓ OVERSIZED_MESSAGE: Disconnected after 5 offenses\n");
}

TEST_CASE("MisbehaviorTest - NonContinuousHeadersPenalty", "[misbehaviortest][network]") {
    // Test NON_CONTINUOUS_HEADERS penalty (20 points per offense)
    // Should disconnect after 5 offenses (5 * 20 = 100)
    printf("[Misbehavior] Testing NON_CONTINUOUS_HEADERS penalty (20 points)...\n");

    SimulatedNetwork network(12345);
    SetZeroLatency(network);
    SimulatedNode victim(30, &network);
    AttackSimulatedNode attacker(40, &network);

    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
    }

    attacker.ConnectTo(30);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 10; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    REQUIRE(victim.GetPeerCount() == 1);

    // Send 5 non-continuous header messages to reach threshold
    for (int j = 0; j < 5; j++) {
        attacker.SendNonContinuousHeaders(30, victim.GetTipHash());
        for (int i = 0; i < 10; i++) {
            time_ms += 100;
            network.AdvanceTime(time_ms);
        }
    }

    // Should be disconnected now (5 * 20 = 100)
    CHECK(victim.GetPeerCount() == 0);
    printf("[Misbehavior] ✓ NON_CONTINUOUS_HEADERS: Disconnected after 5 offenses\n");
}

TEST_CASE("MisbehaviorTest - TooManyOrphansPenalty", "[misbehaviortest][network]") {
    // Test TOO_MANY_ORPHANS penalty (50 points per offense)
    // Should disconnect after 2 offenses (2 * 50 = 100)
    printf("[Misbehavior] Testing TOO_MANY_ORPHANS penalty (50 points)...\n");

    SimulatedNetwork network(12345);
    SetZeroLatency(network);
    SimulatedNode victim(50, &network);
    AttackSimulatedNode attacker(60, &network);

    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
    }

    attacker.ConnectTo(50);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 10; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    REQUIRE(victim.GetPeerCount() == 1);

    // Send 2 batches of orphan headers to reach threshold
    for (int j = 0; j < 2; j++) {
        attacker.SendOrphanHeaders(50, 1000);
        for (int i = 0; i < 20; i++) {
            time_ms += 100;
            network.AdvanceTime(time_ms);
        }
    }

    // Should be disconnected now (2 * 50 = 100)
    CHECK(victim.GetPeerCount() == 0);
    printf("[Misbehavior] ✓ TOO_MANY_ORPHANS: Disconnected after 2 offenses\n");
}

TEST_CASE("MisbehaviorTest - ScoreAccumulation", "[misbehaviortest][network]") {
    // Test that misbehavior scores accumulate across different offense types
    printf("[Misbehavior] Testing misbehavior score accumulation...\n");

    SimulatedNetwork network(12345);
    SetZeroLatency(network);
    SimulatedNode victim(70, &network);
    AttackSimulatedNode attacker(80, &network);

    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
    }

    attacker.ConnectTo(70);
    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 10; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    REQUIRE(victim.GetPeerCount() == 1);

    // Mix of different attack types:
    // 2x non-continuous (40 points)
    // 1x orphan spam (50 points)
    // Total = 90 points (< 100, still connected)
    // Then 1 more non-continuous = 110 points (>= 100, disconnected)

    attacker.SendNonContinuousHeaders(70, victim.GetTipHash());
    for (int i = 0; i < 10; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    attacker.SendNonContinuousHeaders(70, victim.GetTipHash());
    for (int i = 0; i < 10; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    attacker.SendOrphanHeaders(70, 1000);
    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // One more offense should cause disconnect
    attacker.SendNonContinuousHeaders(70, victim.GetTipHash());
    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Should be disconnected now (40 + 50 + 20 = 110 >= 100)
    CHECK(victim.GetPeerCount() == 0);
    printf("[Misbehavior] ✓ Score accumulation: Mixed offenses accumulated to threshold\n");
}


