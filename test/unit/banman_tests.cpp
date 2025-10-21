// Copyright (c) 2024 Coinbase Chain
// Unit tests for BanMan basic functionality
// Focuses on persistence, expiration, and core operations

#include "catch_amalgamated.hpp"
#include "network/banman.hpp"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>

using namespace coinbasechain::network;
using json = nlohmann::json;

// Test fixture to manage temporary directories
class BanManTestFixture {
public:
    std::string test_dir;

    BanManTestFixture() {
        // Create unique test directory
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        test_dir = "/tmp/banman_test_" + std::to_string(now);
        std::filesystem::create_directory(test_dir);
    }

    ~BanManTestFixture() {
        // Clean up test directory
        std::filesystem::remove_all(test_dir);
    }

    std::string GetBanlistPath() const {
        return test_dir + "/banlist.json";
    }
};

TEST_CASE("BanMan - Basic Ban Operations", "[network][banman][unit]") {
    BanMan banman("");  // In-memory

    SECTION("Ban and check") {
        REQUIRE_FALSE(banman.IsBanned("192.168.1.1"));

        banman.Ban("192.168.1.1", 3600);
        REQUIRE(banman.IsBanned("192.168.1.1"));

        // Different address not banned
        REQUIRE_FALSE(banman.IsBanned("192.168.1.2"));
    }

    SECTION("Unban") {
        banman.Ban("192.168.1.1", 3600);
        REQUIRE(banman.IsBanned("192.168.1.1"));

        banman.Unban("192.168.1.1");
        REQUIRE_FALSE(banman.IsBanned("192.168.1.1"));
    }

    SECTION("Get banned list") {
        banman.Ban("192.168.1.1", 3600);
        banman.Ban("192.168.1.2", 7200);

        auto banned = banman.GetBanned();
        REQUIRE(banned.size() == 2);
        REQUIRE(banned.find("192.168.1.1") != banned.end());
        REQUIRE(banned.find("192.168.1.2") != banned.end());
    }

    SECTION("Clear all bans") {
        banman.Ban("192.168.1.1", 3600);
        banman.Ban("192.168.1.2", 3600);
        banman.Ban("192.168.1.3", 3600);

        REQUIRE(banman.GetBanned().size() == 3);

        banman.ClearBanned();

        REQUIRE(banman.GetBanned().size() == 0);
        REQUIRE_FALSE(banman.IsBanned("192.168.1.1"));
        REQUIRE_FALSE(banman.IsBanned("192.168.1.2"));
        REQUIRE_FALSE(banman.IsBanned("192.168.1.3"));
    }
}

TEST_CASE("BanMan - Discouragement", "[network][banman][unit]") {
    BanMan banman("");

    SECTION("Discourage and check") {
        REQUIRE_FALSE(banman.IsDiscouraged("192.168.1.1"));

        banman.Discourage("192.168.1.1");
        REQUIRE(banman.IsDiscouraged("192.168.1.1"));

        // Different address not discouraged
        REQUIRE_FALSE(banman.IsDiscouraged("192.168.1.2"));
    }

    SECTION("Clear discouraged") {
        banman.Discourage("192.168.1.1");
        banman.Discourage("192.168.1.2");

        REQUIRE(banman.IsDiscouraged("192.168.1.1"));
        REQUIRE(banman.IsDiscouraged("192.168.1.2"));

        banman.ClearDiscouraged();

        REQUIRE_FALSE(banman.IsDiscouraged("192.168.1.1"));
        REQUIRE_FALSE(banman.IsDiscouraged("192.168.1.2"));
    }

    SECTION("Ban and discourage are independent") {
        banman.Ban("192.168.1.1", 3600);
        banman.Discourage("192.168.1.1");

        REQUIRE(banman.IsBanned("192.168.1.1"));
        REQUIRE(banman.IsDiscouraged("192.168.1.1"));

        // Clear bans doesn't affect discouragement
        banman.ClearBanned();
        REQUIRE_FALSE(banman.IsBanned("192.168.1.1"));
        REQUIRE(banman.IsDiscouraged("192.168.1.1"));
    }
}

