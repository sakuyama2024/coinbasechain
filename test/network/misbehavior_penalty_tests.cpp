// Copyright (c) 2024 Coinbase Chain
// Misbehavior penalty tests for P2P network DoS protection

#include "network_test_helpers.hpp"

using namespace coinbasechain::test;

// ==============================================================================
// MISBEHAVIOR PENALTY TESTS
// ==============================================================================

TEST_CASE("MisbehaviorTest - InvalidPoWPenalty", "[misbehaviortest][network]") {
    // Test INVALID_POW penalty (100 points - instant disconnect)
    printf("[Misbehavior] Testing INVALID_POW penalty (100 points)...\n");

    SimulatedNetwork network(12345);
    SetZeroLatency(network);
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);

    // Build small chain FIRST (while PoW bypass is enabled)
    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
    }

    // NOW enable PoW validation for victim so it can detect invalid PoW from peers
    victim.SetBypassPOWValidation(false);

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

    // Ensure all async disconnect operations complete
    // Use AdvanceTime to ensure both nodes process the disconnect event
    for (int i = 0; i < 5; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Attacker should be disconnected (100 >= DISCOURAGEMENT_THRESHOLD)
    printf("[TEST] victim.GetPeerCount() = %zu\n", victim.GetPeerCount());
    printf("[TEST] victim.GetInboundPeerCount() = %zu\n", victim.GetInboundPeerCount());
    printf("[TEST] victim.GetOutboundPeerCount() = %zu\n", victim.GetOutboundPeerCount());
    printf("[TEST] attacker.GetPeerCount() = %zu\n", attacker.GetPeerCount());
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
    // Test TOO_MANY_ORPHANS penalty (100 points - instant disconnect)
    // Should disconnect after 1 offense (1 * 100 = 100)
    printf("[Misbehavior] Testing TOO_MANY_ORPHANS penalty (100 points)...\n");

    SimulatedNetwork network(12345);
    SetZeroLatency(network);
    SimulatedNode victim(50, &network);
    AttackSimulatedNode attacker(60, &network);

    // Build small chain FIRST (while PoW bypass is enabled)
    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
    }

    // NOW disable PoW bypass so victim can properly detect orphan headers from peers
    // (orphan detection requires real validation to check if parent exists)
    victim.SetBypassPOWValidation(false);

    // Attacker connects
    attacker.ConnectTo(50);
    network.AdvanceTime(network.GetCurrentTime() + 100);

    for (int i = 0; i < 10; i++) {
        network.AdvanceTime(network.GetCurrentTime() + 100);
    }

    REQUIRE(victim.GetPeerCount() == 1);

    // Send 1 batch of 1000 orphan headers (exceeds MAX_ORPHAN_HEADERS_PER_PEER=50)
    // This triggers TOO_MANY_ORPHANS penalty (100 points)
    printf("[Misbehavior] Sending 1000 orphan headers...\n");
    attacker.SendOrphanHeaders(50, 1000);

    // Allow time for message processing and periodic disconnect checks
    printf("[Misbehavior] Processing messages...\n");
    for (int i = 0; i < 50; i++) {
        size_t delivered = network.AdvanceTime(network.GetCurrentTime() + 100);
        if (i < 5 || i % 10 == 0) {
            printf("[Misbehavior] Iteration %d: delivered=%zu victim_peers=%zu attacker_peers=%zu\n",
                   i, delivered, victim.GetPeerCount(), attacker.GetPeerCount());
        }
    }

    // Should be disconnected now (1 * 100 = 100 >= DISCOURAGEMENT_THRESHOLD)
    printf("[Misbehavior] Final check: victim.GetPeerCount()=%zu (expected 0)\n", victim.GetPeerCount());
    CHECK(victim.GetPeerCount() == 0);
    printf("[Misbehavior] ✓ TOO_MANY_ORPHANS: Disconnected after 1 offense\n");
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
    network.AdvanceTime(network.GetCurrentTime() + 100);

    for (int i = 0; i < 10; i++) {
        network.AdvanceTime(network.GetCurrentTime() + 100);
    }

    REQUIRE(victim.GetPeerCount() == 1);

    // Accumulate misbehavior score gradually:
    // 4x non-continuous (20 points each) = 80 points (< 100, still connected)
    // Then 1 more non-continuous = 100 points (>= 100, disconnected)
    // Note: Orphan spam is 100 points (instant disconnect), so not used here

    for (int j = 0; j < 4; j++) {
        attacker.SendNonContinuousHeaders(70, victim.GetTipHash());
        for (int i = 0; i < 10; i++) {
            network.AdvanceTime(network.GetCurrentTime() + 100);
        }
    }

    // Should still be connected (score = 80 < 100)
    CHECK(victim.GetPeerCount() == 1);
    printf("[Misbehavior] Score = 80 (4x20), still connected\n");

    // One more offense should cause disconnect
    attacker.SendNonContinuousHeaders(70, victim.GetTipHash());
    for (int i = 0; i < 20; i++) {
        network.AdvanceTime(network.GetCurrentTime() + 100);
    }

    // Should be disconnected now (80 + 20 = 100 >= 100)
    CHECK(victim.GetPeerCount() == 0);
    printf("[Misbehavior] ✓ Score accumulation: 5 offenses reached threshold (100)\n");
}


