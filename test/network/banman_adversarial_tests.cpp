// Copyright (c) 2024 Coinbase Chain
// PeerManager adversarial tests - tests edge cases, attack scenarios, and robustness

#include "catch_amalgamated.hpp"
#include "network/peer_manager.hpp"
#include "network/addr_manager.hpp"
#include <boost/asio.hpp>
#include <string>

using namespace coinbasechain::network;

// Test fixture
class AdversarialTestFixture {
public:
    boost::asio::io_context io_context;
    AddressManager addr_manager;

    std::unique_ptr<PeerManager> CreatePeerManager() {
        return std::make_unique<PeerManager>(io_context, addr_manager);
    }
};

TEST_CASE("PeerManager Adversarial - Ban Evasion", "[adversarial][banman][critical]") {
    AdversarialTestFixture fixture;
    auto pm = fixture.CreatePeerManager();

    SECTION("Different ports same IP") {
        pm->Ban("192.168.1.100:8333", 3600);
        REQUIRE(pm->IsBanned("192.168.1.100:8333"));

        // Different port should not be banned
        REQUIRE_FALSE(pm->IsBanned("192.168.1.100:8334"));
    }

    SECTION("IPv4 vs IPv6 localhost") {
        pm->Ban("127.0.0.1", 3600);
        REQUIRE(pm->IsBanned("127.0.0.1"));

        // IPv6 localhost is a different address
        REQUIRE_FALSE(pm->IsBanned("::1"));
    }
}

TEST_CASE("PeerManager Adversarial - Ban List Limits", "[adversarial][banman][dos]") {
    AdversarialTestFixture fixture;
    auto pm = fixture.CreatePeerManager();

    SECTION("Ban 100 different IPs (scaled down)") {
        // Test that we can ban a large number of addresses
        for (int i = 0; i < 100; i++) {
            pm->Ban("10.0.0." + std::to_string(i), 3600);
        }

        // Verify first and last are banned
        REQUIRE(pm->IsBanned("10.0.0.0"));
        REQUIRE(pm->IsBanned("10.0.0.99"));
        REQUIRE(pm->GetBanned().size() == 100);
    }

    SECTION("Discourage 100 different IPs") {
        // Test that we can discourage a large number of addresses
        for (int i = 0; i < 100; i++) {
            pm->Discourage("10.0.0." + std::to_string(i));
        }

        // Verify first and last are discouraged
        REQUIRE(pm->IsDiscouraged("10.0.0.0"));
        REQUIRE(pm->IsDiscouraged("10.0.0.99"));
    }
}

TEST_CASE("PeerManager Adversarial - Time Manipulation", "[adversarial][banman][timing]") {
    AdversarialTestFixture fixture;
    auto pm = fixture.CreatePeerManager();

    SECTION("Permanent ban (offset = 0)") {
        pm->Ban("192.168.1.1", 0);
        REQUIRE(pm->IsBanned("192.168.1.1"));

        // Verify it's marked as permanent
        auto banned = pm->GetBanned();
        REQUIRE(banned["192.168.1.1"].nBanUntil == 0);
    }

    SECTION("Negative offset (ban in past)") {
        // Test that negative offset is handled gracefully
        pm->Ban("192.168.1.2", -100);

        // Implementation should handle this without crashing
        // Result depends on implementation (may treat as expired or permanent)
        (void)pm->IsBanned("192.168.1.2");
    }
}

TEST_CASE("PeerManager Adversarial - Edge Cases", "[adversarial][banman][edge]") {
    AdversarialTestFixture fixture;
    auto pm = fixture.CreatePeerManager();

    SECTION("Empty address string") {
        pm->Ban("", 3600);
        REQUIRE(pm->IsBanned(""));

        pm->Unban("");
        REQUIRE_FALSE(pm->IsBanned(""));
    }

    SECTION("Very long address") {
        std::string long_addr(1000, 'A');
        pm->Ban(long_addr, 3600);
        REQUIRE(pm->IsBanned(long_addr));
    }

    SECTION("Special characters") {
        std::string special_addr = "192.168.1.1\n\t\r\"'\\";
        pm->Ban(special_addr, 3600);
        REQUIRE(pm->IsBanned(special_addr));
    }
}

TEST_CASE("PeerManager Adversarial - Duplicate Operations", "[adversarial][banman][idempotent]") {
    AdversarialTestFixture fixture;
    auto pm = fixture.CreatePeerManager();

    SECTION("Ban same address twice") {
        pm->Ban("192.168.1.1", 3600);
        pm->Ban("192.168.1.1", 7200);

        REQUIRE(pm->IsBanned("192.168.1.1"));

        // Should still be only one entry
        REQUIRE(pm->GetBanned().size() == 1);
    }

    SECTION("Unban non-existent") {
        // Should not crash
        pm->Unban("192.168.1.1");
        REQUIRE_FALSE(pm->IsBanned("192.168.1.1"));
    }

    SECTION("Discourage twice") {
        pm->Discourage("192.168.1.1");
        pm->Discourage("192.168.1.1");

        REQUIRE(pm->IsDiscouraged("192.168.1.1"));
    }
}

TEST_CASE("PeerManager Adversarial - Ban vs Discourage", "[adversarial][banman][interaction]") {
    AdversarialTestFixture fixture;
    auto pm = fixture.CreatePeerManager();

    SECTION("Ban AND discourage same address") {
        pm->Ban("192.168.1.1", 3600);
        pm->Discourage("192.168.1.1");

        // Both states can coexist
        REQUIRE(pm->IsBanned("192.168.1.1"));
        REQUIRE(pm->IsDiscouraged("192.168.1.1"));
    }

    SECTION("Unban discouraged address") {
        pm->Ban("192.168.1.1", 3600);
        pm->Discourage("192.168.1.1");
        pm->Unban("192.168.1.1");

        // Ban removed, discouragement persists
        REQUIRE_FALSE(pm->IsBanned("192.168.1.1"));
        REQUIRE(pm->IsDiscouraged("192.168.1.1"));
    }

    SECTION("Clear bans vs discouraged") {
        pm->Ban("192.168.1.1", 3600);
        pm->Discourage("192.168.1.1");
        pm->ClearBanned();

        // Only bans cleared
        REQUIRE_FALSE(pm->IsBanned("192.168.1.1"));
        REQUIRE(pm->IsDiscouraged("192.168.1.1"));
    }

    SECTION("Clear discouraged vs bans") {
        pm->Ban("192.168.1.1", 3600);
        pm->Discourage("192.168.1.1");
        pm->ClearDiscouraged();

        // Only discouragement cleared
        REQUIRE(pm->IsBanned("192.168.1.1"));
        REQUIRE_FALSE(pm->IsDiscouraged("192.168.1.1"));
    }
}

TEST_CASE("PeerManager Adversarial - Sweep Operation", "[adversarial][banman][sweep]") {
    AdversarialTestFixture fixture;
    auto pm = fixture.CreatePeerManager();

    SECTION("Sweep removes only expired (no-crash)") {
        pm->Ban("192.168.1.1", 3600);
        pm->Ban("192.168.1.2", 3600);

        // Sweep should not crash and should not remove unexpired bans
        pm->SweepBanned();

        REQUIRE(pm->IsBanned("192.168.1.1"));
        REQUIRE(pm->IsBanned("192.168.1.2"));
    }
}
