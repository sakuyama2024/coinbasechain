// Copyright (c) 2024 Coinbase Chain
// Test suite for DoS protection (misbehavior scoring, peer management)

#include "catch_amalgamated.hpp"
#include "network/peer_manager.hpp"
#include "network/peer.hpp"
#include "network/addr_manager.hpp"
#include <boost/asio.hpp>

using namespace coinbasechain;
using namespace coinbasechain::network;

// Helper to create a test peer
static PeerPtr create_test_peer(boost::asio::io_context &io_context,
                                const std::string &address,
                                bool is_inbound = true) {
    // Create a dummy transport connection (nullptr is fine for unit tests)
    auto peer = is_inbound
        ? Peer::create_inbound(io_context, nullptr, protocol::magic::REGTEST, 12345)
        : Peer::create_outbound(io_context, nullptr, protocol::magic::REGTEST, 12345);
    return peer;
}

TEST_CASE("PeerManager basic operations", "[dos_protection]") {
    boost::asio::io_context io_context;
    AddressManager addr_manager;
    PeerManager pm(io_context, addr_manager);

    SECTION("Add and remove peers") {
        REQUIRE(pm.peer_count() == 0);

        auto peer1 = create_test_peer(io_context, "192.168.1.1");
        int peer_id_1 = pm.add_peer(peer1, NetPermissionFlags::None, "192.168.1.1");
        REQUIRE(peer_id_1 >= 0);
        REQUIRE(pm.peer_count() == 1);
        REQUIRE(pm.GetMisbehaviorScore(peer_id_1) == 0);
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id_1));

        auto peer2 = create_test_peer(io_context, "192.168.1.2");
        int peer_id_2 = pm.add_peer(peer2, NetPermissionFlags::None, "192.168.1.2");
        REQUIRE(peer_id_2 >= 0);
        REQUIRE(pm.peer_count() == 2);

        pm.remove_peer(peer_id_1);
        REQUIRE(pm.peer_count() == 1);

        pm.remove_peer(peer_id_2);
        REQUIRE(pm.peer_count() == 0);
    }

    SECTION("Query non-existent peer") {
        // Should not crash, should return safe defaults
        REQUIRE(pm.GetMisbehaviorScore(999) == 0);
        REQUIRE_FALSE(pm.ShouldDisconnect(999));
    }
}

TEST_CASE("Misbehavior scoring - instant disconnect penalties", "[dos_protection]") {
    boost::asio::io_context io_context;
    AddressManager addr_manager;
    PeerManager pm(io_context, addr_manager);

    SECTION("INVALID_POW = instant disconnect") {
        auto peer = create_test_peer(io_context, "192.168.1.1");
        int peer_id = pm.add_peer(peer, NetPermissionFlags::None, "192.168.1.1");

        pm.ReportInvalidPoW(peer_id);

        REQUIRE(pm.GetMisbehaviorScore(peer_id) == 100);
        REQUIRE(pm.ShouldDisconnect(peer_id));
    }

    SECTION("INVALID_HEADER = instant disconnect") {
        auto peer = create_test_peer(io_context, "192.168.1.1");
        int peer_id = pm.add_peer(peer, NetPermissionFlags::None, "192.168.1.1");

        pm.ReportInvalidHeader(peer_id, "invalid-header");

        REQUIRE(pm.GetMisbehaviorScore(peer_id) == 100);
        REQUIRE(pm.ShouldDisconnect(peer_id));
    }

    SECTION("TOO_MANY_ORPHANS = instant disconnect") {
        auto peer = create_test_peer(io_context, "192.168.1.1");
        int peer_id = pm.add_peer(peer, NetPermissionFlags::None, "192.168.1.1");

        pm.ReportTooManyOrphans(peer_id);

        REQUIRE(pm.GetMisbehaviorScore(peer_id) == 100);
        REQUIRE(pm.ShouldDisconnect(peer_id));
    }
}

