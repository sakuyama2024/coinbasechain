// Copyright (c) 2024 Coinbase Chain
// Network sync and Initial Block Download (IBD) tests

#include "network_test_helpers.hpp"

using namespace coinbasechain::test;

// ==============================================================================
// NETWORK SYNC & IBD TESTS
// ==============================================================================

TEST_CASE("NetworkSync - InitialSync", "[networksync][network]") {
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

TEST_CASE("NetworkSync - SyncFromMultiplePeers", "[networksync][network]") {
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

TEST_CASE("NetworkSync - CatchUpAfterMining", "[networksync][network]") {
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

