// Copyright (c) 2024 Coinbase Chain
// Unit tests for network/nat_manager.cpp - UPnP NAT traversal
//
// These tests verify:
// - Basic lifecycle (construction, stop)
// - API behavior (GetExternalIP, GetExternalPort, IsPortMapped)
// - State management
//
// NOTE: Tests that call Start() require actual UPnP hardware and are slow (2+ seconds)
// Those are kept in integration tests tagged with [.] to skip by default

#include <catch_amalgamated.hpp>
#include "network/nat_manager.hpp"
#include <thread>

using namespace coinbasechain::network;

TEST_CASE("NAT Manager - Basic Construction", "[nat][network]") {
    SECTION("Can construct and destruct NAT manager") {
        NATManager manager;
        // Should not crash
        REQUIRE(true);
    }

    SECTION("Initial state is not mapped") {
        NATManager manager;
        REQUIRE_FALSE(manager.IsPortMapped());
    }

    SECTION("Initial external IP is empty") {
        NATManager manager;
        REQUIRE(manager.GetExternalIP().empty());
    }

    SECTION("Initial external port is 0") {
        NATManager manager;
        REQUIRE(manager.GetExternalPort() == 0);
    }
}

TEST_CASE("NAT Manager - Stop without Start", "[nat][network]") {
    SECTION("Stop without Start is safe") {
        NATManager manager;
        manager.Stop();  // Should not crash
        REQUIRE_FALSE(manager.IsPortMapped());
    }

    SECTION("Multiple stops are safe") {
        NATManager manager;
        manager.Stop();
        manager.Stop();
        manager.Stop();
        REQUIRE_FALSE(manager.IsPortMapped());
    }
}

TEST_CASE("NAT Manager - Destructor", "[nat][network]") {
    SECTION("Destructor does not crash") {
        {
            NATManager manager;
            // manager goes out of scope here, destructor should cleanup
        }

        // If we reach here without hanging, the destructor worked
        REQUIRE(true);
    }
}

TEST_CASE("NAT Manager - Thread Safety", "[nat][network]") {
    SECTION("Concurrent stops are safe") {
        NATManager manager;

        std::vector<std::thread> threads;
        for (int i = 0; i < 5; ++i) {
            threads.emplace_back([&manager]() {
                manager.Stop();
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        REQUIRE_FALSE(manager.IsPortMapped());
    }
}

// ===== INTEGRATION TESTS (SLOW - require actual UPnP hardware) =====
// Tagged with [.] to skip by default
// Run with: ./coinbasechain_tests "[nat][integration]"

TEST_CASE("NAT Manager - UPnP Integration", "[nat][integration][.]") {
    NATManager manager;
    uint16_t test_port = 39994;

    SECTION("Full UPnP workflow") {
        bool started = manager.Start(test_port);

        // This test only makes sense if we have UPnP
        if (!started) {
            SKIP("No UPnP-capable gateway found");
        }

        REQUIRE(started);
        REQUIRE(manager.IsPortMapped());

        // Should have external IP
        std::string external_ip = manager.GetExternalIP();
        REQUIRE_FALSE(external_ip.empty());
        INFO("External IP: " << external_ip);

        // Should have external port
        uint16_t external_port = manager.GetExternalPort();
        REQUIRE(external_port > 0);
        REQUIRE(external_port == test_port);  // Should map to same port
        INFO("External Port: " << external_port);

        // Cleanup
        manager.Stop();
        REQUIRE_FALSE(manager.IsPortMapped());
    }
}

TEST_CASE("NAT Manager - Start Twice", "[nat][integration][.]") {
    SECTION("Cannot start twice") {
        NATManager manager;
        uint16_t test_port = 39998;

        bool first_start = manager.Start(test_port);
        bool second_start = manager.Start(test_port + 1);

        // Second start should fail
        REQUIRE_FALSE(second_start);

        if (first_start) {
            manager.Stop();
        }
    }
}