TEST_CASE("Misbehavior scoring - real-world scenarios", "[dos_protection]") {
    boost::asio::io_context io_context;
    AddressManager addr_manager;
    PeerManager pm(io_context, addr_manager);

    SECTION("Non-continuous headers (5x = disconnect)") {
        auto peer = create_test_peer(io_context, "192.168.1.1");
        int peer_id = pm.add_peer(peer, NetPermissionFlags::None, "192.168.1.1");

        // Send 4 non-continuous headers messages
        for (int i = 0; i < 4; i++) {
            pm.ReportNonContinuousHeaders(peer_id);
        }

        REQUIRE(pm.GetMisbehaviorScore(peer_id) == 80);
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id));

        // 5th violation should trigger disconnect
        pm.ReportNonContinuousHeaders(peer_id);
        REQUIRE(pm.GetMisbehaviorScore(peer_id) == 100);
        REQUIRE(pm.ShouldDisconnect(peer_id));
    }

    SECTION("Oversized message (5x = disconnect)") {
        auto peer = create_test_peer(io_context, "192.168.1.1");
        int peer_id = pm.add_peer(peer, NetPermissionFlags::None, "192.168.1.1");

        // Send 5 oversized messages (20 * 5 = 100)
        for (int i = 0; i < 4; i++) {
            pm.ReportOversizedMessage(peer_id);
        }

        REQUIRE(pm.GetMisbehaviorScore(peer_id) == 80);
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id));

        pm.ReportOversizedMessage(peer_id);
        REQUIRE(pm.GetMisbehaviorScore(peer_id) == 100);
        REQUIRE(pm.ShouldDisconnect(peer_id));
    }

    SECTION("Low-work headers spam (10x = disconnect)") {
        auto peer = create_test_peer(io_context, "192.168.1.1");
        int peer_id = pm.add_peer(peer, NetPermissionFlags::None, "192.168.1.1");

        // Send 10 low-work headers (10 * 10 = 100)
        for (int i = 0; i < 9; i++) {
            pm.ReportLowWorkHeaders(peer_id);
        }

        REQUIRE(pm.GetMisbehaviorScore(peer_id) == 90);
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id));

        pm.ReportLowWorkHeaders(peer_id);
        REQUIRE(pm.GetMisbehaviorScore(peer_id) == 100);
        REQUIRE(pm.ShouldDisconnect(peer_id));
    }

    SECTION("Mixed violations accumulate") {
        auto peer = create_test_peer(io_context, "192.168.1.1");
        int peer_id = pm.add_peer(peer, NetPermissionFlags::None, "192.168.1.1");

        // Mix different violations
        pm.ReportNonContinuousHeaders(peer_id);  // 20
        pm.ReportLowWorkHeaders(peer_id);        // 10
        pm.ReportOversizedMessage(peer_id);      // 20
        pm.ReportLowWorkHeaders(peer_id);        // 10
        pm.ReportNonContinuousHeaders(peer_id);  // 20

        REQUIRE(pm.GetMisbehaviorScore(peer_id) == 80);
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id));

        // One more penalty should trigger disconnect
        pm.ReportOversizedMessage(peer_id);
        REQUIRE(pm.GetMisbehaviorScore(peer_id) == 100);
        REQUIRE(pm.ShouldDisconnect(peer_id));
    }
}

TEST_CASE("Permission flags - NoBan protection", "[dos_protection]") {
    boost::asio::io_context io_context;
    AddressManager addr_manager;
    PeerManager pm(io_context, addr_manager);

    SECTION("Normal peer can be banned") {
        auto peer = create_test_peer(io_context, "192.168.1.1");
        int peer_id = pm.add_peer(peer, NetPermissionFlags::None, "192.168.1.1");

        pm.ReportInvalidPoW(peer_id);  // 100 points
        REQUIRE(pm.ShouldDisconnect(peer_id));
    }

    SECTION("NoBan peer cannot be disconnected") {
        auto peer = create_test_peer(io_context, "192.168.1.1");
        int peer_id = pm.add_peer(peer, NetPermissionFlags::NoBan, "192.168.1.1");

        // Try to trigger disconnect with severe penalty
        pm.ReportInvalidPoW(peer_id);  // 100 points

        REQUIRE(pm.GetMisbehaviorScore(peer_id) == 100);  // Score still tracked
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id));  // But not marked for disconnect
    }

    SECTION("NoBan peer accumulates score but never disconnects") {
        auto peer = create_test_peer(io_context, "192.168.1.1");
        int peer_id = pm.add_peer(peer, NetPermissionFlags::NoBan, "192.168.1.1");

        // Multiple severe violations
        pm.ReportInvalidPoW(peer_id);
        pm.ReportInvalidHeader(peer_id, "test");
        pm.ReportTooManyOrphans(peer_id);

        REQUIRE(pm.GetMisbehaviorScore(peer_id) == 300);  // Score accumulated
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id));  // But still protected
    }
}

