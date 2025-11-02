// Copyright (c) 2024 Coinbase Chain
// Unit tests for PeerManager ban functionality
// Focuses on persistence, expiration, and core operations

#include "catch_amalgamated.hpp"
#include "network/peer_manager.hpp"
#include "network/addr_manager.hpp"
#include <boost/asio.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>

using namespace coinbasechain::network;
using json = nlohmann::json;

// Test fixture to manage temporary directories and PeerManager dependencies
class BanTestFixture {
public:
    std::string test_dir;
    boost::asio::io_context io_context;
    AddressManager addr_manager;

    BanTestFixture() {
        // Create unique test directory
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        test_dir = "/tmp/peermgr_ban_test_" + std::to_string(now);
        std::filesystem::create_directory(test_dir);
    }

    ~BanTestFixture() {
        // Clean up test directory
        std::filesystem::remove_all(test_dir);
    }

    std::string GetBanlistPath() const {
        return test_dir + "/banlist.json";
    }

    // Helper to create a PeerManager for testing ban functionality
    std::unique_ptr<PeerManager> CreatePeerManager(const std::string& datadir = "") {
        auto pm = std::make_unique<PeerManager>(io_context, addr_manager);
        if (!datadir.empty()) {
            pm->LoadBans(datadir);
        }
        return pm;
    }
};

TEST_CASE("PeerManager - Basic Ban Operations", "[network][peermgr][ban][unit]") {
    BanTestFixture fixture;
    auto pm = fixture.CreatePeerManager();

    SECTION("Ban and check") {
        REQUIRE_FALSE(pm->IsBanned("192.168.1.1"));

        pm->Ban("192.168.1.1", 3600);
        REQUIRE(pm->IsBanned("192.168.1.1"));

        // Different address not banned
        REQUIRE_FALSE(pm->IsBanned("192.168.1.2"));
    }

    SECTION("Unban") {
        pm->Ban("192.168.1.1", 3600);
        REQUIRE(pm->IsBanned("192.168.1.1"));

        pm->Unban("192.168.1.1");
        REQUIRE_FALSE(pm->IsBanned("192.168.1.1"));
    }

    SECTION("Get banned list") {
        pm->Ban("192.168.1.1", 3600);
        pm->Ban("192.168.1.2", 7200);

        auto banned = pm->GetBanned();
        REQUIRE(banned.size() == 2);
        REQUIRE(banned.find("192.168.1.1") != banned.end());
        REQUIRE(banned.find("192.168.1.2") != banned.end());
    }

    SECTION("Clear all bans") {
        pm->Ban("192.168.1.1", 3600);
        pm->Ban("192.168.1.2", 3600);
        pm->Ban("192.168.1.3", 3600);

        REQUIRE(pm->GetBanned().size() == 3);

        pm->ClearBanned();

        REQUIRE(pm->GetBanned().size() == 0);
        REQUIRE_FALSE(pm->IsBanned("192.168.1.1"));
        REQUIRE_FALSE(pm->IsBanned("192.168.1.2"));
        REQUIRE_FALSE(pm->IsBanned("192.168.1.3"));
    }
}

TEST_CASE("PeerManager - Discouragement", "[network][peermgr][ban][unit]") {
    BanTestFixture fixture;
    auto pm = fixture.CreatePeerManager();

    SECTION("Discourage and check") {
        REQUIRE_FALSE(pm->IsDiscouraged("192.168.1.1"));

        pm->Discourage("192.168.1.1");
        REQUIRE(pm->IsDiscouraged("192.168.1.1"));

        // Different address not discouraged
        REQUIRE_FALSE(pm->IsDiscouraged("192.168.1.2"));
    }

    SECTION("Clear discouraged") {
        pm->Discourage("192.168.1.1");
        pm->Discourage("192.168.1.2");

        REQUIRE(pm->IsDiscouraged("192.168.1.1"));
        REQUIRE(pm->IsDiscouraged("192.168.1.2"));

        pm->ClearDiscouraged();

        REQUIRE_FALSE(pm->IsDiscouraged("192.168.1.1"));
        REQUIRE_FALSE(pm->IsDiscouraged("192.168.1.2"));
    }
}

