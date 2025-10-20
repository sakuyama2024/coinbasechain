// Copyright (c) 2024 Coinbase Chain
// Test suite for header synchronization via NetworkManager
// Adapted from header_sync_tests.cpp to work with new architecture

#include "network_test_helpers.hpp"
#include "chain/chainparams.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;

// ==============================================================================
// HEADER SYNCHRONIZATION TESTS (via NetworkManager)
// ==============================================================================

TEST_CASE("NetworkManager HeaderSync - Basic Sync", "[.][network_header_sync][network]") {
    SimulatedNetwork network(50001);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    SECTION("Initialize with genesis") {
        // Both nodes start at genesis
        REQUIRE(node1.GetTipHeight() == 0);
        REQUIRE(node2.GetTipHeight() == 0);
        REQUIRE(!node1.GetTipHash().IsNull());
        REQUIRE(!node2.GetTipHash().IsNull());
    }

    SECTION("Process valid chain of headers") {
        // Node1 mines 10 blocks
        for (int i = 0; i < 10; i++) {
            node1.MineBlock();
        }

        // Connect nodes
        node2.ConnectTo(1);
        network.AdvanceTime(100);

        // Wait for sync
        for (int i = 0; i < 20; i++) {
            network.AdvanceTime(200);
        }

        // Node2 should have synced the headers
        REQUIRE(node2.GetTipHeight() == 10);
        REQUIRE(node2.GetTipHash() == node1.GetTipHash());
    }
}

TEST_CASE("NetworkManager HeaderSync - Locators", "[network_header_sync][network]") {
    SimulatedNetwork network(50002);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    SECTION("Sync uses locators to find common ancestor") {
        // Node1 mines 100 blocks
        for (int i = 0; i < 100; i++) {
            node1.MineBlock();
        }

        // Connect nodes - node2 will send GETHEADERS with locator from genesis
        node2.ConnectTo(1);
        network.AdvanceTime(100);

        // Wait for sync
        for (int i = 0; i < 50; i++) {
            network.AdvanceTime(200);
        }

        // Node2 should have received all headers using locator protocol
        REQUIRE(node2.GetTipHeight() == 100);
        REQUIRE(node2.GetTipHash() == node1.GetTipHash());
    }
}

TEST_CASE("NetworkManager HeaderSync - Synced Status", "[network_header_sync][network]") {
    SimulatedNetwork network(50003);
    SetZeroLatency(network);

    // Initialize network time to a realistic value (current time)
    // This avoids mock time pollution from previous tests
    network.AdvanceTime(std::time(nullptr) * 1000ULL);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    SECTION("Not synced at genesis (old timestamp)") {
        // Genesis has old timestamp (Feb 2011), current time is 2025
        // So nodes should be in IBD
        REQUIRE(node1.GetIsIBD() == true);
        REQUIRE(node2.GetIsIBD() == true);
    }

    SECTION("Synced after receiving recent headers") {
        // Node1 mines blocks with current timestamps
        for (int i = 0; i < 20; i++) {
            node1.MineBlock();
            network.AdvanceTime(network.GetCurrentTime() + 1000);  // Advance 1 second per block
        }

        // Connect and sync node2
        node2.ConnectTo(1);
        for (int i = 0; i < 50; i++) {
            network.AdvanceTime(network.GetCurrentTime() + 200);
        }

        // Node2 should now be synced
        REQUIRE(node2.GetTipHeight() == 20);
        REQUIRE(node2.GetTipHash() == node1.GetTipHash());
    }
}

TEST_CASE("NetworkManager HeaderSync - Request More", "[network_header_sync][network]") {
    SimulatedNetwork network(50004);
    SetZeroLatency(network);

    SimulatedNode miner(1, &network);
    SimulatedNode syncing(2, &network);

    SECTION("Should request more after full batch (2000 headers)") {
        // Mine exactly 2000 blocks (MAX_HEADERS_SIZE)
        printf("[Test] Mining 2000 blocks...\n");
        for (int i = 0; i < 2000; i++) {
            miner.MineBlock();
            if (i % 500 == 0) {
                printf("[Test] Mined %d blocks\n", i);
            }
        }

        REQUIRE(miner.GetTipHeight() == 2000);

        // Connect syncing node
        printf("[Test] Connecting syncing node...\n");
        syncing.ConnectTo(1);
        network.AdvanceTime(100);

        // Allow first batch to sync (2000 headers)
        for (int i = 0; i < 30; i++) {
            network.AdvanceTime(500);
        }

        // Syncing node should have received all 2000 headers
        // NetworkManager should automatically request more if needed
        REQUIRE(syncing.GetTipHeight() == 2000);
        printf("[Test] Synced %d headers\n", syncing.GetTipHeight());
    }

    SECTION("Should not request more after partial batch") {
        // Mine only 100 blocks
        for (int i = 0; i < 100; i++) {
            miner.MineBlock();
        }

        REQUIRE(miner.GetTipHeight() == 100);

        // Connect and sync
        syncing.ConnectTo(1);
        network.AdvanceTime(100);

        for (int i = 0; i < 30; i++) {
            network.AdvanceTime(200);
        }

        // Should have synced all 100 (partial batch indicates peer is done)
        REQUIRE(syncing.GetTipHeight() == 100);
    }
}

