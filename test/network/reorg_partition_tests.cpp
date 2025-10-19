// Copyright (c) 2024 Coinbase Chain
// Reorg, partition, network conditions, and scale tests

#include "network_test_helpers.hpp"

using namespace coinbasechain::test;

// ==============================================================================
// REORG, PARTITION & SCALE TESTS
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

TEST_CASE("ScaleTest - ThousandNodeStressTest", "[.][scaletest][network]") {
    // This test verifies the harness can handle 1000+ nodes
    // Disabled by default (slow) - hidden from default runs with [.] tag
    // Run explicitly with: ./coinbasechain_tests "[scaletest]"

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

