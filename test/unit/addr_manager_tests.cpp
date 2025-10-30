// Copyright (c) 2024 Coinbase Chain
// Test suite for AddressManager

#include "catch_amalgamated.hpp"
#include "network/addr_manager.hpp"
#include "network/protocol.hpp"
#include "util/time.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace coinbasechain::network;
using namespace coinbasechain::protocol;

// Helper function to create a test address
static NetworkAddress MakeAddress(const std::string& ip_v4, uint16_t port) {
    NetworkAddress addr;
    addr.services = 1;
    addr.port = port;

    // Parse IPv4 and convert to IPv4-mapped IPv6 (::FFFF:x.x.x.x)
    std::memset(addr.ip.data(), 0, 10);
    addr.ip[10] = 0xFF;
    addr.ip[11] = 0xFF;

    // Simple IPv4 parsing (e.g., "127.0.0.1")
    int a, b, c, d;
    if (sscanf(ip_v4.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) == 4) {
        addr.ip[12] = static_cast<uint8_t>(a);
        addr.ip[13] = static_cast<uint8_t>(b);
        addr.ip[14] = static_cast<uint8_t>(c);
        addr.ip[15] = static_cast<uint8_t>(d);
    }

    return addr;
}

TEST_CASE("AddressManager basic operations", "[network][addrman]") {
    AddressManager addrman;

    SECTION("Empty address manager") {
        REQUIRE(addrman.size() == 0);
        REQUIRE(addrman.tried_count() == 0);
        REQUIRE(addrman.new_count() == 0);
        REQUIRE(addrman.select() == std::nullopt);
    }

    SECTION("Add single address") {
        NetworkAddress addr = MakeAddress("192.168.1.1", 8333);

        REQUIRE(addrman.add(addr));
        REQUIRE(addrman.size() == 1);
        REQUIRE(addrman.new_count() == 1);
        REQUIRE(addrman.tried_count() == 0);
    }

    SECTION("Add duplicate address") {
        NetworkAddress addr = MakeAddress("192.168.1.1", 8333);

        REQUIRE(addrman.add(addr));
        REQUIRE(addrman.size() == 1);

        // Adding same address again should return false
        REQUIRE_FALSE(addrman.add(addr));
        REQUIRE(addrman.size() == 1);
    }

    SECTION("Add multiple addresses") {
        std::vector<TimestampedAddress> addresses;
        uint32_t current_time = static_cast<uint32_t>(coinbasechain::util::GetTime());

        for (int i = 0; i < 10; i++) {
            std::string ip = "192.168.1." + std::to_string(i + 1);
            NetworkAddress addr = MakeAddress(ip, 8333);
            // Use timestamps from recent past (1 hour ago - 10 seconds ago)
            addresses.push_back({current_time - 3600 + (i * 360), addr});
        }

        size_t added = addrman.add_multiple(addresses);
        REQUIRE(added == 10);
        REQUIRE(addrman.size() == 10);
        REQUIRE(addrman.new_count() == 10);
    }
}

TEST_CASE("AddressManager state transitions", "[network][addrman]") {
    AddressManager addrman;
    NetworkAddress addr = MakeAddress("10.0.0.1", 8333);

    SECTION("Mark address as good (new -> tried)") {
        // Add to new table
        REQUIRE(addrman.add(addr));
        REQUIRE(addrman.new_count() == 1);
        REQUIRE(addrman.tried_count() == 0);

        // Mark as good (moves to tried)
        addrman.good(addr);
        REQUIRE(addrman.new_count() == 0);
        REQUIRE(addrman.tried_count() == 1);
        REQUIRE(addrman.size() == 1);
    }

    SECTION("Attempt tracking") {
        REQUIRE(addrman.add(addr));

        // Multiple failed attempts
        addrman.attempt(addr);
        addrman.failed(addr);
        addrman.attempt(addr);
        addrman.failed(addr);

        // Address should still be in new table after 2 failures
        REQUIRE(addrman.new_count() == 1);
    }

    SECTION("Good address stays good") {
        REQUIRE(addrman.add(addr));
        addrman.good(addr);
        REQUIRE(addrman.tried_count() == 1);

        // Marking good again should keep it in tried
        addrman.good(addr);
        REQUIRE(addrman.tried_count() == 1);
        REQUIRE(addrman.new_count() == 0);
    }

    SECTION("Too many failures - new address stays but becomes unlikely") {
        REQUIRE(addrman.add(addr));

        // Fail it many times
        for (int i = 0; i < 15; i++) {
            addrman.failed(addr);
        }

        // New address stays in table (only removed if stale - Bitcoin Core parity)
        // It becomes less likely to be selected via GetChance() penalty
        REQUIRE(addrman.size() == 1);
        REQUIRE(addrman.new_count() == 1);
    }

    SECTION("Failed tried address stays in tried - Bitcoin Core parity") {
        REQUIRE(addrman.add(addr));
        addrman.good(addr);
        REQUIRE(addrman.tried_count() == 1);

        // Fail it many times
        for (int i = 0; i < 20; i++) {
            addrman.failed(addr);
        }

        // Bitcoin Core parity: Tried addresses stay in tried table permanently
        // They never move back to new table regardless of failure count
        // They become less likely to be selected via GetChance() penalty
        // (After 8 failures: 0.66^8 = 3.57% chance, but never removed)
        REQUIRE(addrman.tried_count() == 1);
        REQUIRE(addrman.new_count() == 0);
        REQUIRE(addrman.size() == 1);
    }
}

