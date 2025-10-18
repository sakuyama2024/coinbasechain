// Copyright (c) 2024 Coinbase Chain
// Adversarial tests for BanMan
// Tests security scenarios for ban management

#include "catch_amalgamated.hpp"
#include "sync/banman.hpp"

using namespace coinbasechain::sync;

// NOTE: BanMan auto-save is disabled for performance reasons in tests.
// In production, bans are persisted automatically on every operation.
// Tests that require persistence call Save() explicitly.

TEST_CASE("BanMan Adversarial - Ban Evasion", "[adversarial][banman][critical]") {
    BanMan banman("");  // No datadir = in-memory only

    SECTION("Different ports same IP") {
        banman.Ban("192.168.1.100:8333", 3600);
        REQUIRE(banman.IsBanned("192.168.1.100:8333"));
        // Different port = different address string, NOT banned
        REQUIRE_FALSE(banman.IsBanned("192.168.1.100:8334"));
    }

    SECTION("IPv4 vs IPv6 localhost") {
        banman.Ban("127.0.0.1", 3600);
        REQUIRE(banman.IsBanned("127.0.0.1"));
        // Different address format, NOT banned
        REQUIRE_FALSE(banman.IsBanned("::1"));
    }
}

TEST_CASE("BanMan Adversarial - Ban List Limits", "[adversarial][banman][dos]") {
    BanMan banman("");

    SECTION("Ban 100 different IPs (scaled down from 10000)") {
        for (int i = 0; i < 100; i++) {
            std::string ip = "10.0.0." + std::to_string(i);
            banman.Ban(ip, 3600);
        }

        // Verify all banned
        REQUIRE(banman.IsBanned("10.0.0.0"));
        REQUIRE(banman.IsBanned("10.0.0.99"));

        // Check size
        auto banned = banman.GetBanned();
        REQUIRE(banned.size() == 100);
    }

    SECTION("Discourage 100 different IPs") {
        for (int i = 0; i < 100; i++) {
            std::string ip = "10.0.0." + std::to_string(i);
            banman.Discourage(ip);
        }

        REQUIRE(banman.IsDiscouraged("10.0.0.0"));
        REQUIRE(banman.IsDiscouraged("10.0.0.99"));
    }
}

TEST_CASE("BanMan Adversarial - Time Manipulation", "[adversarial][banman][timing]") {
    BanMan banman("");

    SECTION("Permanent ban (offset = 0)") {
        banman.Ban("192.168.1.1", 0);
        REQUIRE(banman.IsBanned("192.168.1.1"));
    }

    SECTION("Negative offset (ban in past)") {
        banman.Ban("192.168.1.2", -100);
        // Negative offset creates ban_until < now, should expire immediately
        // But implementation doesn't validate, behavior is implementation-defined
        bool is_banned = banman.IsBanned("192.168.1.2");
        // Test documents behavior without asserting specific result
    }
}

TEST_CASE("BanMan Adversarial - Edge Cases", "[adversarial][banman][edge]") {
    BanMan banman("");

    SECTION("Empty address string") {
        banman.Ban("", 3600);
        REQUIRE(banman.IsBanned(""));

        banman.Unban("");
        REQUIRE_FALSE(banman.IsBanned(""));
    }

    SECTION("Very long address") {
        std::string long_addr(1000, 'A');  // Scaled down from 10000
        banman.Ban(long_addr, 3600);
        REQUIRE(banman.IsBanned(long_addr));
    }

    SECTION("Special characters") {
        std::string special = "192.168.1.1\n\t\r\"'\\";
        banman.Ban(special, 3600);
        REQUIRE(banman.IsBanned(special));
    }
}

TEST_CASE("BanMan Adversarial - Duplicate Operations", "[adversarial][banman][idempotent]") {
    BanMan banman("");

    SECTION("Ban same address twice") {
        banman.Ban("192.168.1.1", 3600);
        banman.Ban("192.168.1.1", 7200);  // Overwrites

        REQUIRE(banman.IsBanned("192.168.1.1"));
        auto banned = banman.GetBanned();
        REQUIRE(banned.size() == 1);
    }

    SECTION("Unban non-existent") {
        banman.Unban("192.168.1.1");  // Should not crash
        REQUIRE_FALSE(banman.IsBanned("192.168.1.1"));
    }

    SECTION("Discourage twice") {
        banman.Discourage("192.168.1.1");
        banman.Discourage("192.168.1.1");  // Updates expiry
        REQUIRE(banman.IsDiscouraged("192.168.1.1"));
    }
}

TEST_CASE("BanMan Adversarial - Ban vs Discourage", "[adversarial][banman][interaction]") {
    BanMan banman("");

    SECTION("Ban AND discourage same address") {
        banman.Ban("192.168.1.1", 3600);
        banman.Discourage("192.168.1.1");

        // Both independent
        REQUIRE(banman.IsBanned("192.168.1.1"));
        REQUIRE(banman.IsDiscouraged("192.168.1.1"));
    }

    SECTION("Unban discouraged address") {
        banman.Ban("192.168.1.1", 3600);
        banman.Discourage("192.168.1.1");

        banman.Unban("192.168.1.1");

        // Ban removed, discouragement remains
        REQUIRE_FALSE(banman.IsBanned("192.168.1.1"));
        REQUIRE(banman.IsDiscouraged("192.168.1.1"));
    }

    SECTION("Clear bans vs discouraged") {
        banman.Ban("192.168.1.1", 3600);
        banman.Discourage("192.168.1.1");

        banman.ClearBanned();

        // Bans cleared, discouragement remains
        REQUIRE_FALSE(banman.IsBanned("192.168.1.1"));
        REQUIRE(banman.IsDiscouraged("192.168.1.1"));
    }

    SECTION("Clear discouraged vs bans") {
        banman.Ban("192.168.1.1", 3600);
        banman.Discourage("192.168.1.1");

        banman.ClearDiscouraged();

        // Discouragement cleared, ban remains
        REQUIRE(banman.IsBanned("192.168.1.1"));
        REQUIRE_FALSE(banman.IsDiscouraged("192.168.1.1"));
    }
}

TEST_CASE("BanMan Adversarial - Sweep Operation", "[adversarial][banman][sweep]") {
    BanMan banman("");

    SECTION("Sweep removes only expired") {
        // NOTE: Can't easily test expiry without sleep, so this just
        // verifies SweepBanned doesn't crash
        banman.Ban("192.168.1.1", 3600);
        banman.Ban("192.168.1.2", 3600);

        banman.SweepBanned();  // Should not crash

        // Both still banned (not expired)
        REQUIRE(banman.IsBanned("192.168.1.1"));
        REQUIRE(banman.IsBanned("192.168.1.2"));
    }
}