TEST_CASE("BanMan - Persistence", "[network][banman][unit]") {
    BanManTestFixture fixture;

    SECTION("Save and load bans") {
        {
            BanMan banman(fixture.test_dir, false);  // Disable auto-save for tests

            // Add some bans
            banman.Ban("192.168.1.1", 3600);
            banman.Ban("192.168.1.2", 0);  // Permanent
            banman.Ban("10.0.0.1", 7200);

            // Save
            REQUIRE(banman.Save());

            // Verify file exists
            REQUIRE(std::filesystem::exists(fixture.GetBanlistPath()));
        }

        // Load in new instance
        {
            BanMan banman2(fixture.test_dir, false);  // Disable auto-save for tests
            REQUIRE(banman2.Load());

            // Check bans persisted
            REQUIRE(banman2.IsBanned("192.168.1.1"));
            REQUIRE(banman2.IsBanned("192.168.1.2"));
            REQUIRE(banman2.IsBanned("10.0.0.1"));

            auto banned = banman2.GetBanned();
            REQUIRE(banned.size() == 3);
        }
    }

    SECTION("Load from non-existent file") {
        BanMan banman(fixture.test_dir, false);  // Disable auto-save for tests

        // Should succeed (not an error for first run)
        REQUIRE(banman.Load());

        // Should have no bans
        REQUIRE(banman.GetBanned().size() == 0);
    }

    SECTION("Save with no datadir") {
        BanMan banman("");  // No datadir

        banman.Ban("192.168.1.1", 3600);

        // Save should succeed but do nothing
        REQUIRE(banman.Save());

        // No file should be created
        REQUIRE_FALSE(std::filesystem::exists("/banlist.json"));
    }

    SECTION("Load with no datadir") {
        BanMan banman("");

        // Should succeed
        REQUIRE(banman.Load());

        // Should have no bans
        REQUIRE(banman.GetBanned().size() == 0);
    }

    SECTION("Discouragement is not persisted") {
        {
            BanMan banman(fixture.test_dir, false);  // Disable auto-save for tests

            // Add ban and discouragement
            banman.Ban("192.168.1.1", 3600);
            banman.Discourage("192.168.1.2");

            REQUIRE(banman.Save());
        }

        {
            BanMan banman2(fixture.test_dir, false);  // Disable auto-save for tests
            REQUIRE(banman2.Load());

            // Ban persisted
            REQUIRE(banman2.IsBanned("192.168.1.1"));

            // Discouragement NOT persisted (in-memory only)
            REQUIRE_FALSE(banman2.IsDiscouraged("192.168.1.2"));
        }
    }
}

TEST_CASE("BanMan - JSON File Format", "[network][banman][unit]") {
    BanManTestFixture fixture;

    SECTION("Verify JSON structure") {
        {
            BanMan banman(fixture.test_dir, false);  // Disable auto-save for tests

            // Add bans with different expiry types
            banman.Ban("192.168.1.1", 3600);  // Timed
            banman.Ban("192.168.1.2", 0);     // Permanent

            REQUIRE(banman.Save());
        }

        // Read and parse JSON file
        std::ifstream file(fixture.GetBanlistPath());
        REQUIRE(file.is_open());

        json j;
        file >> j;

        // Check structure - flat object with addresses as keys
        REQUIRE(j.is_object());
        REQUIRE(j.size() == 2);

        // Check individual entries
        REQUIRE(j.contains("192.168.1.1"));
        auto entry1 = j["192.168.1.1"];
        REQUIRE(entry1.contains("version"));
        REQUIRE(entry1.contains("create_time"));
        REQUIRE(entry1.contains("ban_until"));
        REQUIRE(entry1["ban_until"] > 0);  // Timed ban

        REQUIRE(j.contains("192.168.1.2"));
        auto entry2 = j["192.168.1.2"];
        REQUIRE(entry2["version"] == CBanEntry::CURRENT_VERSION);
        REQUIRE(entry2["create_time"] > 0);
        REQUIRE(entry2["ban_until"] == 0);  // Permanent ban
    }

    SECTION("Load corrupted file") {
        // Create corrupted JSON file
        std::ofstream file(fixture.GetBanlistPath());
        file << "{ invalid json ]";
        file.close();

        BanMan banman(fixture.test_dir, false);  // Disable auto-save for tests

        // Should handle error gracefully
        bool loaded = banman.Load();
        // Implementation may return true (ignoring corrupt file) or false
        // Either is acceptable as long as it doesn't crash

        // Should have no bans
        REQUIRE(banman.GetBanned().size() == 0);
    }
}

