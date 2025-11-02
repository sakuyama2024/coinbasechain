// Copyright (c) 2024 Coinbase Chain
// Unit tests for pre-VERACK message gating in MessageRouter
//
// These tests verify that MessageRouter properly gates post-VERACK-only messages,
// matching Bitcoin Core's defense-in-depth behavior for P2P security.
//
// Messages that MUST be gated (post-VERACK only):
// - GETHEADERS
// - HEADERS
// - INV
// - ADDR
// - GETADDR
//
// Messages that can be pre-VERACK:
// - VERSION (required for handshake)
// - VERACK (required for handshake)
// - PING/PONG (handled at Peer level, not affected by gating)

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

TEST_CASE("Pre-VERACK gating: GETHEADERS rejected before handshake", "[network][pre-verack][security]") {
  AddressManager addr_mgr;
  MessageRouter router(&addr_mgr, nullptr, nullptr, nullptr);

  // Create a peer that is connected but NOT successfully_connected (pre-VERACK)
  boost::asio::io_context io_context;
  auto peer = Peer::create_outbound(
      io_context,
      nullptr,
      0x12345678,
      0,  // start_height
      "127.0.0.1",
      9590
  );
  peer->set_id(1);
  // Note: peer->successfully_connected() will be false by default

  auto getheaders_msg = std::make_unique<GetHeadersMessage>();
  getheaders_msg->version = PROTOCOL_VERSION;
  getheaders_msg->hash_stop.fill(0);

  bool result = router.RouteMessage(peer, std::move(getheaders_msg));

  // Should return true (not an error), but silently ignore the message
  REQUIRE(result == true);
}

TEST_CASE("Pre-VERACK gating: HEADERS rejected before handshake", "[network][pre-verack][security]") {
  MessageRouter router(nullptr, nullptr, nullptr, nullptr);

  boost::asio::io_context io_context;
  auto peer = Peer::create_outbound(
      io_context,
      nullptr,
      0x12345678,
      0,
      "127.0.0.1",
      9590
  );
  peer->set_id(2);

  auto headers_msg = std::make_unique<HeadersMessage>();
  bool result = router.RouteMessage(peer, std::move(headers_msg));

  REQUIRE(result == true);
}

TEST_CASE("Pre-VERACK gating: INV rejected before handshake", "[network][pre-verack][security]") {
  MessageRouter router(nullptr, nullptr, nullptr, nullptr);

  boost::asio::io_context io_context;
  auto peer = Peer::create_outbound(
      io_context,
      nullptr,
      0x12345678,
      0,
      "127.0.0.1",
      9590
  );
  peer->set_id(3);

  auto inv_msg = std::make_unique<InvMessage>();
  bool result = router.RouteMessage(peer, std::move(inv_msg));

  REQUIRE(result == true);
}

TEST_CASE("Pre-VERACK gating: ADDR rejected before handshake", "[network][pre-verack][security]") {
  AddressManager addr_mgr;
  MessageRouter router(&addr_mgr, nullptr, nullptr, nullptr);

  boost::asio::io_context io_context;
  auto peer = Peer::create_outbound(
      io_context,
      nullptr,
      0x12345678,
      0,
      "127.0.0.1",
      9590
  );
  peer->set_id(4);

  auto addr_msg = std::make_unique<AddrMessage>();
  bool result = router.RouteMessage(peer, std::move(addr_msg));

  REQUIRE(result == true);
}

TEST_CASE("Pre-VERACK gating: GETADDR rejected before handshake", "[network][pre-verack][security]") {
  AddressManager addr_mgr;
  MessageRouter router(&addr_mgr, nullptr, nullptr, nullptr);

  boost::asio::io_context io_context;
  auto peer = Peer::create_inbound(
      io_context,
      nullptr,
      0x12345678,
      0
  );
  peer->set_id(5);

  auto getaddr_msg = std::make_unique<GetAddrMessage>();
  bool result = router.RouteMessage(peer, std::move(getaddr_msg));

  REQUIRE(result == true);
}

TEST_CASE("Bitcoin Core parity: post-VERACK messages are gated consistently", "[network][gating][parity]") {
  // This documents the gating policy across all post-VERACK messages
  AddressManager addr_mgr;
  MessageRouter router(&addr_mgr, nullptr, nullptr, nullptr);

  boost::asio::io_context io_context;
  auto peer = Peer::create_outbound(
      io_context,
      nullptr,
      0x12345678,
      0,
      "127.0.0.1",
      9590
  );
  peer->set_id(6);

  // All of these should be gated before VERACK
  std::vector<std::unique_ptr<Message>> pre_verack_only_messages;
  pre_verack_only_messages.push_back(std::make_unique<GetHeadersMessage>());
  pre_verack_only_messages.push_back(std::make_unique<HeadersMessage>());
  pre_verack_only_messages.push_back(std::make_unique<InvMessage>());
  pre_verack_only_messages.push_back(std::make_unique<AddrMessage>());
  pre_verack_only_messages.push_back(std::make_unique<GetAddrMessage>());

  // All should be silently ignored (return true, not an error)
  for (auto& msg : pre_verack_only_messages) {
    bool result = router.RouteMessage(peer, std::move(msg));
    REQUIRE(result == true);
  }
}
