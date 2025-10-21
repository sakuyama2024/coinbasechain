// Copyright (c) 2024 Coinbase Chain
// Unit tests for network/message_router.cpp - Message routing logic
//
// These tests verify:
// - Message routing to correct handlers
// - Null pointer handling
// - Unknown message types
// - Handler delegation (verack, addr, getaddr, inv, headers, getheaders)

#include "catch_amalgamated.hpp"
#include "network/message_router.hpp"
#include "network/message.hpp"
#include "network/protocol.hpp"
#include "network/peer.hpp"
#include "network/addr_manager.hpp"
#include <boost/asio.hpp>

using namespace coinbasechain;
using namespace coinbasechain::network;
using namespace coinbasechain::message;

// Note: MessageRouter delegates to actual manager classes (AddressManager, HeaderSyncManager,
// BlockRelayManager). For unit testing, we focus on:
// 1. Routing logic (correct handler for each message type)
// 2. Null pointer handling
// 3. Unknown message handling
// The actual manager integration is tested in integration tests.

// Helper fixture for creating test peers
class MessageRouterTestFixture {
public:
    boost::asio::io_context io_context;

    PeerPtr create_test_peer(int id = 1, bool successfully_connected = true) {
        auto peer = Peer::create_outbound(
            io_context,
            nullptr,
            0x12345678,
            0xabcdef,
            0,  // start_height
            "127.0.0.1",
            8333
        );
        peer->set_id(id);
        // Note: successfully_connected() checks if handshake is complete
        // For unit testing, we can't easily set this without connecting
        // So we'll test with the assumption that peer state is managed externally
        return peer;
    }
};

TEST_CASE("MessageRouter - Construction", "[network][message_router][unit]") {
    AddressManager addr_mgr;

    MessageRouter router(&addr_mgr, nullptr, nullptr);

    // Should construct without error
    REQUIRE(true);
}

TEST_CASE("MessageRouter - Null Message", "[network][message_router][unit]") {
    MessageRouterTestFixture fixture;
    AddressManager addr_mgr;

    MessageRouter router(&addr_mgr, nullptr, nullptr);

    auto peer = fixture.create_test_peer();

    SECTION("Null message pointer") {
        bool result = router.RouteMessage(peer, nullptr);
        REQUIRE_FALSE(result);
    }
}

TEST_CASE("MessageRouter - Null Peer", "[network][message_router][unit]") {
    AddressManager addr_mgr;

    MessageRouter router(&addr_mgr, nullptr, nullptr);

    auto msg = std::make_unique<VerackMessage>();

    SECTION("Null peer pointer") {
        bool result = router.RouteMessage(nullptr, std::move(msg));
        REQUIRE_FALSE(result);
    }
}

TEST_CASE("MessageRouter - Unknown Message Type", "[network][message_router][unit]") {
    MessageRouterTestFixture fixture;
    AddressManager addr_mgr;

    MessageRouter router(&addr_mgr, nullptr, nullptr);

    auto peer = fixture.create_test_peer();

    // Create a ping message (not handled by MessageRouter)
    auto msg = std::make_unique<PingMessage>(12345);

    SECTION("Unknown message returns true (not an error)") {
        bool result = router.RouteMessage(peer, std::move(msg));
        REQUIRE(result);
    }
}

TEST_CASE("MessageRouter - VERACK Message", "[network][message_router][unit]") {
    MessageRouterTestFixture fixture;
    AddressManager addr_mgr;

    MessageRouter router(&addr_mgr, nullptr, nullptr);

    auto peer = fixture.create_test_peer();
    auto msg = std::make_unique<VerackMessage>();

    SECTION("Route VERACK message") {
        bool result = router.RouteMessage(peer, std::move(msg));
        REQUIRE(result);
    }
}

TEST_CASE("MessageRouter - ADDR Message", "[network][message_router][unit]") {
    MessageRouterTestFixture fixture;
    AddressManager addr_mgr;
    
    

    MessageRouter router(&addr_mgr, nullptr, nullptr);

    auto peer = fixture.create_test_peer();

    SECTION("Route ADDR message with addresses") {
        auto msg = std::make_unique<AddrMessage>();

        // Add some test addresses
        protocol::TimestampedAddress addr1;
        addr1.timestamp = 123456;
        addr1.address.services = 1;
        addr1.address.port = 8333;
        msg->addresses.push_back(addr1);

        protocol::TimestampedAddress addr2;
        addr2.timestamp = 123457;
        addr2.address.services = 1;
        addr2.address.port = 8334;
        msg->addresses.push_back(addr2);

        bool result = router.RouteMessage(peer, std::move(msg));

        REQUIRE(result);
        // Addresses are added to AddressManager
    }

    SECTION("Route ADDR message with empty addresses") {
        auto msg = std::make_unique<AddrMessage>();

        bool result = router.RouteMessage(peer, std::move(msg));

        REQUIRE(result);
        // Empty addresses list is handled gracefully
    }
}

