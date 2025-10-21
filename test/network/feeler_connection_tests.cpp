// Feeler connection tests
#include "network_test_helpers.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;
using namespace coinbasechain::network;

// Helper to create IPv4 NetworkAddress from string like "127.0.0.2"
protocol::NetworkAddress make_address(const std::string& ip_str, uint16_t port) {
    // Parse IP string like "127.0.0.2" to bytes
    std::vector<uint8_t> bytes;
    size_t start = 0;
    for (int i = 0; i < 4; i++) {
        size_t dot = ip_str.find('.', start);
        std::string part = (dot == std::string::npos) ? ip_str.substr(start) : ip_str.substr(start, dot - start);
        bytes.push_back(static_cast<uint8_t>(std::stoi(part)));
        start = dot + 1;
    }

    // Convert to uint32_t (network byte order - big endian)
    uint32_t ipv4 = (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];

    return protocol::NetworkAddress::from_ipv4(0, ipv4, port);
}

TEST_CASE("Feeler connections - basic functionality", "[network][feeler]") {
    SimulatedNetwork network(12345);
    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    SECTION("Feeler connection attempt is made") {
        // Add node2's address to node1's address manager "new" table
        protocol::NetworkAddress addr2 = make_address(node2.GetAddress(), node2.GetPort());
        node1.GetNetworkManager().address_manager().add(addr2);

        // Trigger feeler connection
        node1.GetNetworkManager().attempt_feeler_connection();

        // Process events to execute deferred callbacks
        node1.ProcessEvents();
        node2.ProcessEvents();

        // Advance time gradually to allow VERSION/VERACK handshake
        // (gradual advancement ensures messages queued during processing are delivered)
        for (int i = 0; i < 10; i++) {
            network.AdvanceTime(100 * (i + 1));
        }

        // Feeler should auto-disconnect after handshake completes
        CHECK(node1.GetPeerCount() == 0);
        CHECK(node2.GetPeerCount() == 0);
    }

    SECTION("Feelers don't count against outbound limit") {
        size_t outbound_before = node1.GetNetworkManager().outbound_peer_count();
        CHECK(outbound_before == 0);

        protocol::NetworkAddress feeler_addr = make_address(node2.GetAddress(), node2.GetPort());
        node1.GetNetworkManager().address_manager().add(feeler_addr);

        node1.GetNetworkManager().attempt_feeler_connection();
        node1.ProcessEvents();
        node2.ProcessEvents();

        for (int i = 0; i < 5; i++) {
            network.AdvanceTime(50 * (i + 1));
        }

        // Feelers don't count toward outbound limit
        size_t outbound_with_feeler = node1.GetNetworkManager().outbound_peer_count();
        CHECK(outbound_with_feeler == 0);
    }

    SECTION("Feeler selects from 'new' table only") {
        protocol::NetworkAddress addr_new = make_address(node2.GetAddress(), node2.GetPort());
        node1.GetNetworkManager().address_manager().add(addr_new);

        // Verify address is selectable from "new" table
        auto selected = node1.GetNetworkManager().address_manager().select_new_for_feeler();
        REQUIRE(selected.has_value());
        CHECK(selected->port == node2.GetPort());

        node1.GetNetworkManager().attempt_feeler_connection();
        node1.ProcessEvents();
        node2.ProcessEvents();

        for (int i = 0; i < 5; i++) {
            network.AdvanceTime(50 * (i + 1));
        }
    }

    SECTION("No feeler when 'new' table is empty") {
        size_t peers_before = node1.GetNetworkManager().active_peer_count();

        node1.GetNetworkManager().attempt_feeler_connection();
        node1.ProcessEvents();
        node2.ProcessEvents();

        for (int i = 0; i < 5; i++) {
            network.AdvanceTime(50 * (i + 1));
        }

        // No connection should be made
        size_t peers_after = node1.GetNetworkManager().active_peer_count();
        CHECK(peers_after == peers_before);
    }
}

TEST_CASE("Feeler connections - connection type tracking", "[network][feeler]") {
    SimulatedNetwork network(12345);
    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    SECTION("Feeler has correct ConnectionType") {
        protocol::NetworkAddress addr2 = make_address(node2.GetAddress(), node2.GetPort());
        node1.GetNetworkManager().address_manager().add(addr2);

        node1.GetNetworkManager().attempt_feeler_connection();
        node1.ProcessEvents();
        node2.ProcessEvents();

        network.AdvanceTime(50);

        auto& peer_mgr = node1.GetNetworkManager().peer_manager();
        auto peers = peer_mgr.get_all_peers();

        bool found_feeler = false;
        for (auto& peer : peers) {
            if (peer && peer->is_feeler()) {
                found_feeler = true;
                CHECK(peer->connection_type() == ConnectionType::FEELER);
                CHECK(ConnectionTypeAsString(peer->connection_type()) == "feeler");
            }
        }

        if (!peers.empty()) {
            CHECK(found_feeler);
        }
    }

    SECTION("Regular outbound connections are not feelers") {
        protocol::NetworkAddress addr2 = make_address(node2.GetAddress(), node2.GetPort());

        node1.GetNetworkManager().connect_to(addr2);
        node1.ProcessEvents();
        node2.ProcessEvents();

        network.AdvanceTime(50);

        auto& peer_mgr = node1.GetNetworkManager().peer_manager();
        auto peers = peer_mgr.get_all_peers();

        for (auto& peer : peers) {
            if (peer && !peer->is_inbound()) {
                CHECK_FALSE(peer->is_feeler());
                CHECK(peer->connection_type() == ConnectionType::OUTBOUND);
            }
        }
    }
}

TEST_CASE("ConnectionType string conversion", "[network][feeler]") {
    CHECK(ConnectionTypeAsString(ConnectionType::INBOUND) == "inbound");
    CHECK(ConnectionTypeAsString(ConnectionType::OUTBOUND) == "outbound");
    CHECK(ConnectionTypeAsString(ConnectionType::MANUAL) == "manual");
    CHECK(ConnectionTypeAsString(ConnectionType::FEELER) == "feeler");
}

TEST_CASE("AddrManager feeler support", "[network][feeler]") {
    SimulatedNetwork network(12345);
    SimulatedNode node(1, &network);

    SECTION("select_new_for_feeler returns address from new table") {
        // Initially empty
        auto addr1 = node.GetNetworkManager().address_manager().select_new_for_feeler();
        CHECK_FALSE(addr1.has_value());

        // Add address
        protocol::NetworkAddress test_addr = make_address("192.168.1.1", 8333);
        node.GetNetworkManager().address_manager().add(test_addr);

        // Should now return an address
        auto addr2 = node.GetNetworkManager().address_manager().select_new_for_feeler();
        REQUIRE(addr2.has_value());
        CHECK(addr2->port == 8333);
    }
}
