// Copyright (c) 2024 Coinbase Chain
// Test suite for DoS protection (misbehavior scoring, peer management)

#include "catch_amalgamated.hpp"
#include "sync/peer_manager.hpp"

using namespace coinbasechain;
using namespace coinbasechain::sync;

TEST_CASE("PeerManager basic operations", "[dos_protection]") {
    PeerManager pm;

    SECTION("Add and remove peers") {
        REQUIRE(pm.GetPeerCount() == 0);

        pm.AddPeer(1, "192.168.1.1");
        REQUIRE(pm.GetPeerCount() == 1);
        REQUIRE(pm.GetMisbehaviorScore(1) == 0);
        REQUIRE_FALSE(pm.ShouldDisconnect(1));

        pm.AddPeer(2, "192.168.1.2");
        REQUIRE(pm.GetPeerCount() == 2);

        pm.RemovePeer(1);
        REQUIRE(pm.GetPeerCount() == 1);

        pm.RemovePeer(2);
        REQUIRE(pm.GetPeerCount() == 0);
    }

    SECTION("Query non-existent peer") {
        // Should not crash, should return safe defaults
        REQUIRE(pm.GetMisbehaviorScore(999) == 0);
        REQUIRE_FALSE(pm.ShouldDisconnect(999));
    }
}

TEST_CASE("Misbehavior scoring - basic penalties", "[dos_protection]") {
    PeerManager pm;
    pm.AddPeer(1, "192.168.1.1");

    SECTION("Single small penalty") {
        bool should_disconnect = pm.Misbehaving(1, 10, "test-penalty");
        REQUIRE_FALSE(should_disconnect);
        REQUIRE(pm.GetMisbehaviorScore(1) == 10);
        REQUIRE_FALSE(pm.ShouldDisconnect(1));
    }

    SECTION("Multiple small penalties accumulate") {
        pm.Misbehaving(1, 10, "penalty-1");
        REQUIRE(pm.GetMisbehaviorScore(1) == 10);

        pm.Misbehaving(1, 15, "penalty-2");
        REQUIRE(pm.GetMisbehaviorScore(1) == 25);

        pm.Misbehaving(1, 20, "penalty-3");
        REQUIRE(pm.GetMisbehaviorScore(1) == 45);

        REQUIRE_FALSE(pm.ShouldDisconnect(1));
    }

    SECTION("Reaching threshold triggers disconnect") {
        // 5 penalties of 20 = 100 (threshold)
        pm.Misbehaving(1, 20, "penalty-1");
        pm.Misbehaving(1, 20, "penalty-2");
        pm.Misbehaving(1, 20, "penalty-3");
        pm.Misbehaving(1, 20, "penalty-4");

        REQUIRE(pm.GetMisbehaviorScore(1) == 80);
        REQUIRE_FALSE(pm.ShouldDisconnect(1));

        // This one should trigger disconnect
        bool should_disconnect = pm.Misbehaving(1, 20, "penalty-5");
        REQUIRE(should_disconnect);
        REQUIRE(pm.GetMisbehaviorScore(1) == 100);
        REQUIRE(pm.ShouldDisconnect(1));
    }

    SECTION("Exceeding threshold still triggers disconnect") {
        bool should_disconnect = pm.Misbehaving(1, 150, "severe-violation");
        REQUIRE(should_disconnect);
        REQUIRE(pm.GetMisbehaviorScore(1) == 150);
        REQUIRE(pm.ShouldDisconnect(1));
    }
}

TEST_CASE("Misbehavior scoring - instant disconnect penalties", "[dos_protection]") {
    PeerManager pm;

    SECTION("INVALID_POW = instant disconnect") {
        pm.AddPeer(1, "192.168.1.1");

        bool should_disconnect = pm.Misbehaving(1, MisbehaviorPenalty::INVALID_POW, "invalid-pow");
        REQUIRE(should_disconnect);
        REQUIRE(pm.GetMisbehaviorScore(1) == 100);
        REQUIRE(pm.ShouldDisconnect(1));
    }

    SECTION("INVALID_HEADER = instant disconnect") {
        pm.AddPeer(1, "192.168.1.1");

        bool should_disconnect = pm.Misbehaving(1, MisbehaviorPenalty::INVALID_HEADER, "invalid-header");
        REQUIRE(should_disconnect);
        REQUIRE(pm.GetMisbehaviorScore(1) == 100);
        REQUIRE(pm.ShouldDisconnect(1));
    }
}

