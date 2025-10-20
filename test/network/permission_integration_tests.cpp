#include "catch_amalgamated.hpp"
#include "simulated_network.hpp"
#include "simulated_node.hpp"
#include "attack_simulated_node.hpp"
#include "network/peer_manager.hpp"
#include "network/protocol.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;
using namespace coinbasechain::network;

/**
 * Permission Integration Tests
 *
 * These tests verify that NetPermissionFlags work correctly in realistic
 * network scenarios. This is the testing gap identified after the NoBan bug:
 * we had unit tests for the API, but no integration tests verifying permissions
 * work correctly when actual messages flow through the real network components.
 *
 * Key Coverage:
 * - NoBan peers: Score tracked, never disconnected, never banned
 * - Manual peers: Can be manually disconnected despite protections
 * - Score tracking: Verify scores accumulate for all peers (even NoBan)
 * - Banning behavior: Verify normal peers get banned, NoBan peers don't
 */

TEST_CASE("NoBan peer survives invalid PoW attack", "[network][permissions][noban]") {
    SimulatedNetwork network(12345);
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(100, &network);

    uint64_t time_ms = 1000000;

    // Victim mines a small chain (bypass enabled for speed)
    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
        time_ms += 50;
        network.AdvanceTime(time_ms);
    }

    SECTION("Normal peer gets banned for invalid PoW") {
        // Normal connection (no special permissions)
        REQUIRE(attacker.ConnectTo(1));

        // Advance time to complete handshake and initial sync
        for (int i = 0; i < 10; i++) {
            time_ms += 100;
            network.AdvanceTime(time_ms);
        }

        REQUIRE(victim.GetPeerCount() == 1);

        // NOW disable bypass so victim can detect invalid PoW from attacker
        victim.SetBypassPOWValidation(false);

        // Attacker sends invalid PoW headers (instant disconnect)
        attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 1);

        // Process the attack (give time for validation and disconnect)
        for (int i = 0; i < 20; i++) {
            time_ms += 100;
            network.AdvanceTime(time_ms);
        }

        // Verify disconnected and discouraged (temporary ban)
        REQUIRE(victim.GetPeerCount() == 0);
        REQUIRE(victim.GetBanMan().IsDiscouraged(attacker.GetAddress()));
    }

    SECTION("NoBan peer survives invalid PoW") {
        // Configure victim to accept NoBan connections
        victim.SetInboundPermissions(NetPermissionFlags::NoBan);

        // Attacker connects (will be accepted as NoBan peer)
        REQUIRE(attacker.ConnectTo(1));

        // Advance time to complete handshake and initial sync
        for (int i = 0; i < 10; i++) {
            time_ms += 100;
            network.AdvanceTime(time_ms);
        }

        REQUIRE(victim.GetPeerCount() == 1);

        // NOW disable bypass so victim can detect invalid PoW from attacker
        victim.SetBypassPOWValidation(false);

        // Attacker sends invalid PoW headers (normally instant disconnect)
        attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 1);

        // Process the attack (give time for validation, but peer should stay connected)
        for (int i = 0; i < 20; i++) {
            time_ms += 100;
            network.AdvanceTime(time_ms);
        }

        // Verify peer STAYS connected despite misbehavior
        REQUIRE(victim.GetPeerCount() == 1);
        REQUIRE_FALSE(victim.IsBanned(attacker.GetAddress()));

        // Verify score was still tracked (Bitcoin behavior)
        // NoBan peers get scores tracked, just not disconnected
        auto& peer_manager = victim.GetNetworkManager().peer_manager();
        // Get actual peer_id (not node_id) from PeerManager
        auto peers = peer_manager.get_all_peers();
        REQUIRE(peers.size() == 1);
        int peer_id = peers[0]->id();
        int score = peer_manager.GetMisbehaviorScore(peer_id);
        REQUIRE(score >= 100);  // Invalid PoW = 100 points
    }
}

