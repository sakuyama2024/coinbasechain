#include "catch_amalgamated.hpp"
#include "simulated_node.hpp"
#include "simulated_network.hpp"
#include "attack_simulated_node.hpp"
#include "chain/chainparams.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;

/**
 * Test for Low-Work Header DoS Protection (Bitcoin Core Approach)
 *
 * Bitcoin Core protects against low-work header spam by IGNORING headers
 * with insufficient chainwork, without penalizing or disconnecting the peer.
 *
 * Rationale:
 * - Legitimate scenarios exist (network partitions, different chain views)
 * - New nodes syncing might send requests that look like low-work
 * - Not necessarily malicious behavior
 *
 * Protection still works because:
 * - Low-work headers are NOT stored (prevents memory DoS)
 * - Low-work headers are NOT validated fully (prevents CPU DoS)
 * - Attackers gain nothing (we just ignore them)
 */

/**
 * Expected behavior (Bitcoin Core approach):
 * 1. Node builds a high-work chain (200 blocks)
 * 2. Attacker has a low-work fork from genesis (20 blocks)
 * 3. Attacker sends low-work headers to victim
 * 4. Victim IGNORES low-work headers without penalty
 * 5. Victim does NOT disconnect attacker
 * 6. Victim's chain remains unchanged
 */
TEST_CASE("Low-work header spam is ignored", "[network][dos]") {
    SimulatedNetwork network(12345);

    // Create victim node with high-work chain
    SimulatedNode victim(1, &network);

    // Create attacker node
    AttackSimulatedNode attacker(100, &network);

    SECTION("Victim ignores low-work spam without penalty (Bitcoin Core behavior)") {
        printf("\n=== Test: Low-Work Headers Ignored (Bitcoin Core Behavior) ===\n");

        uint64_t time_ms = 1000;

        // Step 1: Victim mines a high-work chain (200 blocks)
        printf("[Step 1] Victim mining 200 blocks...\n");
        for (int i = 0; i < 200; i++) {
            victim.MineBlock();
            time_ms += 50;
            network.AdvanceTime(time_ms);
        }

        auto victim_tip_before = victim.GetTipHash();
        auto victim_height_before = victim.GetTipHeight();
        printf("  Victim chain: height=%d, tip=%s\n",
               victim_height_before, victim_tip_before.ToString().substr(0, 16).c_str());

        // Step 2: Attacker mines a separate low-work fork from genesis (20 blocks)
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
        // Expected (Bitcoin Core): Victim IGNORES them without penalty or disconnection
        printf("[Step 4] Attacker spamming with low-work headers...\n");

        for (int spam_round = 1; spam_round <= 10; spam_round++) {
            // Send the attacker's low-work chain headers
            attacker.SendLowWorkHeaders(1, attacker_chain);
            time_ms += 100;
            network.AdvanceTime(time_ms);

            printf("  Spam round %d: low-work headers sent\n", spam_round);
        }

        // Additional network processing time
        for (int i = 0; i < 10; i++) {
            time_ms += 200;
            network.AdvanceTime(time_ms);
        }

        // Step 5: Verify Bitcoin Core behavior
        printf("[Step 5] Verifying Bitcoin Core behavior...\n");

        // 1. Nodes should still be connected (no disconnect)
        printf("  Checking connection status...\n");
        REQUIRE(victim.GetPeerCount() > 0);
        REQUIRE(attacker.GetPeerCount() > 0);
        printf("  ✅ Nodes still connected (correct - Bitcoin Core doesn't disconnect)\n");

        // 2. Victim should NOT have accepted low-work headers
        auto victim_tip_after = victim.GetTipHash();
        auto victim_height_after = victim.GetTipHeight();
        printf("  Checking victim chain...\n");
        REQUIRE(victim_tip_after == victim_tip_before);
        REQUIRE(victim_height_after == victim_height_before);
        printf("  ✅ Victim chain unchanged at height %d (correct - ignored low-work)\n", victim_height_after);

        printf("\n=== RESULT: Test PASSED ===\n");
        printf("✓ Low-work headers correctly ignored\n");
        printf("✓ No disconnection occurred (Bitcoin Core behavior)\n");
        printf("✓ Victim chain protected from low-work spam\n");
    }
}

/**
 * Additional test: Verify high-work headers are still accepted
 * This ensures the fix doesn't break normal operation
 */
TEST_CASE("High-work headers are accepted during sync", "[network][dos]") {
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

        auto node1_tip = node1.GetTipHash();
        printf("  Node1 height=%d, tip=%s\n",
               node1.GetTipHeight(), node1_tip.ToString().substr(0, 16).c_str());

        // Connect nodes
        printf("[Step 2] Connecting nodes...\n");
        node2.ConnectTo(1);
        time_ms += 500;
        network.AdvanceTime(time_ms);

        // Node2 should sync from Node1
        printf("[Step 3] Waiting for sync...\n");
        for (int i = 0; i < 50; i++) {
            time_ms += 200;
            network.AdvanceTime(time_ms);
            if (node2.GetTipHeight() == node1.GetTipHeight()) {
                printf("  Sync complete at iteration %d\n", i);
                break;
            }
        }

        // Verify sync succeeded
        REQUIRE(node1.GetPeerCount() >= 1);
        REQUIRE(node2.GetPeerCount() >= 1);
        REQUIRE(node2.GetTipHeight() == 100);
        REQUIRE(node2.GetTipHash() == node1.GetTipHash());

        printf("  ✅ Sync successful - high-work headers accepted\n");
        printf("  ✅ Node2 synced to height %d\n", node2.GetTipHeight());
        printf("\n=== RESULT: Test PASSED ===\n");
    }
}