TEST_CASE("BanMan - Ban Expiration", "[network][banman][unit]") {
    BanMan banman("");

    SECTION("Permanent ban (offset = 0)") {
        banman.Ban("192.168.1.1", 0);

        // Should always be banned
        REQUIRE(banman.IsBanned("192.168.1.1"));

        auto banned = banman.GetBanned();
        REQUIRE(banned.size() == 1);
        REQUIRE(banned["192.168.1.1"].nBanUntil == 0);
    }

    SECTION("Timed ban") {
        banman.Ban("192.168.1.1", 1);  // 1 second ban

        // Immediately after ban, should be banned
        REQUIRE(banman.IsBanned("192.168.1.1"));

        // Wait for expiry (add extra time to handle timing variations)
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        // Should now be expired
        REQUIRE_FALSE(banman.IsBanned("192.168.1.1"));
    }

    SECTION("SweepBanned removes expired") {
        // Add a very short ban
        banman.Ban("192.168.1.1", 1);  // 1 second
        banman.Ban("192.168.1.2", 3600);  // 1 hour

        REQUIRE(banman.GetBanned().size() == 2);

        // Wait for first to expire (add extra time to handle timing variations)
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        // Sweep
        banman.SweepBanned();

        // Only non-expired should remain
        auto banned = banman.GetBanned();
        REQUIRE(banned.size() == 1);
        REQUIRE(banned.find("192.168.1.2") != banned.end());
        REQUIRE(banned.find("192.168.1.1") == banned.end());
    }

    SECTION("Negative offset (treated as permanent)") {
        banman.Ban("192.168.1.1", -100);  // Negative offset treated as permanent

        // Should be permanently banned (implementation treats negative as permanent)
        REQUIRE(banman.IsBanned("192.168.1.1"));

        // Should be in list with ban_until = 0 (permanent)
        auto banned = banman.GetBanned();
        REQUIRE(banned.size() == 1);
        REQUIRE(banned["192.168.1.1"].nBanUntil == 0);

        // SweepBanned should NOT remove it (permanent ban)
        banman.SweepBanned();
        REQUIRE(banman.GetBanned().size() == 1);
    }
}

TEST_CASE("BanMan - CBanEntry", "[network][banman][unit]") {
    SECTION("IsExpired with permanent ban") {
        CBanEntry entry(100, 0);  // nBanUntil = 0 means permanent

        REQUIRE_FALSE(entry.IsExpired(200));
        REQUIRE_FALSE(entry.IsExpired(1000000));
    }

    SECTION("IsExpired with timed ban") {
        CBanEntry entry(100, 500);  // Expires at time 500

        REQUIRE_FALSE(entry.IsExpired(400));  // Before expiry
        REQUIRE(entry.IsExpired(500));        // At expiry
        REQUIRE(entry.IsExpired(600));        // After expiry
    }

    SECTION("Default construction") {
        CBanEntry entry;

        REQUIRE(entry.nVersion == CBanEntry::CURRENT_VERSION);
        REQUIRE(entry.nCreateTime == 0);
        REQUIRE(entry.nBanUntil == 0);

        // Default is permanent ban
        REQUIRE_FALSE(entry.IsExpired(1000000));
    }
}

TEST_CASE("BanMan - Thread Safety", "[network][banman][unit]") {
    BanMan banman("");

    SECTION("Concurrent bans") {
        std::vector<std::thread> threads;

        // Launch 10 threads each banning different IPs
        for (int t = 0; t < 10; t++) {
            threads.emplace_back([&banman, t]() {
                for (int i = 0; i < 10; i++) {
                    std::string ip = "10." + std::to_string(t) + ".0." + std::to_string(i);
                    banman.Ban(ip, 3600);
                }
            });
        }

        // Wait for all threads
        for (auto& t : threads) {
            t.join();
        }

        // Should have 100 bans
        REQUIRE(banman.GetBanned().size() == 100);
    }

    SECTION("Concurrent reads") {
        // Add some bans
        for (int i = 0; i < 10; i++) {
            banman.Ban("10.0.0." + std::to_string(i), 3600);
        }

        std::vector<std::thread> threads;
        std::atomic<int> banned_count(0);

        // Launch threads to check bans
        for (int t = 0; t < 10; t++) {
            threads.emplace_back([&banman, &banned_count]() {
                for (int i = 0; i < 10; i++) {
                    if (banman.IsBanned("10.0.0." + std::to_string(i))) {
                        banned_count++;
                    }
                }
            });
        }

        // Wait for all threads
        for (auto& t : threads) {
            t.join();
        }

        // Each of 10 threads should find all 10 bans
        REQUIRE(banned_count == 100);
    }
}

TEST_CASE("BanMan - Auto-save on destruction", "[network][banman][unit]") {
    BanManTestFixture fixture;

    {
        BanMan banman(fixture.test_dir, false);  // Disable auto-save for tests
        banman.Ban("192.168.1.1", 3600);
        // Destructor should auto-save
    }

    // Check file was created
    REQUIRE(std::filesystem::exists(fixture.GetBanlistPath()));

    // Load and verify
    BanMan banman2(fixture.test_dir, false);  // Disable auto-save for tests
    REQUIRE(banman2.Load());
    REQUIRE(banman2.IsBanned("192.168.1.1"));
}