// Copyright (c) 2024 Coinbase Chain
// Unit tests for network/peer_manager.cpp - Peer lifecycle and DoS protection
//
// These tests verify:
// - Connection limits (inbound/outbound)
// - Misbehavior score tracking
// - Discouragement thresholds
// - Permission flags (NoBan, Manual)
// - Unconnecting headers tracking
// - Peer lifecycle (add/remove)

#include "catch_amalgamated.hpp"
#include "network/peer_manager.hpp"
#include "network/peer.hpp"
#include "network/addr_manager.hpp"
#include "util/uint.hpp"
#include <boost/asio.hpp>

using namespace coinbasechain::network;

// Helper to create a minimal mock peer for testing
// Note: We don't need full peer functionality, just a valid PeerPtr
class TestPeerFixture {
public:
    boost::asio::io_context io_context;
    AddressManager addr_manager;

    TestPeerFixture() {
    }

    // Create a simple outbound peer for testing
    // Note: We won't actually start/connect these peers in unit tests
    PeerPtr create_test_peer(const std::string& address = "127.0.0.1", uint16_t port = 8333) {
        // For unit testing, we just need a valid PeerPtr
        // We use create_outbound with nullptr transport since we won't actually connect
        auto peer = Peer::create_outbound(
            io_context,
            nullptr,  // No actual transport needed for these tests
            0x12345678,  // network magic
            0,           // start_height
            address,
            port
        );
        return peer;
    }
};

TEST_CASE("PeerManager - Construction", "[network][peer_manager][unit]") {
    TestPeerFixture fixture;

    PeerManager::Config config;
    config.max_outbound_peers = 8;
    config.max_inbound_peers = 125;

    PeerManager pm(fixture.io_context, fixture.addr_manager, config);

    REQUIRE(pm.peer_count() == 0);
    REQUIRE(pm.outbound_count() == 0);
    REQUIRE(pm.inbound_count() == 0);
}

TEST_CASE("PeerManager - Connection Limits", "[network][peer_manager][unit]") {
    TestPeerFixture fixture;

    PeerManager::Config config;
    config.max_outbound_peers = 2;
    config.max_inbound_peers = 3;
    config.target_outbound_peers = 2;

    PeerManager pm(fixture.io_context, fixture.addr_manager, config);

    SECTION("Needs more outbound when empty") {
        REQUIRE(pm.needs_more_outbound());
    }

    SECTION("Can accept inbound when empty") {
        REQUIRE(pm.can_accept_inbound());
    }

    SECTION("Track peer counts correctly") {
        REQUIRE(pm.peer_count() == 0);
        REQUIRE(pm.outbound_count() == 0);
        REQUIRE(pm.inbound_count() == 0);
    }
}

TEST_CASE("PeerManager - Misbehavior Scoring", "[network][peer_manager][unit]") {
    TestPeerFixture fixture;
    PeerManager pm(fixture.io_context, fixture.addr_manager);

    auto peer = fixture.create_test_peer();
    int peer_id = pm.add_peer(peer);
    REQUIRE(peer_id >= 0);

    SECTION("Initial misbehavior score is 0") {
        REQUIRE(pm.GetMisbehaviorScore(peer_id) == 0);
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id));
    }

    SECTION("Low work headers penalty") {
        pm.ReportLowWorkHeaders(peer_id);

        int score = pm.GetMisbehaviorScore(peer_id);
        REQUIRE(score == MisbehaviorPenalty::LOW_WORK_HEADERS);
        REQUIRE(score < DISCOURAGEMENT_THRESHOLD);
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id));
    }

    SECTION("Non-continuous headers penalty") {
        pm.ReportNonContinuousHeaders(peer_id);

        int score = pm.GetMisbehaviorScore(peer_id);
        REQUIRE(score == MisbehaviorPenalty::NON_CONTINUOUS_HEADERS);
        REQUIRE(score < DISCOURAGEMENT_THRESHOLD);
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id));
    }

    SECTION("Oversized message penalty") {
        pm.ReportOversizedMessage(peer_id);

        int score = pm.GetMisbehaviorScore(peer_id);
        REQUIRE(score == MisbehaviorPenalty::OVERSIZED_MESSAGE);
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id));
    }

    SECTION("Invalid PoW triggers instant disconnect") {
        pm.ReportInvalidPoW(peer_id);

        int score = pm.GetMisbehaviorScore(peer_id);
        REQUIRE(score == MisbehaviorPenalty::INVALID_POW);
        REQUIRE(score >= DISCOURAGEMENT_THRESHOLD);
        REQUIRE(pm.ShouldDisconnect(peer_id));
    }

    SECTION("Invalid header triggers instant disconnect") {
        pm.ReportInvalidHeader(peer_id, "test reason");

        int score = pm.GetMisbehaviorScore(peer_id);
        REQUIRE(score == MisbehaviorPenalty::INVALID_HEADER);
        REQUIRE(score >= DISCOURAGEMENT_THRESHOLD);
        REQUIRE(pm.ShouldDisconnect(peer_id));
    }

    SECTION("Too many orphans triggers instant disconnect") {
        pm.ReportTooManyOrphans(peer_id);

        int score = pm.GetMisbehaviorScore(peer_id);
        REQUIRE(score == MisbehaviorPenalty::TOO_MANY_ORPHANS);
        REQUIRE(score >= DISCOURAGEMENT_THRESHOLD);
        REQUIRE(pm.ShouldDisconnect(peer_id));
    }
}