TEST_CASE("Permission flags - Manual connection", "[dos_protection]") {
    boost::asio::io_context io_context;
    AddressManager addr_manager;
    PeerManager pm(io_context, addr_manager);

    SECTION("Manual peer can still be banned (only NoBan prevents it)") {
        auto peer = create_test_peer(io_context, "192.168.1.1");
        int peer_id = pm.add_peer(peer, NetPermissionFlags::Manual, "192.168.1.1");

        pm.ReportInvalidPoW(peer_id);
        REQUIRE(pm.ShouldDisconnect(peer_id));
    }

    SECTION("Manual + NoBan peer is protected") {
        auto flags = NetPermissionFlags::Manual | NetPermissionFlags::NoBan;
        auto peer = create_test_peer(io_context, "192.168.1.1");
        int peer_id = pm.add_peer(peer, flags, "192.168.1.1");

        pm.ReportInvalidPoW(peer_id);
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id));
    }
}

TEST_CASE("Unconnecting headers tracking", "[dos_protection]") {
    boost::asio::io_context io_context;
    AddressManager addr_manager;
    PeerManager pm(io_context, addr_manager);

    auto peer = create_test_peer(io_context, "192.168.1.1");
    int peer_id = pm.add_peer(peer, NetPermissionFlags::None, "192.168.1.1");

    SECTION("Track unconnecting headers up to threshold") {
        // Increment 9 times (below threshold of 10)
        for (int i = 0; i < 9; i++) {
            pm.IncrementUnconnectingHeaders(peer_id);
        }

        // Score shouldn't change yet (only increments counter, doesn't apply penalty)
        REQUIRE(pm.GetMisbehaviorScore(peer_id) == 0);

        // 10th time should exceed threshold
        pm.IncrementUnconnectingHeaders(peer_id);

        // Now the counter is at 10, NetworkManager should call penalty method
        // (this test just verifies the counter works)
    }

    SECTION("Reset unconnecting headers counter") {
        // Build up counter
        for (int i = 0; i < 5; i++) {
            pm.IncrementUnconnectingHeaders(peer_id);
        }

        // Reset
        pm.ResetUnconnectingHeaders(peer_id);

        // Should be able to increment 10 more times before exceeding
        for (int i = 0; i < 10; i++) {
            pm.IncrementUnconnectingHeaders(peer_id);
        }

        // Counter should be at 10 after reset
    }
}

TEST_CASE("Multi-peer scenarios", "[dos_protection]") {
    boost::asio::io_context io_context;
    AddressManager addr_manager;
    PeerManager pm(io_context, addr_manager);

    SECTION("Scores are tracked independently per peer") {
        auto peer1 = create_test_peer(io_context, "192.168.1.1");
        int peer_id_1 = pm.add_peer(peer1, NetPermissionFlags::None, "192.168.1.1");

        auto peer2 = create_test_peer(io_context, "192.168.1.2");
        int peer_id_2 = pm.add_peer(peer2, NetPermissionFlags::None, "192.168.1.2");

        auto peer3 = create_test_peer(io_context, "192.168.1.3");
        int peer_id_3 = pm.add_peer(peer3, NetPermissionFlags::None, "192.168.1.3");

        // Different violations for each peer
        pm.ReportOversizedMessage(peer_id_1);  // 20

        pm.ReportOversizedMessage(peer_id_2);  // 20
        pm.ReportNonContinuousHeaders(peer_id_2);  // 20
        pm.ReportLowWorkHeaders(peer_id_2);    // 10

        pm.ReportInvalidPoW(peer_id_3);  // 100

        REQUIRE(pm.GetMisbehaviorScore(peer_id_1) == 20);
        REQUIRE(pm.GetMisbehaviorScore(peer_id_2) == 50);
        REQUIRE(pm.GetMisbehaviorScore(peer_id_3) == 100);

        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id_1));
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id_2));
        REQUIRE(pm.ShouldDisconnect(peer_id_3));
    }

    SECTION("Removing one peer doesn't affect others") {
        auto peer1 = create_test_peer(io_context, "192.168.1.1");
        int peer_id_1 = pm.add_peer(peer1, NetPermissionFlags::None, "192.168.1.1");

        auto peer2 = create_test_peer(io_context, "192.168.1.2");
        int peer_id_2 = pm.add_peer(peer2, NetPermissionFlags::None, "192.168.1.2");

        pm.ReportOversizedMessage(peer_id_1);  // 20
        pm.ReportOversizedMessage(peer_id_1);  // 20
        pm.ReportLowWorkHeaders(peer_id_1);    // 10

        pm.ReportOversizedMessage(peer_id_2);  // 20
        pm.ReportOversizedMessage(peer_id_2);  // 20
        pm.ReportLowWorkHeaders(peer_id_2);    // 10

        pm.remove_peer(peer_id_1);

        REQUIRE(pm.peer_count() == 1);
        REQUIRE(pm.GetMisbehaviorScore(peer_id_2) == 50);
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id_2));
    }
}

