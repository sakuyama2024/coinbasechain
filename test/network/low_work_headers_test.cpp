#include "catch_amalgamated.hpp"
#include "simulated_node.hpp"
#include "simulated_network.hpp"
#include "attack_simulated_node.hpp"
#include "chain/chainparams.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;

/**
 * Test for Low-Work Header DoS Protection
 *
 * This test verifies that the node rejects headers with insufficient work
 * after Initial Block Download is complete. This prevents an attacker from
 * spamming the node with valid-but-low-work headers.
 *
 * Expected behavior:
 * 1. Node builds a high-work chain (200 blocks)
 * 2. Attacker mines a separate low-work fork from genesis (20 blocks)
 * 3. Attacker sends their low-work headers to victim
 * 4. After 10 violations, victim should disconnect attacker
 *
 * BUG STATUS: This test currently FAILS because the low-work check
 * is not implemented in network_manager.cpp::handle_headers_message()
 */
TEST_CASE("Low-work header spam triggers disconnect", "[network][dos][.low-work]") {
    SimulatedNetwork network(12345);

    // Create victim node with high-work chain
    SimulatedNode victim(1, &network);

    // Create attacker node
    AttackSimulatedNode attacker(100, &network);

    SECTION("Attacker with low-work fork gets disconnected after spam") {
        printf("\n=== Test: Low-Work Header Spam Detection ===\n");

        uint64_t time_ms = 1000;

        // Step 1: Victim mines a high-work chain (200 blocks)
        printf("[Step 1] Victim mining 200 blocks...\n");
        for (int i = 0; i < 200; i++) {
            victim.MineBlock();
            time_ms += 50;
            network.AdvanceTime(time_ms);
        }

        auto victim_tip = victim.GetTipHash();
        auto victim_height = victim.GetTipHeight();
        printf("  Victim chain: height=%d, tip=%s\n",
               victim_height, victim_tip.ToString().substr(0, 16).c_str());

        // Step 2: Attacker mines a separate low-work fork from genesis (20 blocks)
        // These blocks are mined privately and not broadcast
        printf("[Step 2] Attacker mining 20-block low-work fork...\n");
        std::vector<uint256> attacker_chain;
        for (int i = 0; i < 20; i++) {
            uint256 block_hash = attacker.MineBlockPrivate("attacker_address");
            attacker_chain.push_back(block_hash);
        }

        auto attacker_tip = attacker.GetTipHash();
        auto attacker_height = attacker.GetTipHeight();
        printf("  Attacker chain: height=%d, tip=%s\n",
               attacker_height, attacker_tip.ToString().substr(0, 16).c_str());

        // Step 3: Connect attacker to victim
        printf("[Step 3] Attacker connecting to victim...\n");
        attacker.ConnectTo(1);
        time_ms += 500;
        network.AdvanceTime(time_ms);

        // Verify connection established
        REQUIRE(victim.GetPeerCount() >= 1);
        REQUIRE(attacker.GetPeerCount() >= 1);
        printf("  Connection established\n");

        // Step 4: Spam victim with low-work headers repeatedly
        // Expected: After 10 violations (10 * LOW_WORK_HEADERS penalty = 100), disconnect
        printf("[Step 4] Attacker spamming with low-work headers...\n");

        for (int spam_round = 1; spam_round <= 15; spam_round++) {
            printf("  Spam round %d: Sending low-work headers...\n", spam_round);

            // Send the attacker's low-work chain headers
            attacker.SendLowWorkHeaders(1, attacker_chain);
            time_ms += 100;
            network.AdvanceTime(time_ms);

            // Check if disconnected (peer count drops to 0)
            if (victim.GetPeerCount() == 0 || attacker.GetPeerCount() == 0) {
                printf("  ✅ Attacker disconnected after %d spam attempts\n", spam_round);

                // Verify this happened around the 10th violation
                // (LOW_WORK_HEADERS penalty = 10, so 10 violations = 100 score)
                REQUIRE(spam_round <= 12);  // Allow some tolerance
                REQUIRE(spam_round >= 8);   // But should happen around 10

                printf("\n=== RESULT: Test PASSED ===\n");
                printf("Low-work header spam correctly triggered disconnect\n");
                return;  // Test passed
            }
        }

        // If we get here, attacker was never disconnected
        printf("  ❌ Attacker still connected after 15 spam attempts!\n");
        printf("\n=== RESULT: Test FAILED ===\n");
        printf("BUG CONFIRMED: Low-work header check is NOT enforced\n");
        printf("Expected: Disconnect after ~10 violations\n");
        printf("Actual: No disconnect after 15 violations\n");

        // This SHOULD fail currently (proving the bug exists)
        FAIL("Low-work header DoS protection not enforced - attacker never disconnected");
    }
}

/**
 * Additional test: Verify high-work headers are accepted
 * This ensures the fix doesn't break normal operation
 */
TEST_CASE("High-work headers are accepted during sync", "[network][dos][.low-work]") {
    SimulatedNetwork network(54321);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    SECTION("Normal sync with high-work headers succeeds") {
        printf("\n=== Test: High-Work Headers Accepted ===\n");

        uint64_t time_ms = 1000;

        // Node1 mines 100 blocks
        printf("[Step 1] Node1 mining 100 blocks...\n");
        for (int i = 0; i < 100; i++) {
            node1.MineBlock();
            time_ms += 50;
            network.AdvanceTime(time_ms);
        }

        // Connect nodes
        printf("[Step 2] Connecting nodes...\n");
        node1.ConnectTo(2);
        time_ms += 500;
        network.AdvanceTime(time_ms);

        // Node2 should sync from Node1
        printf("[Step 3] Waiting for sync...\n");
        for (int i = 0; i < 50; i++) {
            time_ms += 200;
            network.AdvanceTime(time_ms);
            if (node2.GetTipHeight() == node1.GetTipHeight()) {
                break;
            }
        }

        // Verify sync succeeded
        REQUIRE(node1.GetPeerCount() >= 1);
        REQUIRE(node2.GetPeerCount() >= 1);
        REQUIRE(node2.GetTipHeight() == 100);
        REQUIRE(node2.GetTipHash() == node1.GetTipHash());

        printf("  ✅ Sync successful - high-work headers accepted\n");
        printf("\n=== RESULT: Test PASSED ===\n");
    }
}