TEST_CASE("NoBan peer survives low-work header spam", "[network][permissions][noban]") {
    SimulatedNetwork network(12345);
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(100, &network);

    uint64_t time_ms = 1000000000;

    // Victim mines high-work chain (20 blocks with bypass enabled for speed)
    for (int i = 0; i < 20; i++) {
        victim.MineBlock();
        time_ms += 50;
        network.AdvanceTime(time_ms);
    }

    // Attacker mines low-work fork (5 blocks from genesis)
    std::vector<uint256> attacker_chain;
    for (int i = 0; i < 5; i++) {
        uint256 block_hash = attacker.MineBlockPrivate("attacker_address");
        attacker_chain.push_back(block_hash);
    }

    SECTION("Normal peer gets disconnected for low-work spam") {
        // Normal connection
        REQUIRE(attacker.ConnectTo(1));

        // Complete handshake and initial sync
        for (int i = 0; i < 15; i++) {
            time_ms += 100;
            network.AdvanceTime(time_ms);
        }

        REQUIRE(victim.GetPeerCount() == 1);

        // NOW disable bypass so victim can detect low-work attack
        victim.SetBypassPOWValidation(false);

        // Spam low-work headers (10 attempts = 100 points)
        for (int i = 0; i < 10; i++) {
            attacker.SendLowWorkHeaders(1, attacker_chain);
            time_ms += 1000;
            network.AdvanceTime(time_ms);
        }

        // Verify disconnected and discouraged (temporary ban)
        REQUIRE(victim.GetPeerCount() == 0);
        REQUIRE(victim.GetBanMan().IsDiscouraged(attacker.GetAddress()));
    }

    SECTION("NoBan peer survives low-work spam") {
        // Configure victim to accept NoBan connections
        victim.SetInboundPermissions(NetPermissionFlags::NoBan);

        // Attacker connects as NoBan peer
        REQUIRE(attacker.ConnectTo(1));

        // Complete handshake and initial sync
        for (int i = 0; i < 15; i++) {
            time_ms += 100;
            network.AdvanceTime(time_ms);
        }

        REQUIRE(victim.GetPeerCount() == 1);

        // NOW disable bypass so victim can detect low-work attack
        victim.SetBypassPOWValidation(false);

        // Spam low-work headers excessively (way over threshold)
        for (int i = 0; i < 20; i++) {
            attacker.SendLowWorkHeaders(1, attacker_chain);
            time_ms += 1000;
            network.AdvanceTime(time_ms);
        }

        // Verify peer STAYS connected despite massive misbehavior
        REQUIRE(victim.GetPeerCount() == 1);
        REQUIRE_FALSE(victim.IsBanned(attacker.GetAddress()));

        // Verify score accumulated (Bitcoin behavior: track but don't disconnect)
        auto& peer_manager = victim.GetNetworkManager().peer_manager();
        // Get actual peer_id (not node_id) from PeerManager
        auto peers = peer_manager.get_all_peers();
        REQUIRE(peers.size() == 1);
        int peer_id = peers[0]->id();
        int score = peer_manager.GetMisbehaviorScore(peer_id);
        REQUIRE(score >= 100);  // Should have exceeded threshold
    }
}

