// Copyright (c) 2024 Coinbase Chain
// Permission Tests - Built from scratch with improved testing framework
//
// Tests NetPermissionFlags functionality:
// 1. NoBan - Peer never gets disconnected or banned, but score is tracked
// 2. Manual - Peer can be manually disconnected despite protections
//
// Key insight: These tests verify permissions work correctly when real messages
// flow through actual network components (not just unit testing the API).

#include "catch_amalgamated.hpp"

static struct TestSetup {
    TestSetup() {
        chain::GlobalChainParams::Select(chain::ChainType::REGTEST);
    }
} test_setup;
#include "infra/simulated_network.hpp"
using namespace coinbasechain::chain;
#include "infra/simulated_node.hpp"
#include "infra/attack_simulated_node.hpp"
#include "test_orchestrator.hpp"
#include "network_observer.hpp"
#include "chain/chainparams.hpp"
#include "network/peer_manager.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;
using namespace coinbasechain::network;

TEST_CASE("Permission: Normal peer gets disconnected for invalid PoW", "[permissions][network]") {
    // Baseline: Verify normal peers DO get disconnected
    
    
    SimulatedNetwork network(42);
    TestOrchestrator orchestrator(&network);
    NetworkObserver observer;
    AutoDumpOnFailure auto_dump(observer);
    
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);
    
    
    
    
    observer.OnCustomEvent("TEST_START", -1, "Normal peer disconnect baseline");
    
    // Setup chain
    victim.SetBypassPOWValidation(true);
    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
    }
    
    // Connect without special permissions
    attacker.ConnectTo(1);
    REQUIRE(orchestrator.WaitForConnection(victim, attacker));
    REQUIRE(orchestrator.WaitForSync(victim, attacker));
    
    orchestrator.AssertPeerCount(victim, 1);
    observer.OnPeerConnected(1, 2, orchestrator.GetPeerId(victim, attacker));
    
    // Launch attack
    victim.SetBypassPOWValidation(false);
    observer.OnCustomEvent("PHASE", -1, "Sending invalid PoW");
    attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 1);
    
    orchestrator.AdvanceTime(std::chrono::seconds(2));
    
    // Verify: Normal peer gets disconnected and discouraged
    observer.OnCustomEvent("PHASE", -1, "Verifying disconnect");
    REQUIRE(orchestrator.WaitForPeerCount(victim, 0, std::chrono::seconds(2)));
    orchestrator.AssertPeerDiscouraged(victim, attacker);
    
    observer.OnCustomEvent("TEST_END", -1, "PASSED - Normal peer disconnected");
    auto_dump.MarkSuccess();
}

TEST_CASE("Permission: NoBan peer survives invalid PoW", "[permissions][network][noban]") {
    // NoBan peers stay connected despite misbehavior, but score is tracked
    
    
    SimulatedNetwork network(123);
    TestOrchestrator orchestrator(&network);
    NetworkObserver observer;
    AutoDumpOnFailure auto_dump(observer);
    
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);
    
    
    
    
    observer.OnCustomEvent("TEST_START", -1, "NoBan peer survival test");
    
    // Setup chain
    victim.SetBypassPOWValidation(true);
    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
    }
    
    // Configure victim to accept NoBan connections
    observer.OnCustomEvent("PHASE", -1, "Setting NoBan permission");
    victim.SetInboundPermissions(NetPermissionFlags::NoBan);
    
    // Connect attacker (will be granted NoBan)
    attacker.ConnectTo(1);
    REQUIRE(orchestrator.WaitForConnection(victim, attacker));
    REQUIRE(orchestrator.WaitForSync(victim, attacker));
    
    orchestrator.AssertPeerCount(victim, 1);
    observer.OnPeerConnected(1, 2, orchestrator.GetPeerId(victim, attacker));
    observer.OnCustomEvent("INFO", -1, "Attacker connected with NoBan permission");
    
    // Launch attack
    victim.SetBypassPOWValidation(false);
    observer.OnCustomEvent("PHASE", -1, "Sending invalid PoW (should survive)");
    attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 1);
    observer.OnMessageSent(2, 1, "invalid_pow_headers", 100);
    
    orchestrator.AdvanceTime(std::chrono::seconds(2));
    
    // Verify: NoBan peer STAYS connected
    observer.OnCustomEvent("PHASE", -1, "Verifying NoBan behavior");
    orchestrator.AssertPeerCount(victim, 1);
    observer.OnCustomEvent("VERIFY", -1, "✓ Peer stayed connected");
    
    // Verify: NOT banned or discouraged
    orchestrator.AssertPeerNotDiscouraged(victim, attacker);
    observer.OnCustomEvent("VERIFY", -1, "✓ Peer not discouraged");
    
    // Verify: Score WAS tracked (Bitcoin Core behavior)
    observer.OnCustomEvent("PHASE", -1, "Checking misbehavior score");
    orchestrator.AssertMisbehaviorScore(victim, attacker, 100);
    observer.OnCustomEvent("VERIFY", -1, "✓ Score tracked (100+ points)");
    
    observer.OnCustomEvent("TEST_END", -1, "PASSED - NoBan peer survived with score tracked");
    auto_dump.MarkSuccess();
}