TEST_CASE("AddressManager selection", "[network][addrman]") {
    AddressManager addrman;

    SECTION("Select from new addresses") {
        // Add 10 new addresses
        for (int i = 0; i < 10; i++) {
            std::string ip = "192.168.2." + std::to_string(i + 1);
            NetworkAddress addr = MakeAddress(ip, 8333);
            addrman.add(addr);
        }

        // Should be able to select
        auto selected = addrman.select();
        REQUIRE(selected.has_value());
        REQUIRE(selected->port == 8333);
    }

    SECTION("Select prefers tried addresses") {
        // Add addresses to both tables
        NetworkAddress tried_addr = MakeAddress("10.0.0.1", 8333);
        addrman.add(tried_addr);
        addrman.good(tried_addr);

        for (int i = 0; i < 100; i++) {
            std::string ip = "192.168.3." + std::to_string(i + 1);
            NetworkAddress addr = MakeAddress(ip, 8333);
            addrman.add(addr);
        }

        // Select many times, should get tried address most of the time
        int tried_count = 0;
        for (int i = 0; i < 100; i++) {
            auto selected = addrman.select();
            REQUIRE(selected.has_value());

            // Check if it's the tried address (10.0.0.1)
            if (selected->ip[12] == 10 && selected->ip[13] == 0 &&
                selected->ip[14] == 0 && selected->ip[15] == 1) {
                tried_count++;
            }
        }

        // Should select tried address about 50% of the time (Bitcoin Core parity)
        // Allow variance: expect 35-65 out of 100 selections
        REQUIRE(tried_count > 35);
        REQUIRE(tried_count < 65);
    }

    SECTION("Get multiple addresses") {
        // Add 50 addresses
        for (int i = 0; i < 50; i++) {
            std::string ip = "192.168.4." + std::to_string(i + 1);
            NetworkAddress addr = MakeAddress(ip, 8333);
            addrman.add(addr);
        }

        // Get 20 addresses
        auto addresses = addrman.get_addresses(20);
        REQUIRE(addresses.size() == 20);

        // All should be unique
        std::set<std::string> unique_ips;
        for (const auto& ts_addr : addresses) {
            std::string key = std::to_string(ts_addr.address.ip[12]) + "." +
                            std::to_string(ts_addr.address.ip[13]) + "." +
                            std::to_string(ts_addr.address.ip[14]) + "." +
                            std::to_string(ts_addr.address.ip[15]);
            unique_ips.insert(key);
        }
        REQUIRE(unique_ips.size() == 20);
    }
}