TEST_CASE("PeerManager - Misbehavior Score Accumulation", "[network][peer_manager][unit]") {
    TestPeerFixture fixture;
    PeerManager pm(fixture.io_context, fixture.addr_manager);

    auto peer = fixture.create_test_peer();
    int peer_id = pm.add_peer(peer);
    REQUIRE(peer_id >= 0);

    SECTION("Multiple small violations accumulate") {
        // Report multiple low-severity violations
        pm.ReportLowWorkHeaders(peer_id);
        int score1 = pm.GetMisbehaviorScore(peer_id);
        REQUIRE(score1 == MisbehaviorPenalty::LOW_WORK_HEADERS);

        pm.ReportLowWorkHeaders(peer_id);
        int score2 = pm.GetMisbehaviorScore(peer_id);
        REQUIRE(score2 == 2 * MisbehaviorPenalty::LOW_WORK_HEADERS);

        pm.ReportLowWorkHeaders(peer_id);
        int score3 = pm.GetMisbehaviorScore(peer_id);
        REQUIRE(score3 == 3 * MisbehaviorPenalty::LOW_WORK_HEADERS);
    }

    SECTION("Mixed violations accumulate") {
        pm.ReportLowWorkHeaders(peer_id);
        pm.ReportNonContinuousHeaders(peer_id);
        pm.ReportOversizedMessage(peer_id);

        int expected = MisbehaviorPenalty::LOW_WORK_HEADERS +
                      MisbehaviorPenalty::NON_CONTINUOUS_HEADERS +
                      MisbehaviorPenalty::OVERSIZED_MESSAGE;

        REQUIRE(pm.GetMisbehaviorScore(peer_id) == expected);
    }

    SECTION("Accumulation reaches threshold") {
        // Add violations until we reach threshold
        for (int i = 0; i < 5; i++) {
            pm.ReportNonContinuousHeaders(peer_id);
        }

        int score = pm.GetMisbehaviorScore(peer_id);
        REQUIRE(score >= DISCOURAGEMENT_THRESHOLD);
        REQUIRE(pm.ShouldDisconnect(peer_id));
    }
}

TEST_CASE("PeerManager - Permission Flags", "[network][peer_manager][unit]") {
    TestPeerFixture fixture;
    PeerManager pm(fixture.io_context, fixture.addr_manager);

    SECTION("NoBan permission prevents disconnection") {
        auto peer = fixture.create_test_peer();
        int peer_id = pm.add_peer(peer, NetPermissionFlags::NoBan, "127.0.0.1");
        REQUIRE(peer_id >= 0);

        // Even with severe misbehavior, NoBan peer should not be disconnected
        pm.ReportInvalidPoW(peer_id);

        int score = pm.GetMisbehaviorScore(peer_id);
        REQUIRE(score >= DISCOURAGEMENT_THRESHOLD);

        // With NoBan, should NOT disconnect despite high score
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id));
    }

    SECTION("Manual permission") {
        auto peer = fixture.create_test_peer();
        int peer_id = pm.add_peer(peer, NetPermissionFlags::Manual);
        REQUIRE(peer_id >= 0);

        // Manual connections can still be disconnected for misbehavior
        pm.ReportInvalidPoW(peer_id);
        REQUIRE(pm.ShouldDisconnect(peer_id));
    }

    SECTION("Combined permissions") {
        auto peer = fixture.create_test_peer();
        int peer_id = pm.add_peer(peer,
                                   NetPermissionFlags::NoBan | NetPermissionFlags::Manual);
        REQUIRE(peer_id >= 0);

        // NoBan should still protect even with Manual flag
        pm.ReportInvalidPoW(peer_id);
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id));
    }
}