TEST_CASE("MessageRouter - GETADDR Message", "[network][message_router][unit]") {
    MessageRouterTestFixture fixture;
    AddressManager addr_mgr;
    
    

    MessageRouter router(&addr_mgr, nullptr, nullptr);

    auto peer = fixture.create_test_peer();

    SECTION("Route GETADDR message") {
        auto msg = std::make_unique<GetAddrMessage>();

        bool result = router.RouteMessage(peer, std::move(msg));

        REQUIRE(result);
        // Response is sent via peer->send_message() with addresses from AddressManager
    }
}

TEST_CASE("MessageRouter - INV Message", "[network][message_router][unit]") {
    MessageRouterTestFixture fixture;
    AddressManager addr_mgr;
    
    

    MessageRouter router(&addr_mgr, nullptr, nullptr);

    auto peer = fixture.create_test_peer();

    SECTION("Route INV message") {
        auto msg = std::make_unique<InvMessage>();

        protocol::InventoryVector inv;
        inv.type = protocol::InventoryType::MSG_BLOCK;
        inv.hash.fill(0xaa);
        msg->inventory.push_back(inv);

        // With null BlockRelayManager, should return false
        bool result = router.RouteMessage(peer, std::move(msg));

        REQUIRE_FALSE(result);
    }
}

TEST_CASE("MessageRouter - HEADERS Message", "[network][message_router][unit]") {
    MessageRouterTestFixture fixture;
    AddressManager addr_mgr;
    
    

    MessageRouter router(&addr_mgr, nullptr, nullptr);

    auto peer = fixture.create_test_peer();

    SECTION("Route HEADERS message") {
        auto msg = std::make_unique<HeadersMessage>();

        // With null HeaderSyncManager, should return false
        bool result = router.RouteMessage(peer, std::move(msg));

        REQUIRE_FALSE(result);
    }
}

TEST_CASE("MessageRouter - GETHEADERS Message", "[network][message_router][unit]") {
    MessageRouterTestFixture fixture;
    AddressManager addr_mgr;
    
    

    MessageRouter router(&addr_mgr, nullptr, nullptr);

    auto peer = fixture.create_test_peer();

    SECTION("Route GETHEADERS message") {
        auto msg = std::make_unique<GetHeadersMessage>();

        // With null HeaderSyncManager, should return false
        bool result = router.RouteMessage(peer, std::move(msg));

        REQUIRE_FALSE(result);
    }
}

TEST_CASE("MessageRouter - Null Manager Handling", "[network][message_router][unit]") {
    MessageRouterTestFixture fixture;

    SECTION("Null AddressManager for ADDR") {
        MessageRouter router(nullptr, nullptr, nullptr);
        auto peer = fixture.create_test_peer();
        auto msg = std::make_unique<AddrMessage>();

        bool result = router.RouteMessage(peer, std::move(msg));
        REQUIRE_FALSE(result);
    }

    SECTION("Null AddressManager for GETADDR") {
        MessageRouter router(nullptr, nullptr, nullptr);
        auto peer = fixture.create_test_peer();
        auto msg = std::make_unique<GetAddrMessage>();

        bool result = router.RouteMessage(peer, std::move(msg));
        REQUIRE_FALSE(result);
    }

    SECTION("Null BlockRelayManager for INV") {
        MessageRouter router(nullptr, nullptr, nullptr);
        auto peer = fixture.create_test_peer();
        auto msg = std::make_unique<InvMessage>();

        bool result = router.RouteMessage(peer, std::move(msg));
        REQUIRE_FALSE(result);
    }

    SECTION("Null HeaderSyncManager for HEADERS") {
        MessageRouter router(nullptr, nullptr, nullptr);
        auto peer = fixture.create_test_peer();
        auto msg = std::make_unique<HeadersMessage>();

        bool result = router.RouteMessage(peer, std::move(msg));
        REQUIRE_FALSE(result);
    }

    SECTION("Null HeaderSyncManager for GETHEADERS") {
        MessageRouter router(nullptr, nullptr, nullptr);
        auto peer = fixture.create_test_peer();
        auto msg = std::make_unique<GetHeadersMessage>();

        bool result = router.RouteMessage(peer, std::move(msg));
        REQUIRE_FALSE(result);
    }

    SECTION("Null managers for VERACK (should still succeed)") {
        MessageRouter router(nullptr, nullptr, nullptr);
        auto peer = fixture.create_test_peer();
        auto msg = std::make_unique<VerackMessage>();

        bool result = router.RouteMessage(peer, std::move(msg));
        REQUIRE(result);  // VERACK handler succeeds even with null managers
    }
}

