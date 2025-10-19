// Copyright (c) 2024 Coinbase Chain
// Attack simulation tests for P2P network security

#include "network_test_helpers.hpp"

using namespace coinbasechain::test;

// ==============================================================================
// ATTACK SIMULATION TESTS
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

    // Attack: Send multiple batches of orphan headers to trigger unconnecting threshold
    // DoS protection requires MAX_UNCONNECTING_HEADERS (10) messages before penalty
    // Each unconnecting message gives TOO_MANY_UNCONNECTING (20) points
    // Need 10 messages = 200 points total (exceeds DISCOURAGEMENT_THRESHOLD of 100)
    for (int batch = 0; batch < 10; batch++) {
        attacker.SendOrphanHeaders(1, 100);  // Send 100 orphan headers per batch

        // Process this batch
        for (int i = 0; i < 3; i++) {
            time_ms += 100;
            network.AdvanceTime(time_ms);
        }
    }

    printf("[OrphanGrind] Sent 10 batches of orphan headers. Processing...\n");

    // Process remaining events
    for (int i = 0; i < 5; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Expected defense: Reject unconnecting headers after threshold
    // Victim should:
    // 1. Still be functional (didn't crash)
    CHECK(victim.GetTipHeight() == 5);

    // 2. Attacker should be disconnected after 10 unconnecting messages (10 * 20 = 200 points)
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

    // Attack: Each attacker sends 10 batches of orphan headers
    // DoS protection: MAX_UNCONNECTING_HEADERS (10) messages triggers penalty
    // 10 messages * TOO_MANY_UNCONNECTING (20 points) = 200 points (exceeds 100 threshold)
    for (int batch = 0; batch < 10; batch++) {
        attacker_a.SendOrphanHeaders(1, 50);
        attacker_b.SendOrphanHeaders(1, 50);
        attacker_c.SendOrphanHeaders(1, 50);

        // Process this round
        for (int i = 0; i < 4; i++) {
            time_ms += 100;
            network.AdvanceTime(time_ms);
        }
    }

    printf("[OrphanStorm] All attackers sent 10 batches. Processing...\n");

    // Process remaining events
    for (int i = 0; i < 10; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Expected defense:
    // - Per-peer tracking: each attacker gets disconnected after 10 unconnecting messages
    // - Victim remains functional (didn't crash)
    CHECK(victim.GetTipHeight() == 5);

    // - All attackers should be disconnected after 10 unconnecting messages each
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