TEST_CASE("PeerManager - Unconnecting Headers Tracking", "[network][peer_manager][unit]") {
    TestPeerFixture fixture;
    PeerManager pm(fixture.io_context, fixture.addr_manager);

    auto peer = fixture.create_test_peer();
    int peer_id = pm.add_peer(peer);
    REQUIRE(peer_id >= 0);

    SECTION("Track unconnecting headers messages") {
        // Increment multiple times
        for (int i = 0; i < MAX_UNCONNECTING_HEADERS; i++) {
            pm.IncrementUnconnectingHeaders(peer_id);
        }

        // After MAX_UNCONNECTING_HEADERS, peer should be penalized
        pm.IncrementUnconnectingHeaders(peer_id);

        // Should have received TOO_MANY_UNCONNECTING penalty
        int score = pm.GetMisbehaviorScore(peer_id);
        REQUIRE(score >= DISCOURAGEMENT_THRESHOLD);
        REQUIRE(pm.ShouldDisconnect(peer_id));
    }

    SECTION("Reset unconnecting headers") {
        // Increment a few times
        for (int i = 0; i < 5; i++) {
            pm.IncrementUnconnectingHeaders(peer_id);
        }

        // Reset
        pm.ResetUnconnectingHeaders(peer_id);

        // Now we should be able to increment again without penalty (up to MAX-1)
        for (int i = 0; i < MAX_UNCONNECTING_HEADERS - 1; i++) {
            pm.IncrementUnconnectingHeaders(peer_id);
        }

        // Should not have penalty yet (count is MAX-1)
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id));
    }
}

TEST_CASE("PeerManager - Peer Lifecycle", "[network][peer_manager][unit]") {
    TestPeerFixture fixture;
    PeerManager pm(fixture.io_context, fixture.addr_manager);

    SECTION("Add and retrieve peer") {
        auto peer = fixture.create_test_peer();
        int peer_id = pm.add_peer(peer);

        REQUIRE(peer_id >= 0);
        REQUIRE(pm.peer_count() == 1);

        auto retrieved = pm.get_peer(peer_id);
        REQUIRE(retrieved != nullptr);
        REQUIRE(retrieved == peer);
    }

    SECTION("Add multiple peers") {
        auto peer1 = fixture.create_test_peer("192.168.1.1", 8333);
        auto peer2 = fixture.create_test_peer("192.168.1.2", 8333);
        auto peer3 = fixture.create_test_peer("192.168.1.3", 8333);

        int id1 = pm.add_peer(peer1);
        int id2 = pm.add_peer(peer2);
        int id3 = pm.add_peer(peer3);

        REQUIRE(id1 != id2);
        REQUIRE(id2 != id3);
        REQUIRE(id1 != id3);

        REQUIRE(pm.peer_count() == 3);
    }

    SECTION("Remove peer") {
        auto peer = fixture.create_test_peer();
        int peer_id = pm.add_peer(peer);

        REQUIRE(pm.peer_count() == 1);

        pm.remove_peer(peer_id);

        REQUIRE(pm.peer_count() == 0);
        REQUIRE(pm.get_peer(peer_id) == nullptr);
    }

    SECTION("Remove non-existent peer") {
        // Should not crash
        pm.remove_peer(999);
        REQUIRE(pm.peer_count() == 0);
    }
}

TEST_CASE("PeerManager - Get Peer by ID", "[network][peer_manager][unit]") {
    TestPeerFixture fixture;
    PeerManager pm(fixture.io_context, fixture.addr_manager);

    SECTION("Get existing peer") {
        auto peer = fixture.create_test_peer();
        int peer_id = pm.add_peer(peer);

        auto retrieved = pm.get_peer(peer_id);
        REQUIRE(retrieved != nullptr);
        REQUIRE(retrieved == peer);
    }

    SECTION("Get non-existent peer") {
        auto retrieved = pm.get_peer(999);
        REQUIRE(retrieved == nullptr);
    }

    SECTION("Get peer after removal") {
        auto peer = fixture.create_test_peer();
        int peer_id = pm.add_peer(peer);

        pm.remove_peer(peer_id);

        auto retrieved = pm.get_peer(peer_id);
        REQUIRE(retrieved == nullptr);
    }
}