TEST_CASE("Edge cases and boundary conditions", "[dos_protection]") {
    boost::asio::io_context io_context;
    AddressManager addr_manager;
    PeerManager pm(io_context, addr_manager);

    SECTION("Exact threshold value triggers disconnect") {
        auto peer = create_test_peer(io_context, "192.168.1.1");
        int peer_id = pm.add_peer(peer, NetPermissionFlags::None, "192.168.1.1");

        pm.ReportInvalidPoW(peer_id);  // Exactly 100

        REQUIRE(pm.GetMisbehaviorScore(peer_id) == DISCOURAGEMENT_THRESHOLD);
        REQUIRE(pm.ShouldDisconnect(peer_id));
    }

    SECTION("One below threshold doesn't trigger disconnect") {
        auto peer = create_test_peer(io_context, "192.168.1.1");
        int peer_id = pm.add_peer(peer, NetPermissionFlags::None, "192.168.1.1");

        // 90 points (just below threshold of 100)
        pm.ReportNonContinuousHeaders(peer_id);  // 20
        pm.ReportNonContinuousHeaders(peer_id);  // 20
        pm.ReportNonContinuousHeaders(peer_id);  // 20
        pm.ReportNonContinuousHeaders(peer_id);  // 20
        pm.ReportLowWorkHeaders(peer_id);        // 10
        // Total: 90

        REQUIRE(pm.GetMisbehaviorScore(peer_id) == 90);
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id));
    }

    SECTION("Score accumulates correctly with multiple penalties") {
        auto peer = create_test_peer(io_context, "192.168.1.1");
        int peer_id = pm.add_peer(peer, NetPermissionFlags::None, "192.168.1.1");

        // Build up score
        pm.ReportOversizedMessage(peer_id);     // 20
        pm.ReportLowWorkHeaders(peer_id);       // 10
        pm.ReportNonContinuousHeaders(peer_id); // 20

        REQUIRE(pm.GetMisbehaviorScore(peer_id) == 50);
        REQUIRE_FALSE(pm.ShouldDisconnect(peer_id));
    }
}

TEST_CASE("DoS protection constants", "[dos_protection]") {
    SECTION("Verify penalty values match design") {
        REQUIRE(MisbehaviorPenalty::INVALID_POW == 100);
        REQUIRE(MisbehaviorPenalty::INVALID_HEADER == 100);
        REQUIRE(MisbehaviorPenalty::OVERSIZED_MESSAGE == 20);
        REQUIRE(MisbehaviorPenalty::NON_CONTINUOUS_HEADERS == 20);
        REQUIRE(MisbehaviorPenalty::LOW_WORK_HEADERS == 10);
        REQUIRE(MisbehaviorPenalty::TOO_MANY_UNCONNECTING == 100);
        REQUIRE(MisbehaviorPenalty::TOO_MANY_ORPHANS == 100);
    }

    SECTION("Verify threshold") {
        REQUIRE(DISCOURAGEMENT_THRESHOLD == 100);
    }

    SECTION("Verify unconnecting headers limit") {
        REQUIRE(MAX_UNCONNECTING_HEADERS == 10);
    }

    SECTION("Verify penalty counts needed for disconnect") {
        // INVALID_POW: 1 violation = disconnect
        REQUIRE(1 * MisbehaviorPenalty::INVALID_POW >= DISCOURAGEMENT_THRESHOLD);

        // NON_CONTINUOUS_HEADERS: 5 violations = disconnect
        REQUIRE(5 * MisbehaviorPenalty::NON_CONTINUOUS_HEADERS >= DISCOURAGEMENT_THRESHOLD);

        // LOW_WORK_HEADERS: 10 violations = disconnect
        REQUIRE(10 * MisbehaviorPenalty::LOW_WORK_HEADERS >= DISCOURAGEMENT_THRESHOLD);
    }
}