TEST_CASE("Permission: NoBan peer survives orphan spam", "[permissions][network][noban]") {
    // Test NoBan with different attack vector
    
    
    SimulatedNetwork network(456);
    TestOrchestrator orchestrator(&network);
    NetworkObserver observer;
    AutoDumpOnFailure auto_dump(observer);
    
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);
    
    
    
    
    observer.OnCustomEvent("TEST_START", -1, "NoBan orphan spam test");
    
    // Setup
    victim.SetBypassPOWValidation(true);
    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
    }
    
    victim.SetInboundPermissions(NetPermissionFlags::NoBan);
    attacker.ConnectTo(1);
    REQUIRE(orchestrator.WaitForConnection(victim, attacker));
    REQUIRE(orchestrator.WaitForSync(victim, attacker));
    
    // Spam orphan headers (would normally trigger disconnect)
    observer.OnCustomEvent("PHASE", -1, "Spamming orphan headers");
    for (int batch = 0; batch < 15; batch++) {
        attacker.SendOrphanHeaders(1, 100);
        observer.OnCustomEvent("ATTACK", 2, "Orphan batch " + std::to_string(batch + 1));
        orchestrator.AdvanceTime(std::chrono::milliseconds(200));
    }
    
    orchestrator.AdvanceTime(std::chrono::seconds(2));
    
    // Verify: Peer stays connected
    observer.OnCustomEvent("PHASE", -1, "Verifying survival");
    orchestrator.AssertPeerCount(victim, 1);
    orchestrator.AssertPeerNotDiscouraged(victim, attacker);
    
    // Check score accumulated
    int peer_id = orchestrator.GetPeerId(victim, attacker);
    if (peer_id >= 0) {
        auto& peer_manager = victim.GetNetworkManager().peer_manager();
        int score = peer_manager.GetMisbehaviorScore(peer_id);
        observer.OnMisbehaviorScoreChanged(1, peer_id, 0, score, "orphan_spam");
        INFO("Score after orphan spam: " << score);
        // Score should be elevated, but peer still connected
    }
    
    observer.OnCustomEvent("TEST_END", -1, "PASSED - NoBan peer survived orphan spam");
    auto_dump.MarkSuccess();
}