TEST_CASE("Misbehavior scoring - real-world scenarios", "[dos_protection]") {
    PeerManager pm;

    SECTION("Non-continuous headers (5x = disconnect)") {
        pm.AddPeer(1, "192.168.1.1");

        // Send 4 non-continuous headers messages
        for (int i = 0; i < 4; i++) {
            bool should_disconnect = pm.Misbehaving(1, MisbehaviorPenalty::NON_CONTINUOUS_HEADERS,
                                                    "non-continuous-headers");
            REQUIRE_FALSE(should_disconnect);
        }

        REQUIRE(pm.GetMisbehaviorScore(1) == 80);
        REQUIRE_FALSE(pm.ShouldDisconnect(1));

        // 5th violation should trigger disconnect
        bool should_disconnect = pm.Misbehaving(1, MisbehaviorPenalty::NON_CONTINUOUS_HEADERS,
                                                "non-continuous-headers");
        REQUIRE(should_disconnect);
        REQUIRE(pm.GetMisbehaviorScore(1) == 100);
        REQUIRE(pm.ShouldDisconnect(1));
    }

    SECTION("Oversized message (5x = disconnect)") {
        pm.AddPeer(1, "192.168.1.1");

        // Send 5 oversized messages (20 * 5 = 100)
        for (int i = 0; i < 4; i++) {
            pm.Misbehaving(1, MisbehaviorPenalty::OVERSIZED_MESSAGE, "oversized-headers");
        }

        REQUIRE(pm.GetMisbehaviorScore(1) == 80);

        bool should_disconnect = pm.Misbehaving(1, MisbehaviorPenalty::OVERSIZED_MESSAGE,
                                                "oversized-headers");
        REQUIRE(should_disconnect);
    }

    SECTION("Low-work headers spam (10x = disconnect)") {
        pm.AddPeer(1, "192.168.1.1");

        // Send 10 low-work headers (10 * 10 = 100)
        for (int i = 0; i < 9; i++) {
            pm.Misbehaving(1, MisbehaviorPenalty::LOW_WORK_HEADERS, "low-work-headers");
        }

        REQUIRE(pm.GetMisbehaviorScore(1) == 90);
        REQUIRE_FALSE(pm.ShouldDisconnect(1));

        bool should_disconnect = pm.Misbehaving(1, MisbehaviorPenalty::LOW_WORK_HEADERS,
                                                "low-work-headers");
        REQUIRE(should_disconnect);
        REQUIRE(pm.GetMisbehaviorScore(1) == 100);
    }

    SECTION("Mixed violations accumulate") {
        pm.AddPeer(1, "192.168.1.1");

        // Mix different violations
        pm.Misbehaving(1, MisbehaviorPenalty::NON_CONTINUOUS_HEADERS, "non-continuous");  // 20
        pm.Misbehaving(1, MisbehaviorPenalty::LOW_WORK_HEADERS, "low-work");              // 10
        pm.Misbehaving(1, MisbehaviorPenalty::OVERSIZED_MESSAGE, "oversized");            // 20
        pm.Misbehaving(1, MisbehaviorPenalty::LOW_WORK_HEADERS, "low-work");              // 10
        pm.Misbehaving(1, MisbehaviorPenalty::NON_CONTINUOUS_HEADERS, "non-continuous");  // 20

        REQUIRE(pm.GetMisbehaviorScore(1) == 80);
        REQUIRE_FALSE(pm.ShouldDisconnect(1));

        // One more penalty should trigger disconnect
        bool should_disconnect = pm.Misbehaving(1, MisbehaviorPenalty::OVERSIZED_MESSAGE, "oversized");
        REQUIRE(should_disconnect);
    }
}

