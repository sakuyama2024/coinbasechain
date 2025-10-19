// Copyright (c) 2024 Coinbase Chain
// Functional tests for invalidateblock RPC using network test harness

#include "simulated_network.hpp"
#include "simulated_node.hpp"
#include "chain/chainparams.hpp"
#include <catch_amalgamated.hpp>
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

// Global test setup - ensure REGTEST params are selected
struct InvalidateBlockTestSetup {
    InvalidateBlockTestSetup() {
        coinbasechain::chain::GlobalChainParams::Select(coinbasechain::chain::ChainType::REGTEST);
    }
};
static InvalidateBlockTestSetup invalidate_block_test_setup;

TEST_CASE("InvalidateBlock - Basic invalidation with reorg", "[invalidateblock][functional][network]") {
    printf("[InvalidateBlock] Test: Basic invalidation with reorg\n");

    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    // Node 1 builds a chain: genesis -> A -> B -> C
    printf("[InvalidateBlock] Node 1 building chain A->B->C...\n");
    uint256 blockA = node1.MineBlock();
    uint256 blockB = node1.MineBlock();
    uint256 blockC = node1.MineBlock();

    network.AdvanceTime(network.GetCurrentTime() + 100);

    CHECK(node1.GetTipHeight() == 3);
    CHECK(node1.GetTipHash() == blockC);

    // Node 2 connects and syncs
    printf("[InvalidateBlock] Node 2 connecting and syncing...\n");
    printf("[DEBUG] Before connect: node1_peers=%zu node2_peers=%zu node2_height=%d\n",
           node1.GetPeerCount(), node2.GetPeerCount(), node2.GetTipHeight());
    printf("[DEBUG] Queue stats: sent=%zu delivered=%zu\n",
           network.GetStats().total_messages_sent, network.GetStats().total_messages_delivered);

    node2.ConnectTo(1);
    size_t delivered1 = network.AdvanceTime(network.GetCurrentTime() + 100);
    printf("[DEBUG] After connect+time: delivered=%zu node1_peers=%zu node2_peers=%zu node2_height=%d\n",
           delivered1, node1.GetPeerCount(), node2.GetPeerCount(), node2.GetTipHeight());
    printf("[DEBUG] Queue stats: sent=%zu delivered=%zu\n",
           network.GetStats().total_messages_sent, network.GetStats().total_messages_delivered);

    for (int i = 0; i < 20; i++) {
        size_t delivered = network.AdvanceTime(network.GetCurrentTime() + 100);
        if (delivered > 0 || i == 0 || i == 19) {
            printf("[DEBUG] Round %d: delivered=%zu node2_height=%d node2_peers=%zu\n",
                   i, delivered, node2.GetTipHeight(), node2.GetPeerCount());
        }
    }

    printf("[DEBUG] Final: node2_height=%d (expected 3) node2_peers=%zu\n",
           node2.GetTipHeight(), node2.GetPeerCount());
    CHECK(node2.GetTipHeight() == 3);
    CHECK(node2.GetTipHash() == blockC);
    printf("[InvalidateBlock] Node 2 synced to height 3\n");

    // Disconnect nodes
    node2.DisconnectFrom(1);
    network.AdvanceTime(network.GetCurrentTime() + 100);

    // Node 2 builds a competing fork: A -> D -> E -> F
    printf("[InvalidateBlock] Node 2 building competing fork A->D->E->F...\n");

    // First, invalidate blockB on node2 to rewind to blockA
    bool invalidated = node2.GetChainstate().InvalidateBlock(blockB);
    REQUIRE(invalidated);

    CHECK(node2.GetTipHeight() == 1);  // Should rewind to block A
    CHECK(node2.GetTipHash() == blockA);
    printf("[InvalidateBlock] Node 2 rewound to height 1 after invalidating blockB\n");

    // Now mine new chain
    printf("[InvalidateBlock] Time before mining D,E,F: %lu ms\n", network.GetCurrentTime());
    uint256 blockD = node2.MineBlock();
    uint256 blockE = node2.MineBlock();
    uint256 blockF = node2.MineBlock();

    network.AdvanceTime(network.GetCurrentTime() + 100);
    printf("[InvalidateBlock] Time after mining D,E,F: %lu ms\n", network.GetCurrentTime());

    CHECK(node2.GetTipHeight() == 4);
    printf("[InvalidateBlock] Node 2 built new chain to height 4\n");

    // Reconnect nodes - node 1 should reorg to longer chain
    printf("[InvalidateBlock] Reconnecting nodes...\n");
    printf("[InvalidateBlock] Before reconnect: node1_height=%d node2_height=%d\n",
           node1.GetTipHeight(), node2.GetTipHeight());
    printf("[InvalidateBlock] node1_tip=%s\n", node1.GetTipHash().ToString().substr(0, 16).c_str());
    printf("[InvalidateBlock] node2_tip=%s (blockF=%s)\n",
           node2.GetTipHash().ToString().substr(0, 16).c_str(),
           blockF.ToString().substr(0, 16).c_str());

    node2.ConnectTo(1);

    // Give more time for connection to establish
    for (int i = 0; i < 20; i++) {
        network.AdvanceTime(network.GetCurrentTime() + 100);
    }

    printf("[InvalidateBlock] After reconnect: node1_peers=%zu node2_peers=%zu\n",
           node1.GetPeerCount(), node2.GetPeerCount());

    // Allow time for reorg to complete (block propagation + validation + reorg)
    for (int i = 0; i < 100; i++) {
        size_t delivered = network.AdvanceTime(network.GetCurrentTime() + 100);
        if (i < 5 || i % 10 == 0 || delivered > 0 || i == 99) {
            printf("[InvalidateBlock] Round %d: delivered=%zu node1_height=%d node2_height=%d node1_peers=%zu node2_peers=%zu\n",
                   i, delivered, node1.GetTipHeight(), node2.GetTipHeight(),
                   node1.GetPeerCount(), node2.GetPeerCount());
        }
        if (node1.GetTipHeight() == 4) {
            printf("[InvalidateBlock] Reorg completed at round %d\n", i);
            break;
        }
    }

    // Node 1 should have reorged to node2's longer chain
    printf("[InvalidateBlock] Final: node1_height=%d (expected 4) node1_tip=%s\n",
           node1.GetTipHeight(), node1.GetTipHash().ToString().substr(0, 16).c_str());
    printf("[InvalidateBlock] blockF=%s\n", blockF.ToString().substr(0, 16).c_str());
    CHECK(node1.GetTipHeight() == 4);
    CHECK(node1.GetTipHash() == blockF);
    printf("[InvalidateBlock] ✓ Node 1 reorged to longer chain (height 4)\n");
}