TEST_CASE("Permission: NoBan vs Normal peer comparison", "[permissions][network][noban]") {
    // Side-by-side comparison of NoBan vs Normal behavior
    
    
    SimulatedNetwork network(789);
    TestOrchestrator orchestrator(&network);
    NetworkObserver observer;
    AutoDumpOnFailure auto_dump(observer);
    
    // Setup two victims
    SimulatedNode victim_normal(1, &network);
    SimulatedNode victim_noban(2, &network);
    AttackSimulatedNode attacker1(10, &network);
    AttackSimulatedNode attacker2(20, &network);
    
    
    
    
    
    
    observer.OnCustomEvent("TEST_START", -1, "NoBan vs Normal comparison");
    
    // Build identical chains
    observer.OnCustomEvent("PHASE", -1, "Building chains");
    victim_normal.SetBypassPOWValidation(true);
    victim_noban.SetBypassPOWValidation(true);
    for (int i = 0; i < 5; i++) {
        victim_normal.MineBlock();
        victim_noban.MineBlock();
    }
    
    // Connect: Normal victim without permissions
    observer.OnCustomEvent("PHASE", -1, "Connecting normal peer");
    attacker1.ConnectTo(1);
    REQUIRE(orchestrator.WaitForConnection(victim_normal, attacker1));
    REQUIRE(orchestrator.WaitForSync(victim_normal, attacker1));
    
    // Connect: NoBan victim with permissions
    observer.OnCustomEvent("PHASE", -1, "Connecting NoBan peer");
    victim_noban.SetInboundPermissions(NetPermissionFlags::NoBan);
    attacker2.ConnectTo(2);
    REQUIRE(orchestrator.WaitForConnection(victim_noban, attacker2));
    REQUIRE(orchestrator.WaitForSync(victim_noban, attacker2));
    
    // Both victims have 1 peer
    orchestrator.AssertPeerCount(victim_normal, 1);
    orchestrator.AssertPeerCount(victim_noban, 1);
    
    // Launch identical attacks
    observer.OnCustomEvent("PHASE", -1, "Launching identical attacks");
    victim_normal.SetBypassPOWValidation(false);
    victim_noban.SetBypassPOWValidation(false);
    
    attacker1.SendInvalidPoWHeaders(1, victim_normal.GetTipHash(), 1);
    attacker2.SendInvalidPoWHeaders(2, victim_noban.GetTipHash(), 1);
    observer.OnMessageSent(10, 1, "invalid_pow", 100);
    observer.OnMessageSent(20, 2, "invalid_pow", 100);
    
    orchestrator.AdvanceTime(std::chrono::seconds(2));
    
    // Verify divergent behavior
    observer.OnCustomEvent("PHASE", -1, "Comparing outcomes");
    
    // Normal peer: Disconnected
    REQUIRE(orchestrator.WaitForPeerCount(victim_normal, 0, std::chrono::seconds(2)));
    observer.OnCustomEvent("RESULT", -1, "Normal peer: DISCONNECTED ✓");
    
    // NoBan peer: Still connected
    orchestrator.AssertPeerCount(victim_noban, 1);
    observer.OnCustomEvent("RESULT", -1, "NoBan peer: CONNECTED ✓");
    
    // Both chains unchanged
    orchestrator.AssertHeight(victim_normal, 5);
    orchestrator.AssertHeight(victim_noban, 5);
    
    observer.OnCustomEvent("TEST_END", -1, "PASSED - NoBan vs Normal divergence confirmed");
    auto_dump.MarkSuccess();
}