TEST_CASE("Permission flags - NoBan protection", "[dos_protection]") {
    PeerManager pm;

    SECTION("Normal peer can be banned") {
        pm.AddPeer(1, "192.168.1.1", NetPermissionFlags::None);

        bool should_disconnect = pm.Misbehaving(1, 100, "test-violation");
        REQUIRE(should_disconnect);
        REQUIRE(pm.ShouldDisconnect(1));
    }

    SECTION("NoBan peer cannot be disconnected") {
        pm.AddPeer(1, "192.168.1.1", NetPermissionFlags::NoBan);

        // Try to trigger disconnect with severe penalty
        bool should_disconnect = pm.Misbehaving(1, 100, "test-violation");
        REQUIRE_FALSE(should_disconnect);  // NoBan prevents disconnect
        REQUIRE(pm.GetMisbehaviorScore(1) == 100);  // Score still tracked
        REQUIRE_FALSE(pm.ShouldDisconnect(1));  // But not marked for disconnect
    }

    SECTION("NoBan peer accumulates score but never disconnects") {
        pm.AddPeer(1, "192.168.1.1", NetPermissionFlags::NoBan);

        // Multiple severe violations
        pm.Misbehaving(1, 100, "violation-1");
        pm.Misbehaving(1, 100, "violation-2");
        pm.Misbehaving(1, 100, "violation-3");

        REQUIRE(pm.GetMisbehaviorScore(1) == 300);  // Score accumulated
        REQUIRE_FALSE(pm.ShouldDisconnect(1));  // But still protected
    }
}

TEST_CASE("Permission flags - Manual connection", "[dos_protection]") {
    PeerManager pm;

    SECTION("Manual peer has Manual flag") {
        pm.AddPeer(1, "192.168.1.1", NetPermissionFlags::Manual);

        // Manual peers can still be banned (only NoBan prevents it)
        bool should_disconnect = pm.Misbehaving(1, 100, "test-violation");
        REQUIRE(should_disconnect);
        REQUIRE(pm.ShouldDisconnect(1));
    }

    SECTION("Manual + NoBan peer is protected") {
        auto flags = NetPermissionFlags::Manual | NetPermissionFlags::NoBan;
        pm.AddPeer(1, "192.168.1.1", flags);

        bool should_disconnect = pm.Misbehaving(1, 100, "test-violation");
        REQUIRE_FALSE(should_disconnect);
        REQUIRE_FALSE(pm.ShouldDisconnect(1));
    }
}

TEST_CASE("Unconnecting headers tracking", "[dos_protection]") {
    PeerManager pm;
    pm.AddPeer(1, "192.168.1.1");

    SECTION("Track unconnecting headers up to threshold") {
        // Increment 9 times (below threshold of 10)
        for (int i = 0; i < 9; i++) {
            bool exceeded = pm.IncrementUnconnectingHeaders(1);
            REQUIRE_FALSE(exceeded);
        }

        // 10th time should exceed threshold
        bool exceeded = pm.IncrementUnconnectingHeaders(1);
        REQUIRE(exceeded);
    }

    SECTION("Reset unconnecting headers counter") {
        // Build up counter
        for (int i = 0; i < 5; i++) {
            pm.IncrementUnconnectingHeaders(1);
        }

        // Reset
        pm.ResetUnconnectingHeaders(1);

        // Should be able to increment 10 more times before exceeding
        for (int i = 0; i < 9; i++) {
            bool exceeded = pm.IncrementUnconnectingHeaders(1);
            REQUIRE_FALSE(exceeded);
        }

        bool exceeded = pm.IncrementUnconnectingHeaders(1);
        REQUIRE(exceeded);
    }

    SECTION("Unconnecting headers penalty scenario") {
        // Simulate peer sending 10 unconnecting headers messages
        for (int i = 0; i < 10; i++) {
            bool exceeded = pm.IncrementUnconnectingHeaders(1);
            if (exceeded) {
                // Apply penalty
                pm.Misbehaving(1, MisbehaviorPenalty::TOO_MANY_UNCONNECTING,
                              "too-many-unconnecting-headers");
                // Reset counter after penalty
                pm.ResetUnconnectingHeaders(1);
            }
        }

        REQUIRE(pm.GetMisbehaviorScore(1) == 20);
        REQUIRE_FALSE(pm.ShouldDisconnect(1));
    }
}