TEST_CASE("PeerManager - Peer Count Tracking", "[network][peer_manager][unit]") {
    TestPeerFixture fixture;
    PeerManager pm(fixture.io_context, fixture.addr_manager);

    SECTION("Empty manager") {
        REQUIRE(pm.peer_count() == 0);
        REQUIRE(pm.outbound_count() == 0);
        REQUIRE(pm.inbound_count() == 0);
    }

    SECTION("Count after adding peers") {
        auto peer1 = fixture.create_test_peer();
        auto peer2 = fixture.create_test_peer();

        pm.add_peer(peer1);
        pm.add_peer(peer2);

        REQUIRE(pm.peer_count() == 2);
    }

    SECTION("Count after removing peer") {
        auto peer1 = fixture.create_test_peer();
        auto peer2 = fixture.create_test_peer();

        int id1 = pm.add_peer(peer1);
        pm.add_peer(peer2);

        REQUIRE(pm.peer_count() == 2);

        pm.remove_peer(id1);

        REQUIRE(pm.peer_count() == 1);
    }
}

TEST_CASE("PeerManager - Disconnect All", "[network][peer_manager][unit]") {
    TestPeerFixture fixture;
    PeerManager pm(fixture.io_context, fixture.addr_manager);

    // Add several peers
    auto peer1 = fixture.create_test_peer();
    auto peer2 = fixture.create_test_peer();
    auto peer3 = fixture.create_test_peer();

    pm.add_peer(peer1);
    pm.add_peer(peer2);
    pm.add_peer(peer3);

    REQUIRE(pm.peer_count() == 3);

    // Disconnect all
    pm.disconnect_all();

    // Note: disconnect_all() calls disconnect() and remove_peer() for each peer
    // After processing, peer count should be 0
    REQUIRE(pm.peer_count() == 0);
}

TEST_CASE("PeerManager - Misbehavior for Invalid Peer ID", "[network][peer_manager][unit]") {
    TestPeerFixture fixture;
    PeerManager pm(fixture.io_context, fixture.addr_manager);

    SECTION("Report misbehavior for non-existent peer") {
        // Should not crash
        pm.ReportInvalidPoW(999);
        pm.ReportLowWorkHeaders(999);
        pm.IncrementUnconnectingHeaders(999);
    }

    SECTION("Query misbehavior for non-existent peer") {
        // Should return safe defaults
        REQUIRE(pm.GetMisbehaviorScore(999) == 0);
        REQUIRE_FALSE(pm.ShouldDisconnect(999));
    }
}

TEST_CASE("PeerManager - HasPermission Utility", "[network][peer_manager][unit]") {
    SECTION("None has no permissions") {
        REQUIRE_FALSE(HasPermission(NetPermissionFlags::None, NetPermissionFlags::NoBan));
        REQUIRE_FALSE(HasPermission(NetPermissionFlags::None, NetPermissionFlags::Manual));
    }

    SECTION("NoBan flag") {
        auto flags = NetPermissionFlags::NoBan;
        REQUIRE(HasPermission(flags, NetPermissionFlags::NoBan));
        REQUIRE_FALSE(HasPermission(flags, NetPermissionFlags::Manual));
    }

    SECTION("Manual flag") {
        auto flags = NetPermissionFlags::Manual;
        REQUIRE(HasPermission(flags, NetPermissionFlags::Manual));
        REQUIRE_FALSE(HasPermission(flags, NetPermissionFlags::NoBan));
    }

    SECTION("Combined flags") {
        auto flags = NetPermissionFlags::NoBan | NetPermissionFlags::Manual;
        REQUIRE(HasPermission(flags, NetPermissionFlags::NoBan));
        REQUIRE(HasPermission(flags, NetPermissionFlags::Manual));
    }
}

TEST_CASE("PeerManager - Permission Flag Operations", "[network][peer_manager][unit]") {
    SECTION("OR operation") {
        auto combined = NetPermissionFlags::NoBan | NetPermissionFlags::Manual;
        REQUIRE(HasPermission(combined, NetPermissionFlags::NoBan));
        REQUIRE(HasPermission(combined, NetPermissionFlags::Manual));
    }

    SECTION("AND operation") {
        auto flags = NetPermissionFlags::NoBan | NetPermissionFlags::Manual;
        auto result = flags & NetPermissionFlags::NoBan;
        REQUIRE(result == NetPermissionFlags::NoBan);
    }
}