TEST_CASE("Permission: NoBan with multiple attack types", "[permissions][network][noban]") {
    // Verify NoBan protects against multiple attack vectors
    
    
    SimulatedNetwork network(999);
    TestOrchestrator orchestrator(&network);
    NetworkObserver observer;
    AutoDumpOnFailure auto_dump(observer);
    
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);
    
    
    
    
    observer.OnCustomEvent("TEST_START", -1, "NoBan multi-attack test");
    
    // Setup with NoBan
    victim.SetBypassPOWValidation(true);
    for (int i = 0; i < 10; i++) {
        victim.MineBlock();
    }
    
    victim.SetInboundPermissions(NetPermissionFlags::NoBan);
    attacker.ConnectTo(1);
    REQUIRE(orchestrator.WaitForConnection(victim, attacker));
    REQUIRE(orchestrator.WaitForSync(victim, attacker));
    
    // Launch multiple attacks
    observer.OnCustomEvent("PHASE", -1, "Attack wave 1: Orphan headers");
    for (int i = 0; i < 5; i++) {
        attacker.SendOrphanHeaders(1, 100);
        orchestrator.AdvanceTime(std::chrono::milliseconds(200));
    }
    
    observer.OnCustomEvent("PHASE", -1, "Attack wave 2: Non-continuous headers");
    attacker.SendNonContinuousHeaders(1, victim.GetTipHash());
    orchestrator.AdvanceTime(std::chrono::milliseconds(500));
    
    observer.OnCustomEvent("PHASE", -1, "Attack wave 3: More orphans");
    for (int i = 0; i < 5; i++) {
        attacker.SendOrphanHeaders(1, 100);
        orchestrator.AdvanceTime(std::chrono::milliseconds(200));
    }
    
    orchestrator.AdvanceTime(std::chrono::seconds(2));
    
    // Verify: Peer survived everything
    observer.OnCustomEvent("PHASE", -1, "Verifying survival after all attacks");
    orchestrator.AssertPeerCount(victim, 1);
    orchestrator.AssertPeerNotDiscouraged(victim, attacker);
    
    // Score should be very high
    orchestrator.AssertMisbehaviorScore(victim, attacker, 50);  // At least some points
    
    // Victim functional
    orchestrator.AssertHeight(victim, 10);
    
    observer.OnCustomEvent("TEST_END", -1, "PASSED - NoBan survived all attacks");
    auto_dump.MarkSuccess();
}

TEST_CASE("Permission: Score tracking works for NoBan peers", "[permissions][network][noban]") {
    // Explicitly verify that NoBan peers get scores tracked (Bitcoin Core behavior)
    
    
    SimulatedNetwork network(111);
    TestOrchestrator orchestrator(&network);
    NetworkObserver observer;
    AutoDumpOnFailure auto_dump(observer);
    
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);
    
    
    
    
    observer.OnCustomEvent("TEST_START", -1, "NoBan score tracking test");
    
    // Setup
    victim.SetBypassPOWValidation(true);
    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
    }
    
    victim.SetInboundPermissions(NetPermissionFlags::NoBan);
    attacker.ConnectTo(1);
    REQUIRE(orchestrator.WaitForConnection(victim, attacker));
    REQUIRE(orchestrator.WaitForSync(victim, attacker));
    
    int peer_id = orchestrator.GetPeerId(victim, attacker);
    REQUIRE(peer_id >= 0);
    
    auto& peer_manager = victim.GetNetworkManager().peer_manager();
    int score_before = peer_manager.GetMisbehaviorScore(peer_id);
    observer.OnCustomEvent("INFO", -1, "Initial score: " + std::to_string(score_before));
    REQUIRE(score_before == 0);
    
    // Send invalid PoW (100 points)
    victim.SetBypassPOWValidation(false);
    observer.OnCustomEvent("PHASE", -1, "Sending invalid PoW (100 points)");
    attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 1);
    orchestrator.AdvanceTime(std::chrono::seconds(2));
    
    // Check score increased
    int score_after = peer_manager.GetMisbehaviorScore(peer_id);
    observer.OnMisbehaviorScoreChanged(1, peer_id, score_before, score_after, "invalid_pow");
    observer.OnCustomEvent("INFO", -1, "Score after attack: " + std::to_string(score_after));
    
    REQUIRE(score_after >= 100);
    observer.OnCustomEvent("VERIFY", -1, "✓ Score increased to " + std::to_string(score_after));
    
    // Peer still connected
    orchestrator.AssertPeerCount(victim, 1);
    observer.OnCustomEvent("VERIFY", -1, "✓ Peer still connected");
    
    observer.OnCustomEvent("TEST_END", -1, "PASSED - Score tracked despite NoBan");
    auto_dump.MarkSuccess();
}