TEST_CASE("PeerManager - Permanent Bans", "[network][peermgr][ban][unit]") {
    BanTestFixture fixture;
    auto pm = fixture.CreatePeerManager();

    SECTION("Permanent ban (ban_time_offset = 0)") {
        pm->Ban("192.168.1.1", 0);  // 0 = permanent
        REQUIRE(pm->IsBanned("192.168.1.1"));

        auto banned = pm->GetBanned();
        REQUIRE(banned.size() == 1);
        REQUIRE(banned["192.168.1.1"].nBanUntil == 0);  // 0 means permanent
    }
}

TEST_CASE("PeerManager - Ban Expiration", "[network][peermgr][ban][unit]") {
    BanTestFixture fixture;
    auto pm = fixture.CreatePeerManager();

    SECTION("Ban expires after time passes") {
        auto now = std::chrono::system_clock::now().time_since_epoch().count() / 1000000;

        // Ban for 1 second
        pm->Ban("192.168.1.1", 1);
        REQUIRE(pm->IsBanned("192.168.1.1"));

        // Wait for ban to expire
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // Sweep expired bans
        pm->SweepBanned();

        // Should no longer be banned
        REQUIRE_FALSE(pm->IsBanned("192.168.1.1"));
    }
}

TEST_CASE("PeerManager - Ban Persistence", "[network][peermgr][ban][persistence]") {
    BanTestFixture fixture;

    SECTION("Save and load bans") {
        {
            auto pm = fixture.CreatePeerManager(fixture.test_dir);
            pm->Ban("192.168.1.1", 0);  // Permanent
            pm->Ban("192.168.1.2", 3600);
            pm->Ban("192.168.1.3", 0);  // Permanent

            REQUIRE(pm->IsBanned("192.168.1.1"));
            REQUIRE(pm->IsBanned("192.168.1.2"));
            REQUIRE(pm->IsBanned("192.168.1.3"));

            // Save bans to disk
            REQUIRE(pm->SaveBans());
        }

        // Create new PeerManager and load bans
        {
            auto pm = fixture.CreatePeerManager(fixture.test_dir);

            REQUIRE(pm->IsBanned("192.168.1.1"));
            REQUIRE(pm->IsBanned("192.168.1.2"));
            REQUIRE(pm->IsBanned("192.168.1.3"));

            auto bans = pm->GetBanned();
            REQUIRE(bans.size() == 3);
        }
    }

    SECTION("Unban persists correctly") {
        {
            auto pm = fixture.CreatePeerManager(fixture.test_dir);
            pm->Ban("192.168.1.1", 0);
            pm->Ban("192.168.1.2", 0);
            pm->Ban("192.168.1.3", 0);
            pm->Unban("192.168.1.2");

            REQUIRE(pm->IsBanned("192.168.1.1"));
            REQUIRE_FALSE(pm->IsBanned("192.168.1.2"));
            REQUIRE(pm->IsBanned("192.168.1.3"));

            REQUIRE(pm->SaveBans());
        }

        {
            auto pm = fixture.CreatePeerManager(fixture.test_dir);

            REQUIRE(pm->IsBanned("192.168.1.1"));
            REQUIRE_FALSE(pm->IsBanned("192.168.1.2"));
            REQUIRE(pm->IsBanned("192.168.1.3"));
        }
    }
}

TEST_CASE("PeerManager - Whitelist (NoBan)", "[network][peermgr][ban][unit]") {
    BanTestFixture fixture;
    auto pm = fixture.CreatePeerManager();

    SECTION("Whitelisted address can be banned (like Bitcoin Core)") {
        pm->AddToWhitelist("192.168.1.1");
        REQUIRE(pm->IsWhitelisted("192.168.1.1"));

        // Can ban whitelisted address (ban and whitelist are independent)
        pm->Ban("192.168.1.1", 3600);
        REQUIRE(pm->IsBanned("192.168.1.1"));

        // Both states coexist
        REQUIRE(pm->IsWhitelisted("192.168.1.1"));
        REQUIRE(pm->IsBanned("192.168.1.1"));

        // Note: Whitelist is checked at connection time, not ban time
        // This matches Bitcoin Core behavior where ban/whitelist are independent
    }

    SECTION("Remove from whitelist") {
        pm->AddToWhitelist("192.168.1.1");
        REQUIRE(pm->IsWhitelisted("192.168.1.1"));

        pm->RemoveFromWhitelist("192.168.1.1");
        REQUIRE_FALSE(pm->IsWhitelisted("192.168.1.1"));
    }
}