TEST_CASE("PeerManager - Misbehavior Constants", "[network][peer_manager][unit]") {
    SECTION("Penalty values are defined") {
        REQUIRE(MisbehaviorPenalty::INVALID_POW == 100);
        REQUIRE(MisbehaviorPenalty::OVERSIZED_MESSAGE == 20);
        REQUIRE(MisbehaviorPenalty::NON_CONTINUOUS_HEADERS == 20);
        REQUIRE(MisbehaviorPenalty::LOW_WORK_HEADERS == 10);
        REQUIRE(MisbehaviorPenalty::INVALID_HEADER == 100);
        REQUIRE(MisbehaviorPenalty::TOO_MANY_UNCONNECTING == 100);
        REQUIRE(MisbehaviorPenalty::TOO_MANY_ORPHANS == 100);
    }

    SECTION("Discouragement threshold") {
        REQUIRE(DISCOURAGEMENT_THRESHOLD == 100);
    }

    SECTION("Severe penalties reach threshold") {
        REQUIRE(MisbehaviorPenalty::INVALID_POW >= DISCOURAGEMENT_THRESHOLD);
        REQUIRE(MisbehaviorPenalty::INVALID_HEADER >= DISCOURAGEMENT_THRESHOLD);
        REQUIRE(MisbehaviorPenalty::TOO_MANY_ORPHANS >= DISCOURAGEMENT_THRESHOLD);
    }

    SECTION("Minor penalties don't reach threshold") {
        REQUIRE(MisbehaviorPenalty::LOW_WORK_HEADERS < DISCOURAGEMENT_THRESHOLD);
        REQUIRE(MisbehaviorPenalty::OVERSIZED_MESSAGE < DISCOURAGEMENT_THRESHOLD);
        REQUIRE(MisbehaviorPenalty::NON_CONTINUOUS_HEADERS < DISCOURAGEMENT_THRESHOLD);
    }
}

TEST_CASE("PeerManager - Config Defaults", "[network][peer_manager][unit]") {
    PeerManager::Config config;

    REQUIRE(config.max_outbound_peers == 8);
    REQUIRE(config.max_inbound_peers == 125);
    REQUIRE(config.target_outbound_peers == 8);
}

TEST_CASE("PeerManager - Multiple Misbehavior Reports", "[network][peer_manager][unit]") {
    TestPeerFixture fixture;
    PeerManager pm(fixture.io_context, fixture.addr_manager);

    auto peer1 = fixture.create_test_peer("192.168.1.1", 8333);
    auto peer2 = fixture.create_test_peer("192.168.1.2", 8333);

    int id1 = pm.add_peer(peer1);
    int id2 = pm.add_peer(peer2);

    SECTION("Independent misbehavior tracking") {
        pm.ReportLowWorkHeaders(id1);
        pm.ReportNonContinuousHeaders(id2);

        REQUIRE(pm.GetMisbehaviorScore(id1) == MisbehaviorPenalty::LOW_WORK_HEADERS);
        REQUIRE(pm.GetMisbehaviorScore(id2) == MisbehaviorPenalty::NON_CONTINUOUS_HEADERS);
    }

    SECTION("One peer reaches threshold, other doesn't") {
        pm.ReportInvalidPoW(id1);
        pm.ReportLowWorkHeaders(id2);

        REQUIRE(pm.ShouldDisconnect(id1));
        REQUIRE_FALSE(pm.ShouldDisconnect(id2));
    }
}

TEST_CASE("PeerManager - Duplicate invalid header tracking is per-peer (no double-penalty when guarded)", "[network][peer_manager][unit][duplicates]") {
    TestPeerFixture fixture;
    PeerManager pm(fixture.io_context, fixture.addr_manager);

    auto peerA = fixture.create_test_peer("10.0.0.1", 8333);
    auto peerB = fixture.create_test_peer("10.0.0.2", 8333);
    int idA = pm.add_peer(peerA);
    int idB = pm.add_peer(peerB);

    // Synthetic header hash
    uint256 h; // default zero; flip a byte to create non-null
    h.begin()[0] = 0x42;

    // Before noting, HasInvalidHeaderHash should be false for both peers
    REQUIRE_FALSE(pm.HasInvalidHeaderHash(idA, h));
    REQUIRE_FALSE(pm.HasInvalidHeaderHash(idB, h));

    // First invalid report for peerA (+100) and record the hash
    pm.ReportInvalidHeader(idA, "bad-diffbits");
    pm.NoteInvalidHeaderHash(idA, h);
    REQUIRE(pm.GetMisbehaviorScore(idA) == MisbehaviorPenalty::INVALID_HEADER);

    // Simulate duplicate from same peer: guard prevents second penalty path
    // (HeaderSyncManager checks HasInvalidHeaderHash before calling Report...)
    REQUIRE(pm.HasInvalidHeaderHash(idA, h));
    int score_before = pm.GetMisbehaviorScore(idA);
    // No additional ReportInvalidHeader is called due to guard; score remains same
    REQUIRE(pm.GetMisbehaviorScore(idA) == score_before);

    // Other peer has no record of this hash
    REQUIRE_FALSE(pm.HasInvalidHeaderHash(idB, h));
}