TEST_CASE("InvalidateBlock - Multiple nodes with competing chains", "[invalidateblock][functional][network]") {
    printf("[InvalidateBlock] Test: Multiple nodes with competing chains\n");

    SimulatedNetwork network(54321);
    SetZeroLatency(network);

    SimulatedNode miner1(1, &network);
    SimulatedNode miner2(2, &network);
    SimulatedNode observer(3, &network);

    // Both miners build a common base
    printf("[InvalidateBlock] Miners building common base (height 10)...\n");
    for (int i = 0; i < 10; i++) {
        miner1.MineBlock();
    }

    // Connect and sync
    miner2.ConnectTo(1);
    observer.ConnectTo(1);

    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 30; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(miner1.GetTipHeight() == 10);
    CHECK(miner2.GetTipHeight() == 10);
    CHECK(observer.GetTipHeight() == 10);

    uint256 common_ancestor = miner1.GetTipHash();
    printf("[InvalidateBlock] All nodes synced to height 10\n");

    // Disconnect everyone
    miner2.DisconnectFrom(1);
    observer.DisconnectFrom(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    // Miner1 builds chain A (5 more blocks = height 15)
    printf("[InvalidateBlock] Miner1 building chain A to height 15...\n");
    std::vector<uint256> chainA;
    for (int i = 0; i < 5; i++) {
        chainA.push_back(miner1.MineBlock());
    }

    // Miner2 builds chain B (7 more blocks = height 17)
    printf("[InvalidateBlock] Miner2 building chain B to height 17...\n");
    std::vector<uint256> chainB;
    for (int i = 0; i < 7; i++) {
        chainB.push_back(miner2.MineBlock());
    }

    CHECK(miner1.GetTipHeight() == 15);
    CHECK(miner2.GetTipHeight() == 17);

    // Observer first learns about chain A
    printf("[InvalidateBlock] Observer syncing to chain A...\n");
    observer.ConnectTo(1);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(observer.GetTipHeight() == 15);
    printf("[InvalidateBlock] Observer at height 15 (chain A)\n");

    // Observer now learns about chain B (longer) - should reorg
    printf("[InvalidateBlock] Observer learning about chain B (longer)...\n");
    observer.ConnectTo(2);
    time_ms += 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 30; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    CHECK(observer.GetTipHeight() == 17);
    CHECK(observer.GetTipHash() == miner2.GetTipHash());
    printf("[InvalidateBlock] Observer reorged to chain B (height 17)\n");

    // Now observer invalidates the first block of chain B
    // This should make observer fall back to chain A
    printf("[InvalidateBlock] Observer invalidating first block of chain B...\n");
    bool invalidated = observer.GetChainstate().InvalidateBlock(chainB[0]);
    REQUIRE(invalidated);

    // Observer should rewind below chain B
    CHECK(observer.GetTipHeight() <= 10);
    printf("[InvalidateBlock] Observer rewound to height %d\n", observer.GetTipHeight());

    // Calling ActivateBestChain should switch to chain A (the best valid chain)
    observer.GetChainstate().ActivateBestChain(nullptr);

    // Give time for chain to activate
    time_ms += 100;
    network.AdvanceTime(time_ms);

    // After invalidating chain B, observer should be at height 15
    // It should be on chain A (the blocks we saved in chainA vector)
    CHECK(observer.GetTipHeight() == 15);
    CHECK(observer.GetTipHash() == chainA.back());  // Check against actual chain A tip
    printf("[InvalidateBlock] ✓ Observer switched to chain A after invalidating chain B\n");
}

TEST_CASE("InvalidateBlock - Invalidate and mine new blocks", "[invalidateblock][functional][network]") {
    printf("[InvalidateBlock] Test: Invalidate and mine new blocks\n");

    SimulatedNetwork network(99999);
    SetZeroLatency(network);

    SimulatedNode node(1, &network);

    // Build initial chain: genesis -> A -> B -> C -> D
    printf("[InvalidateBlock] Building initial chain to height 4...\n");
    uint256 blockA = node.MineBlock();
    uint256 blockB = node.MineBlock();
    uint256 blockC = node.MineBlock();
    uint256 blockD = node.MineBlock();

    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    CHECK(node.GetTipHeight() == 4);

    // Invalidate block C (should invalidate C and D as descendants)
    printf("[InvalidateBlock] Invalidating blockC...\n");
    bool invalidated = node.GetChainstate().InvalidateBlock(blockC);
    REQUIRE(invalidated);

    CHECK(node.GetTipHeight() == 2);  // Should rewind to blockB
    CHECK(node.GetTipHash() == blockB);
    printf("[InvalidateBlock] Node rewound to height 2\n");

    // Mine new blocks E, F, G on top of B
    printf("[InvalidateBlock] Mining new blocks E, F, G...\n");
    uint256 blockE = node.MineBlock();
    uint256 blockF = node.MineBlock();
    uint256 blockG = node.MineBlock();

    time_ms += 100;
    network.AdvanceTime(time_ms);

    CHECK(node.GetTipHeight() == 5);
    CHECK(node.GetTipHash() == blockG);

    // Verify C and D are still marked invalid
    auto* blockC_index = node.GetChainstate().LookupBlockIndex(blockC);
    auto* blockD_index = node.GetChainstate().LookupBlockIndex(blockD);

    REQUIRE(blockC_index != nullptr);
    REQUIRE(blockD_index != nullptr);
    CHECK(!blockC_index->IsValid());
    CHECK(!blockD_index->IsValid());

    printf("[InvalidateBlock] ✓ Successfully mined new chain after invalidation\n");
}

TEST_CASE("InvalidateBlock - Network propagation after invalidation", "[invalidateblock][functional][network]") {
    printf("[InvalidateBlock] Test: Network propagation after invalidation\n");

    SimulatedNetwork network(77777);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);
    SimulatedNode node3(3, &network);

    // Node 1 builds a chain
    printf("[InvalidateBlock] Node 1 building initial chain to height 5...\n");
    std::vector<uint256> blocks;
    for (int i = 0; i < 5; i++) {
        blocks.push_back(node1.MineBlock());
    }

    // Connect all nodes
    node2.ConnectTo(1);
    node3.ConnectTo(1);

    uint64_t time_ms = 100;
    network.AdvanceTime(time_ms);

    for (int i = 0; i < 30; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // All nodes should be synced
    CHECK(node1.GetTipHeight() == 5);
    CHECK(node2.GetTipHeight() == 5);
    CHECK(node3.GetTipHeight() == 5);
    printf("[InvalidateBlock] All nodes synced to height 5\n");

    // Node 1 invalidates block 3
    printf("[InvalidateBlock] Node 1 invalidating block 3...\n");
    bool invalidated = node1.GetChainstate().InvalidateBlock(blocks[2]);
    REQUIRE(invalidated);

    CHECK(node1.GetTipHeight() == 2);
    printf("[InvalidateBlock] Node 1 rewound to height 2\n");

    // Node 1 mines new blocks to create longer chain
    printf("[InvalidateBlock] Node 1 mining new chain...\n");
    for (int i = 0; i < 5; i++) {
        node1.MineBlock();
    }

    time_ms += 100;
    network.AdvanceTime(time_ms);

    CHECK(node1.GetTipHeight() == 7);

    // Let network propagate
    for (int i = 0; i < 30; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Nodes 2 and 3 should reorg to node1's new longer chain
    // (they never invalidated blocks, so they just see a competing fork)
    CHECK(node2.GetTipHeight() == 7);
    CHECK(node3.GetTipHeight() == 7);
    CHECK(node2.GetTipHash() == node1.GetTipHash());
    CHECK(node3.GetTipHash() == node1.GetTipHash());

    printf("[InvalidateBlock] ✓ All nodes converged on new chain after invalidation\n");
}

TEST_CASE("InvalidateBlock - Invalidate genesis should fail", "[invalidateblock][functional][network]") {
    printf("[InvalidateBlock] Test: Invalidate genesis should fail\n");

    SimulatedNetwork network(11111);
    SetZeroLatency(network);

    SimulatedNode node(1, &network);

    uint256 genesis_hash = node.GetTipHash();

    // Try to invalidate genesis - should fail
    printf("[InvalidateBlock] Attempting to invalidate genesis...\n");
    bool invalidated = node.GetChainstate().InvalidateBlock(genesis_hash);

    CHECK(!invalidated);
    CHECK(node.GetTipHeight() == 0);
    CHECK(node.GetTipHash() == genesis_hash);

    printf("[InvalidateBlock] ✓ Genesis invalidation correctly rejected\n");
}
