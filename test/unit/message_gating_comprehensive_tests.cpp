// Copyright (c) 2024 Coinbase Chain
// Comprehensive tests for pre-VERACK message gating (Bitcoin Core parity)
//
// This test suite verifies:
// - All post-VERACK-only messages are properly gated before handshake
// - Gating behavior is consistent across all message types
// - Error handling distinguishes between gating and actual errors
// - Edge cases (null peer, peer lifecycle) are handled correctly

#include "catch_amalgamated.hpp"
#include "network/message_router.hpp"
#include "network/addr_manager.hpp"
#include "network/message.hpp"
#include "network/protocol.hpp"
#include "network/peer.hpp"
#include <boost/asio.hpp>

using namespace coinbasechain;
using namespace coinbasechain::network;
using namespace coinbasechain::message;
using namespace coinbasechain::protocol;

/**
 * Helper fixture to create test peers
 */
class GatingTestFixture {
public:
  boost::asio::io_context io_context;

  PeerPtr create_inbound_peer(int id) {
    auto peer = Peer::create_inbound(
        io_context,
        nullptr,
        0x12345678,
        0
    );
    peer->set_id(id);
    return peer;
  }

  PeerPtr create_outbound_peer(int id, const std::string& addr = "127.0.0.1", uint16_t port = 9590) {
    auto peer = Peer::create_outbound(
        io_context,
        nullptr,
        0x12345678,
        0,
        addr,
        port
    );
    peer->set_id(id);
    return peer;
  }
};

/**
 * Test: Null peer handling
 * Verifies that null peer is properly rejected
 */
TEST_CASE("Pre-VERACK gating: null peer rejection", "[network][gating][edge-case]") {
  MessageRouter router(nullptr, nullptr, nullptr, nullptr);

  // Null peer with any message should return false at RouteMessage level
  auto msg = std::make_unique<GetHeadersMessage>();
  bool result = router.RouteMessage(nullptr, std::move(msg));
  REQUIRE(result == false);
}

/**
 * Test: Gating vs. error distinction
 * Verifies that gated messages return true (not an error)
 * while actual errors return false
 */
TEST_CASE("Pre-VERACK gating: gated returns true, not false", "[network][gating][correctness]") {
  GatingTestFixture fixture;
  AddressManager addr_mgr;
  MessageRouter router(&addr_mgr, nullptr, nullptr);

  auto peer = fixture.create_outbound_peer(1);

  // Pre-VERACK message should return true (gated, not an error)
  auto msg = std::make_unique<InvMessage>();
  bool result = router.RouteMessage(peer, std::move(msg));
  REQUIRE(result == true);  // Gated, returns true (not an error)
}

/**
 * Test: Consistent gating across all post-VERACK-only messages
 * Verifies that GETHEADERS, HEADERS, INV, ADDR, GETADDR all gate consistently
 */
TEST_CASE("Pre-VERACK gating: consistent behavior all message types", "[network][gating][parity]") {
  GatingTestFixture fixture;
  AddressManager addr_mgr;
  MessageRouter router(&addr_mgr, nullptr, nullptr);

  // Create 5 pre-VERACK peers
  std::vector<PeerPtr> peers;
  for (int i = 1; i <= 5; ++i) {
    peers.push_back(fixture.create_outbound_peer(i));
  }

  // Each message type should be gated and return true
  SECTION("GETHEADERS gated pre-VERACK") {
    auto msg = std::make_unique<GetHeadersMessage>();
    REQUIRE(router.RouteMessage(peers[0], std::move(msg)) == true);
  }

  SECTION("HEADERS gated pre-VERACK") {
    auto msg = std::make_unique<HeadersMessage>();
    REQUIRE(router.RouteMessage(peers[1], std::move(msg)) == true);
  }

  SECTION("INV gated pre-VERACK") {
    auto msg = std::make_unique<InvMessage>();
    REQUIRE(router.RouteMessage(peers[2], std::move(msg)) == true);
  }

  SECTION("ADDR gated pre-VERACK") {
    auto msg = std::make_unique<AddrMessage>();
    REQUIRE(router.RouteMessage(peers[3], std::move(msg)) == true);
  }

  SECTION("GETADDR gated pre-VERACK") {
    auto msg = std::make_unique<GetAddrMessage>();
    REQUIRE(router.RouteMessage(peers[4], std::move(msg)) == true);
  }
}

/**
 * Test: Inbound vs outbound peers
 * Verifies gating applies to both inbound and outbound peers
 */
TEST_CASE("Pre-VERACK gating: applies to both inbound and outbound", "[network][gating][connection-types]") {
  GatingTestFixture fixture;
  AddressManager addr_mgr;
  MessageRouter router(&addr_mgr, nullptr, nullptr);

  auto inbound_peer = fixture.create_inbound_peer(1);
  auto outbound_peer = fixture.create_outbound_peer(2);

  SECTION("Gating applies to inbound peers") {
    auto msg = std::make_unique<InvMessage>();
    REQUIRE(router.RouteMessage(inbound_peer, std::move(msg)) == true);
  }

  SECTION("Gating applies to outbound peers") {
    auto msg = std::make_unique<InvMessage>();
    REQUIRE(router.RouteMessage(outbound_peer, std::move(msg)) == true);
  }
}

/**
 * Test: Multiple consecutive pre-VERACK messages
 * Verifies gating works consistently across multiple messages from same peer
 */