TEST_CASE("NetworkManager HeaderSync - Multi-batch Sync", "[network_header_sync][network]") {
    // Test syncing more than 2000 headers (requires multiple GETHEADERS/HEADERS round trips)
    SimulatedNetwork network(50005);
    SetZeroLatency(network);

    SimulatedNode miner(1, &network);
    SimulatedNode syncing(2, &network);

    SECTION("Sync 2500 blocks (requires 2 batches)") {
        // Mine 2500 blocks
        printf("[Test] Mining 2500 blocks...\n");
        for (int i = 0; i < 2500; i++) {
            miner.MineBlock();
            if (i % 500 == 0 && i > 0) {
                printf("[Test] Mined %d blocks\n", i);
            }
        }

        REQUIRE(miner.GetTipHeight() == 2500);

        // Connect and sync
        printf("[Test] Starting sync...\n");
        syncing.ConnectTo(1);
        network.AdvanceTime(100);

        // Allow multiple batches to sync
        // Need sufficient time for: GETHEADERS -> HEADERS (2000) -> GETHEADERS -> HEADERS (500)
        for (int i = 0; i < 100; i++) {
            network.AdvanceTime(500);
            if (i % 10 == 0) {
                printf("[Test] Iteration %d: syncing height = %d\n", i, syncing.GetTipHeight());
            }
            if (syncing.GetTipHeight() == 2500) {
                printf("[Test] Fully synced at iteration %d\n", i);
                break;
            }
        }

        // Should have synced all 2500 across multiple batches
        REQUIRE(syncing.GetTipHeight() == 2500);
    }
}

TEST_CASE("NetworkManager HeaderSync - Empty Headers Response", "[network_header_sync][network]") {
    SimulatedNetwork network(50006);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    SECTION("Handle empty HEADERS message gracefully") {
        // Both nodes at same height (genesis)
        node2.ConnectTo(1);
        network.AdvanceTime(100);

        // When node2 sends GETHEADERS, node1 will respond with empty HEADERS
        // (because they're already at same height)
        for (int i = 0; i < 10; i++) {
            network.AdvanceTime(200);
        }

        // Should remain connected and at same height
        REQUIRE(node1.GetPeerCount() > 0);
        REQUIRE(node2.GetPeerCount() > 0);
        REQUIRE(node2.GetTipHeight() == 0);
    }
}

TEST_CASE("NetworkManager HeaderSync - Concurrent Sync from Multiple Peers", "[network_header_sync][network]") {
    SimulatedNetwork network(50007);
    SetZeroLatency(network);

    SimulatedNode peer1(1, &network);
    SimulatedNode peer2(2, &network);
    SimulatedNode syncing(3, &network);

    SECTION("Sync from multiple peers with same chain") {
        // Both peers have same chain
        for (int i = 0; i < 50; i++) {
            peer1.MineBlock();
        }
        network.AdvanceTime(network.GetCurrentTime() + 500);

        // Peer2 syncs from peer1
        peer2.ConnectTo(1);
        for (int i = 0; i < 30; i++) {
            network.AdvanceTime(network.GetCurrentTime() + 200);
        }
        REQUIRE(peer2.GetTipHeight() == 50);

        // Syncing node connects to BOTH
        syncing.ConnectTo(1);
        syncing.ConnectTo(2);
        network.AdvanceTime(network.GetCurrentTime() + 100);

        // Allow sync
        for (int i = 0; i < 50; i++) {
            network.AdvanceTime(network.GetCurrentTime() + 200);
        }

        // Should successfully sync from one of the peers
        REQUIRE(syncing.GetTipHeight() == 50);
        REQUIRE(syncing.GetPeerCount() == 2);  // Connected to both
    }
}

TEST_CASE("NetworkManager HeaderSync - Sync While Mining Continues", "[network_header_sync][network]") {
    SimulatedNetwork network(50008);
    SetZeroLatency(network);

    SimulatedNode miner(1, &network);
    SimulatedNode syncing(2, &network);

    SECTION("Sync catches up while peer continues mining") {
        // Miner starts with 50 blocks
        for (int i = 0; i < 50; i++) {
            miner.MineBlock();
        }

        // Start sync
        syncing.ConnectTo(1);
        network.AdvanceTime(100);

        // Interleave: sync time + more mining
        for (int round = 0; round < 10; round++) {
            // Allow some sync time
            for (int i = 0; i < 5; i++) {
                network.AdvanceTime(200);
            }

            // Miner mines 5 more blocks
            for (int i = 0; i < 5; i++) {
                miner.MineBlock();
            }
        }

        // Final sync round
        for (int i = 0; i < 20; i++) {
            network.AdvanceTime(200);
        }

        // Syncing node should eventually catch up to moving target
        // Miner now has 50 + 50 = 100 blocks
        REQUIRE(miner.GetTipHeight() == 100);
        REQUIRE(syncing.GetTipHeight() == 100);
    }
}