TEST_CASE("MessageRouter - Multiple Messages", "[network][message_router][unit]") {
    MessageRouterTestFixture fixture;
    AddressManager addr_mgr;
    
    

    MessageRouter router(&addr_mgr, nullptr, nullptr);

    auto peer = fixture.create_test_peer();

    SECTION("Route multiple different message types") {
        // ADDR
        auto msg1 = std::make_unique<AddrMessage>();
        REQUIRE(router.RouteMessage(peer, std::move(msg1)));

        // GETADDR
        auto msg2 = std::make_unique<GetAddrMessage>();
        REQUIRE(router.RouteMessage(peer, std::move(msg2)));

        // INV (with null manager, should fail)
        auto msg3 = std::make_unique<InvMessage>();
        REQUIRE_FALSE(router.RouteMessage(peer, std::move(msg3)));

        // HEADERS (with null manager, should fail)
        auto msg4 = std::make_unique<HeadersMessage>();
        REQUIRE_FALSE(router.RouteMessage(peer, std::move(msg4)));

        // GETHEADERS (with null manager, should fail)
        auto msg5 = std::make_unique<GetHeadersMessage>();
        REQUIRE_FALSE(router.RouteMessage(peer, std::move(msg5)));

        // VERACK
        auto msg6 = std::make_unique<VerackMessage>();
        REQUIRE(router.RouteMessage(peer, std::move(msg6)));
    }
}

TEST_CASE("MessageRouter - Message Command Names", "[network][message_router][unit]") {
    SECTION("Verify command names match protocol") {
        VerackMessage verack;
        REQUIRE(verack.command() == protocol::commands::VERACK);

        AddrMessage addr;
        REQUIRE(addr.command() == protocol::commands::ADDR);

        GetAddrMessage getaddr;
        REQUIRE(getaddr.command() == protocol::commands::GETADDR);

        InvMessage inv;
        REQUIRE(inv.command() == protocol::commands::INV);

        HeadersMessage headers;
        REQUIRE(headers.command() == protocol::commands::HEADERS);

        GetHeadersMessage getheaders;
        REQUIRE(getheaders.command() == protocol::commands::GETHEADERS);
    }
}

TEST_CASE("MessageRouter - Edge Cases", "[network][message_router][unit]") {
    MessageRouterTestFixture fixture;
    AddressManager addr_mgr;
    
    

    MessageRouter router(&addr_mgr, nullptr, nullptr);

    SECTION("ADDR message with many addresses") {
        auto peer = fixture.create_test_peer();
        auto msg = std::make_unique<AddrMessage>();

        // Add 100 addresses
        for (int i = 0; i < 100; i++) {
            protocol::TimestampedAddress addr;
            addr.timestamp = 123456 + i;
            addr.address.services = 1;
            addr.address.port = 8333 + i;
            msg->addresses.push_back(addr);
        }

        bool result = router.RouteMessage(peer, std::move(msg));

        REQUIRE(result);
        // All 100 addresses are added to AddressManager
    }

    SECTION("Multiple PONG messages (unhandled)") {
        auto peer = fixture.create_test_peer();

        for (int i = 0; i < 5; i++) {
            auto msg = std::make_unique<PongMessage>(i);
            bool result = router.RouteMessage(peer, std::move(msg));
            REQUIRE(result);  // Should succeed even though unhandled
        }
    }
}

TEST_CASE("MessageRouter - Different Peer IDs", "[network][message_router][unit]") {
    MessageRouterTestFixture fixture;
    AddressManager addr_mgr;
    
    

    MessageRouter router(&addr_mgr, nullptr, nullptr);

    SECTION("Route messages from different peers") {
        auto peer1 = fixture.create_test_peer(1);
        auto peer2 = fixture.create_test_peer(2);
        auto peer3 = fixture.create_test_peer(3);

        auto msg1 = std::make_unique<AddrMessage>();
        auto msg2 = std::make_unique<AddrMessage>();
        auto msg3 = std::make_unique<AddrMessage>();

        REQUIRE(router.RouteMessage(peer1, std::move(msg1)));
        REQUIRE(router.RouteMessage(peer2, std::move(msg2)));
        REQUIRE(router.RouteMessage(peer3, std::move(msg3)));

        // All messages routed successfully
    }
}