TEST_CASE("Pre-VERACK gating: multiple messages from pre-VERACK peer", "[network][gating][sequence]") {
  GatingTestFixture fixture;
  AddressManager addr_mgr;
  MessageRouter router(&addr_mgr, nullptr, nullptr);

  auto peer = fixture.create_outbound_peer(1);

  // Send 5 different message types in sequence, all should be gated
  std::vector<std::unique_ptr<Message>> messages;
  messages.push_back(std::make_unique<GetHeadersMessage>());
  messages.push_back(std::make_unique<HeadersMessage>());
  messages.push_back(std::make_unique<InvMessage>());
  messages.push_back(std::make_unique<AddrMessage>());
  messages.push_back(std::make_unique<GetAddrMessage>());

  for (auto& msg : messages) {
    bool result = router.RouteMessage(peer, std::move(msg));
    REQUIRE(result == true);  // All gated
  }
}

/**
 * Test: VERACK itself is not gated
 * Verifies that VERACK can be processed even from pre-VERACK state
 */
TEST_CASE("Pre-VERACK gating: VERACK is not gated", "[network][gating][handshake]") {
  GatingTestFixture fixture;
  AddressManager addr_mgr;
  MessageRouter router(&addr_mgr, nullptr, nullptr);

  auto peer = fixture.create_outbound_peer(1);

  // VERACK should not be gated (though it would normally be rejected if peer not at right state)
  auto msg = std::make_unique<VerackMessage>();
  bool result = router.RouteMessage(peer, std::move(msg));
  
  // VERACK handler doesn't gate - it has its own checks
  // Should return true (handled) or process normally
  REQUIRE(result == true);
}

/**
 * Test: Gating decision priority
 * Verifies that gating check happens before null manager checks
 * This is important for security - gate regardless of manager availability
 */
TEST_CASE("Pre-VERACK gating: gating before null manager checks", "[network][gating][priority]") {
  GatingTestFixture fixture;
  MessageRouter router(nullptr, nullptr, nullptr, nullptr);  // All managers null

  auto peer = fixture.create_outbound_peer(1);

  // Pre-VERACK message with null managers should return true (gated)
  // not false (manager error)
  SECTION("GETHEADERS gated even with null header_sync_manager") {
    auto msg = std::make_unique<GetHeadersMessage>();
    REQUIRE(router.RouteMessage(peer, std::move(msg)) == true);
  }

  SECTION("HEADERS gated even with null header_sync_manager") {
    auto msg = std::make_unique<HeadersMessage>();
    REQUIRE(router.RouteMessage(peer, std::move(msg)) == true);
  }

  SECTION("INV gated even with null block_relay_manager") {
    auto msg = std::make_unique<InvMessage>();
    REQUIRE(router.RouteMessage(peer, std::move(msg)) == true);
  }

  SECTION("ADDR gated even with null addr_manager") {
    auto msg = std::make_unique<AddrMessage>();
    REQUIRE(router.RouteMessage(peer, std::move(msg)) == true);
  }

  SECTION("GETADDR gated even with null addr_manager") {
    auto msg = std::make_unique<GetAddrMessage>();
    REQUIRE(router.RouteMessage(peer, std::move(msg)) == true);
  }
}

/**
 * Test: Peer state transitions
 * Verifies that successfully_connected() flag controls gating
 */
TEST_CASE("Pre-VERACK gating: peer state flag controls gating", "[network][gating][lifecycle]") {
  GatingTestFixture fixture;
  AddressManager addr_mgr;
  MessageRouter router(&addr_mgr, nullptr, nullptr);

  auto peer = fixture.create_outbound_peer(1);

  // Peer is not successfully_connected by default
  REQUIRE(peer->successfully_connected() == false);
  
  auto msg = std::make_unique<InvMessage>();
  bool result = router.RouteMessage(peer, std::move(msg));
  REQUIRE(result == true);  // Gated because not successfully_connected
}

/**
 * Test: OnPeerDisconnected cleanup
 * Verifies that peer state is properly cleaned up on disconnect
 */
TEST_CASE("Pre-VERACK gating: OnPeerDisconnected cleanup", "[network][gating][lifecycle]") {
  MessageRouter router(nullptr, nullptr, nullptr, nullptr);

  int peer_id = 42;

  // Should not crash when cleaning up peer that never existed
  router.OnPeerDisconnected(peer_id);

  // Multiple cleanups should be idempotent
  router.OnPeerDisconnected(peer_id);

  REQUIRE(true);  // Just verify no crash
}

/**
 * Test: Unknown message types bypass gating (unhandled)
 * Verifies that only specific post-VERACK messages are gated
 * Unknown messages are neither gated nor routed - they just return true
 */
TEST_CASE("Pre-VERACK gating: unknown messages bypass routing", "[network][gating][edge-case]") {
  GatingTestFixture fixture;
  AddressManager addr_mgr;
  MessageRouter router(&addr_mgr, nullptr, nullptr);

  auto peer = fixture.create_outbound_peer(1);

  // PING is not a routed message (handled at Peer level)
  // Unknown messages return true without gating
  auto msg = std::make_unique<PingMessage>(12345);
  bool result = router.RouteMessage(peer, std::move(msg));
  REQUIRE(result == true);  // Unknown messages return true (unhandled, not an error)
}