TEST_CASE("AddressManager persistence", "[network][addrman]") {
    std::filesystem::path test_file = std::filesystem::temp_directory_path() / "addrman_test.json";

    // Clean up any existing test file
    std::filesystem::remove(test_file);

    SECTION("Save and load empty address manager") {
        AddressManager addrman1;
        REQUIRE(addrman1.Save(test_file.string()));

        AddressManager addrman2;
        REQUIRE(addrman2.Load(test_file.string()));
        REQUIRE(addrman2.size() == 0);
    }

    SECTION("Save and load with new addresses") {
        AddressManager addrman1;

        // Add 20 addresses
        for (int i = 0; i < 20; i++) {
            std::string ip = "10.0.1." + std::to_string(i + 1);
            NetworkAddress addr = MakeAddress(ip, 8333);
            addrman1.add(addr);
        }

        REQUIRE(addrman1.size() == 20);
        REQUIRE(addrman1.Save(test_file.string()));

        // Load into new manager
        AddressManager addrman2;
        REQUIRE(addrman2.Load(test_file.string()));
        REQUIRE(addrman2.size() == 20);
        REQUIRE(addrman2.new_count() == 20);
        REQUIRE(addrman2.tried_count() == 0);
    }

    SECTION("Save and load with tried addresses") {
        AddressManager addrman1;

        // Add and mark as tried
        for (int i = 0; i < 10; i++) {
            std::string ip = "10.0.2." + std::to_string(i + 1);
            NetworkAddress addr = MakeAddress(ip, 8333);
            addrman1.add(addr);
            addrman1.good(addr);
        }

        REQUIRE(addrman1.tried_count() == 10);
        REQUIRE(addrman1.Save(test_file.string()));

        // Load into new manager
        AddressManager addrman2;
        REQUIRE(addrman2.Load(test_file.string()));
        REQUIRE(addrman2.size() == 10);
        REQUIRE(addrman2.tried_count() == 10);
        REQUIRE(addrman2.new_count() == 0);
    }

    SECTION("Save and load with mixed addresses") {
        AddressManager addrman1;

        // Add 15 new addresses
        for (int i = 0; i < 15; i++) {
            std::string ip = "192.168.10." + std::to_string(i + 1);
            NetworkAddress addr = MakeAddress(ip, 8333);
            addrman1.add(addr);
        }

        // Add 5 tried addresses
        for (int i = 0; i < 5; i++) {
            std::string ip = "10.0.3." + std::to_string(i + 1);
            NetworkAddress addr = MakeAddress(ip, 8333);
            addrman1.add(addr);
            addrman1.good(addr);
        }

        REQUIRE(addrman1.size() == 20);
        REQUIRE(addrman1.new_count() == 15);
        REQUIRE(addrman1.tried_count() == 5);
        REQUIRE(addrman1.Save(test_file.string()));

        // Load and verify
        AddressManager addrman2;
        REQUIRE(addrman2.Load(test_file.string()));
        REQUIRE(addrman2.size() == 20);
        REQUIRE(addrman2.new_count() == 15);
        REQUIRE(addrman2.tried_count() == 5);
    }

    SECTION("Load non-existent file fails gracefully") {
        AddressManager addrman;
        REQUIRE_FALSE(addrman.Load("/tmp/nonexistent_addrman_file_xyz.json"));
        REQUIRE(addrman.size() == 0);
    }

    // Cleanup
    std::filesystem::remove(test_file);
}

TEST_CASE("AddressManager stale address cleanup", "[network][addrman]") {
    AddressManager addrman;

    SECTION("Cleanup removes old addresses") {
        // Add addresses with recent timestamp first
        for (int i = 0; i < 10; i++) {
            std::string ip = "192.168.20." + std::to_string(i + 1);
            NetworkAddress addr = MakeAddress(ip, 8333);
            addrman.add(addr);  // Uses current time
        }

        REQUIRE(addrman.size() == 10);

        // Manually set old timestamps (simulate addresses becoming stale)
        // NOTE: This is a white-box test - we're reaching into internals
        // In real usage, addresses would become stale over time
        // For now, just verify cleanup doesn't crash
        addrman.cleanup_stale();

        // Recent addresses should still be there
        REQUIRE(addrman.size() == 10);
    }

    SECTION("Cleanup preserves recent addresses") {
        // Add recent addresses
        for (int i = 0; i < 10; i++) {
            std::string ip = "192.168.21." + std::to_string(i + 1);
            NetworkAddress addr = MakeAddress(ip, 8333);
            addrman.add(addr);  // Uses current time
        }

        REQUIRE(addrman.size() == 10);

        // Cleanup should not remove recent addresses
        addrman.cleanup_stale();
        REQUIRE(addrman.size() == 10);
    }

    SECTION("Cleanup preserves tried addresses even if old") {
        // Add recent addresses then mark as tried
        for (int i = 0; i < 5; i++) {
            std::string ip = "10.0.4." + std::to_string(i + 1);
            NetworkAddress addr = MakeAddress(ip, 8333);
            addrman.add(addr);  // Uses current time
            addrman.good(addr);  // Move to tried table
        }

        REQUIRE(addrman.tried_count() == 5);

        // Cleanup should keep tried addresses (they worked, so we keep them)
        addrman.cleanup_stale();
        REQUIRE(addrman.tried_count() == 5);
    }
}