TEST_CASE("Multi-peer scenarios", "[dos_protection]") {
    PeerManager pm;

    SECTION("Scores are tracked independently per peer") {
        pm.AddPeer(1, "192.168.1.1");
        pm.AddPeer(2, "192.168.1.2");
        pm.AddPeer(3, "192.168.1.3");

        // Different violations for each peer
        pm.Misbehaving(1, 20, "peer1-violation");
        pm.Misbehaving(2, 50, "peer2-violation");
        pm.Misbehaving(3, 100, "peer3-violation");

        REQUIRE(pm.GetMisbehaviorScore(1) == 20);
        REQUIRE(pm.GetMisbehaviorScore(2) == 50);
        REQUIRE(pm.GetMisbehaviorScore(3) == 100);

        REQUIRE_FALSE(pm.ShouldDisconnect(1));
        REQUIRE_FALSE(pm.ShouldDisconnect(2));
        REQUIRE(pm.ShouldDisconnect(3));
    }

    SECTION("Removing one peer doesn't affect others") {
        pm.AddPeer(1, "192.168.1.1");
        pm.AddPeer(2, "192.168.1.2");

        pm.Misbehaving(1, 50, "peer1-violation");
        pm.Misbehaving(2, 50, "peer2-violation");

        pm.RemovePeer(1);

        REQUIRE(pm.GetPeerCount() == 1);
        REQUIRE(pm.GetMisbehaviorScore(2) == 50);
        REQUIRE_FALSE(pm.ShouldDisconnect(2));
    }

    SECTION("Can handle many peers simultaneously") {
        // Add 100 peers
        for (int i = 1; i <= 100; i++) {
            pm.AddPeer(i, "192.168.1." + std::to_string(i));
        }

        REQUIRE(pm.GetPeerCount() == 100);

        // Give them all different scores
        for (int i = 1; i <= 100; i++) {
            pm.Misbehaving(i, i, "test-violation");
        }

        // Verify scores
        for (int i = 1; i <= 100; i++) {
            REQUIRE(pm.GetMisbehaviorScore(i) == i);
            if (i >= DISCOURAGEMENT_THRESHOLD) {
                REQUIRE(pm.ShouldDisconnect(i));
            } else {
                REQUIRE_FALSE(pm.ShouldDisconnect(i));
            }
        }
    }
}

TEST_CASE("Edge cases and boundary conditions", "[dos_protection]") {
    PeerManager pm;

    SECTION("Zero penalty does nothing") {
        pm.AddPeer(1, "192.168.1.1");
        bool should_disconnect = pm.Misbehaving(1, 0, "zero-penalty");
        REQUIRE_FALSE(should_disconnect);
        REQUIRE(pm.GetMisbehaviorScore(1) == 0);
    }

    SECTION("Exact threshold value triggers disconnect") {
        pm.AddPeer(1, "192.168.1.1");
        bool should_disconnect = pm.Misbehaving(1, DISCOURAGEMENT_THRESHOLD, "threshold-violation");
        REQUIRE(should_disconnect);
        REQUIRE(pm.GetMisbehaviorScore(1) == DISCOURAGEMENT_THRESHOLD);
        REQUIRE(pm.ShouldDisconnect(1));
    }

    SECTION("One below threshold doesn't trigger disconnect") {
        pm.AddPeer(1, "192.168.1.1");
        bool should_disconnect = pm.Misbehaving(1, DISCOURAGEMENT_THRESHOLD - 1, "below-threshold");
        REQUIRE_FALSE(should_disconnect);
        REQUIRE(pm.GetMisbehaviorScore(1) == DISCOURAGEMENT_THRESHOLD - 1);
        REQUIRE_FALSE(pm.ShouldDisconnect(1));
    }

    SECTION("One above threshold triggers disconnect") {
        pm.AddPeer(1, "192.168.1.1");
        bool should_disconnect = pm.Misbehaving(1, DISCOURAGEMENT_THRESHOLD + 1, "above-threshold");
        REQUIRE(should_disconnect);
        REQUIRE(pm.GetMisbehaviorScore(1) == DISCOURAGEMENT_THRESHOLD + 1);
        REQUIRE(pm.ShouldDisconnect(1));
    }

    SECTION("Score doesn't overflow with extreme values") {
        pm.AddPeer(1, "192.168.1.1");
        pm.Misbehaving(1, 10000, "extreme-violation-1");
        pm.Misbehaving(1, 10000, "extreme-violation-2");

        // Score should accumulate correctly
        REQUIRE(pm.GetMisbehaviorScore(1) == 20000);
        REQUIRE(pm.ShouldDisconnect(1));
    }
}

TEST_CASE("DoS protection constants", "[dos_protection]") {
    SECTION("Verify penalty values match design") {
        REQUIRE(MisbehaviorPenalty::INVALID_POW == 100);
        REQUIRE(MisbehaviorPenalty::INVALID_HEADER == 100);
        REQUIRE(MisbehaviorPenalty::OVERSIZED_MESSAGE == 20);
        REQUIRE(MisbehaviorPenalty::NON_CONTINUOUS_HEADERS == 20);
        REQUIRE(MisbehaviorPenalty::LOW_WORK_HEADERS == 10);
        REQUIRE(MisbehaviorPenalty::TOO_MANY_UNCONNECTING == 20);
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