TEST_CASE("NoBan peer can still be manually disconnected", "[network][permissions][noban]") {
    SimulatedNetwork network(12345);
    SimulatedNode victim(1, &network);
    SimulatedNode peer_node(2, &network);

    uint64_t time_ms = 1000000;

    // Configure victim to accept NoBan connections
    victim.SetInboundPermissions(NetPermissionFlags::NoBan);

    // Peer connects as NoBan
    REQUIRE(peer_node.ConnectTo(1));

    // Complete handshake
    for (int i = 0; i < 10; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    REQUIRE(victim.GetPeerCount() == 1);

    // Verify we can manually disconnect NoBan peers
    victim.DisconnectFrom(peer_node.GetId());

    // Process disconnect
    for (int i = 0; i < 10; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    REQUIRE(victim.GetPeerCount() == 0);
    // Should NOT be banned (manual disconnect, not misbehavior)
    REQUIRE_FALSE(victim.IsBanned(peer_node.GetAddress()));
}

TEST_CASE("Multiple permission flags work together", "[network][permissions][combined]") {
    SimulatedNetwork network(12345);
    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(100, &network);

    uint64_t time_ms = 1000000;

    // Victim mines a small chain (bypass enabled for speed)
    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
        time_ms += 50;
        network.AdvanceTime(time_ms);
    }

    // Configure with both NoBan and Manual permissions
    NetPermissionFlags combined = static_cast<NetPermissionFlags>(
        static_cast<uint32_t>(NetPermissionFlags::NoBan) |
        static_cast<uint32_t>(NetPermissionFlags::Manual)
    );
    victim.SetInboundPermissions(combined);

    // Attacker connects with combined permissions
    REQUIRE(attacker.ConnectTo(1));

    // Complete handshake and initial sync
    for (int i = 0; i < 10; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    REQUIRE(victim.GetPeerCount() == 1);

    // NOW disable bypass so victim can detect invalid PoW from attacker
    victim.SetBypassPOWValidation(false);

    // Send invalid PoW (should survive due to NoBan)
    attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 1);

    // Process attack
    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Verify still connected (NoBan protection)
    REQUIRE(victim.GetPeerCount() == 1);
    REQUIRE_FALSE(victim.IsBanned(attacker.GetAddress()));

    // Verify score tracked
    auto& peer_manager = victim.GetNetworkManager().peer_manager();
    // Get actual peer_id (not node_id) from PeerManager
    auto peers = peer_manager.get_all_peers();
    REQUIRE(peers.size() == 1);
    int peer_id = peers[0]->id();
    int score = peer_manager.GetMisbehaviorScore(peer_id);
    REQUIRE(score >= 100);
}

TEST_CASE("Score tracking works for both normal and NoBan peers", "[network][permissions][scoring]") {
    SimulatedNetwork network(12345);
    SimulatedNode victim(1, &network);
    AttackSimulatedNode normal_attacker(100, &network);
    AttackSimulatedNode noban_attacker(101, &network);

    uint64_t time_ms = 1000000;

    // Victim mines a small chain (bypass enabled for speed)
    for (int i = 0; i < 5; i++) {
        victim.MineBlock();
        time_ms += 50;
        network.AdvanceTime(time_ms);
    }

    // Normal peer connects
    REQUIRE(normal_attacker.ConnectTo(1));

    // Complete handshake and initial sync for normal peer
    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // Verify first connection is fully established
    REQUIRE(victim.GetPeerCount() == 1);

    // Configure NoBan for next connection
    victim.SetInboundPermissions(NetPermissionFlags::NoBan);

    // Give significant delay before second connection to avoid handshake race
    for (int i = 0; i < 10; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    // NoBan peer connects
    REQUIRE(noban_attacker.ConnectTo(1));

    // Complete handshake and initial sync for NoBan peer
    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    REQUIRE(victim.GetPeerCount() == 2);

    // NOW disable bypass so victim can detect invalid PoW from attackers
    victim.SetBypassPOWValidation(false);

    // Both send invalid PoW
    normal_attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 1);
    noban_attacker.SendInvalidPoWHeaders(1, victim.GetTipHash(), 1);

    // Process both attacks (give more time for disconnects to process)
    for (int i = 0; i < 20; i++) {
        time_ms += 100;
        network.AdvanceTime(time_ms);
    }

    auto& peer_manager = victim.GetNetworkManager().peer_manager();

    // Normal peer: disconnected and discouraged (temporary ban)
    REQUIRE(victim.GetPeerCount() == 1);  // Only NoBan peer remains
    REQUIRE(victim.GetBanMan().IsDiscouraged(normal_attacker.GetAddress()));

    // NoBan peer: still connected, score tracked
    REQUIRE_FALSE(victim.GetBanMan().IsDiscouraged(noban_attacker.GetAddress()));
    // Get actual peer_id (not node_id) from PeerManager
    // After disconnect, only the NoBan peer should remain
    auto peers = peer_manager.get_all_peers();
    REQUIRE(peers.size() == 1);
    int peer_id = peers[0]->id();
    int noban_score = peer_manager.GetMisbehaviorScore(peer_id);
    REQUIRE(noban_score >= 100);
}
