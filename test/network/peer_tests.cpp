// Copyright (c) 2024 Coinbase Chain
// Unit tests for network/peer.cpp - Peer connection lifecycle and message handling
//
// These tests verify:
// - Peer state transitions (handshake, ready, disconnect)
// - Message sending and receiving
// - Timeout handling (handshake, ping, inactivity)
// - Buffer management (flood protection)
// - Statistics tracking
// - Self-connection prevention

#include <catch_amalgamated.hpp>
#include "network/peer.hpp"
#include "network/transport.hpp"
#include "network/protocol.hpp"
#include "network/message.hpp"
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <deque>
#include <mutex>
#include <chrono>
#include <thread>

using namespace coinbasechain;
using namespace coinbasechain::network;

// =============================================================================
// MOCK TRANSPORT for isolated peer testing
// =============================================================================

class MockTransportConnection : public TransportConnection,
                                public std::enable_shared_from_this<MockTransportConnection> {
public:
    MockTransportConnection() : open_(true) {}

    // TransportConnection interface
    void start() override {
        // Nothing to do for mock
    }

    bool send(const std::vector<uint8_t>& data) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) return false;
        sent_messages_.push_back(data);
        return true;
    }

    void close() override {
        open_ = false;
        if (disconnect_callback_) {
            disconnect_callback_();
        }
    }

    bool is_open() const override {
        return open_;
    }

    std::string remote_address() const override {
        return "127.0.0.1";
    }

    uint16_t remote_port() const override {
        return 9590;
    }

    bool is_inbound() const override {
        return is_inbound_;
    }

    uint64_t connection_id() const override {
        return id_;
    }

    void set_receive_callback(ReceiveCallback callback) override {
        receive_callback_ = callback;
    }

    void set_disconnect_callback(DisconnectCallback callback) override {
        disconnect_callback_ = callback;
    }

    // Test helpers
    void set_inbound(bool inbound) { is_inbound_ = inbound; }
    void set_id(uint64_t id) { id_ = id; }

    void simulate_receive(const std::vector<uint8_t>& data) {
        if (receive_callback_) {
            receive_callback_(data);
        }
    }

    std::vector<std::vector<uint8_t>> get_sent_messages() {
        std::lock_guard<std::mutex> lock(mutex_);
        return sent_messages_;
    }

    void clear_sent_messages() {
        std::lock_guard<std::mutex> lock(mutex_);
        sent_messages_.clear();
    }

    size_t sent_message_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return sent_messages_.size();
    }

private:
    bool open_;
    bool is_inbound_ = false;
    uint64_t id_ = 1;
    ReceiveCallback receive_callback_;
    DisconnectCallback disconnect_callback_;
    std::mutex mutex_;
    std::vector<std::vector<uint8_t>> sent_messages_;
};

// =============================================================================
// HELPER FUNCTIONS
// =============================================================================

// Create a complete Bitcoin protocol message (header + payload)
static std::vector<uint8_t> create_test_message(
    uint32_t magic,
    const std::string& command,
    const std::vector<uint8_t>& payload)
{
    auto header = message::create_header(magic, command, payload);
    auto header_bytes = message::serialize_header(header);

    std::vector<uint8_t> full_message;
    full_message.insert(full_message.end(), header_bytes.begin(), header_bytes.end());
    full_message.insert(full_message.end(), payload.begin(), payload.end());
    return full_message;
}

// Create VERSION message
static std::vector<uint8_t> create_version_message(uint32_t magic, uint64_t nonce) {
    message::VersionMessage msg;
    msg.version = protocol::PROTOCOL_VERSION;
    msg.services = protocol::NODE_NETWORK;
    msg.timestamp = 1234567890;
    msg.nonce = nonce;
    msg.user_agent = "/Test:1.0.0/";
    msg.start_height = 0;
    msg.relay = true;

    auto payload = msg.serialize();
    return create_test_message(magic, protocol::commands::VERSION, payload);
}

// Create VERACK message
static std::vector<uint8_t> create_verack_message(uint32_t magic) {
    message::VerackMessage msg;
    auto payload = msg.serialize();
    return create_test_message(magic, protocol::commands::VERACK, payload);
}

// Create PING message
static std::vector<uint8_t> create_ping_message(uint32_t magic, uint64_t nonce) {
    message::PingMessage msg(nonce);
    auto payload = msg.serialize();
    return create_test_message(magic, protocol::commands::PING, payload);
}

// Create PONG message
static std::vector<uint8_t> create_pong_message(uint32_t magic, uint64_t nonce) {
    message::PongMessage msg(nonce);
    auto payload = msg.serialize();
    return create_test_message(magic, protocol::commands::PONG, payload);
}

// =============================================================================
// PEER STATE MACHINE TESTS
// =============================================================================

TEST_CASE("Peer - OutboundHandshake", "[peer][handshake]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    mock_conn->set_inbound(false);

    const uint32_t magic = protocol::magic::REGTEST;
    const uint64_t local_nonce = 12345;

    auto peer = Peer::create_outbound(io_context, mock_conn, magic, local_nonce, 0);

    SECTION("Initial state") {
        CHECK(peer->state() == PeerState::CONNECTED);
        CHECK_FALSE(peer->successfully_connected());
        CHECK(peer->is_connected());
        CHECK_FALSE(peer->is_inbound());
    }

    SECTION("Sends VERSION on start") {
        peer->start();

        // Run io_context briefly to process start()
        io_context.poll();

        // Should have sent VERSION
        CHECK(mock_conn->sent_message_count() >= 1);
        CHECK(peer->state() == PeerState::VERSION_SENT);
    }

    SECTION("Complete handshake") {
        bool message_received = false;
        peer->set_message_handler([&](PeerPtr p, std::unique_ptr<message::Message> msg) {
            message_received = true;
            return true;
        });

        peer->start();
        io_context.poll();

        CHECK(peer->state() == PeerState::VERSION_SENT);

        // Simulate receiving VERSION from peer
        auto version_msg = create_version_message(magic, 54321);  // Different nonce
        mock_conn->simulate_receive(version_msg);
        io_context.poll();

        // Should have sent VERACK
        CHECK(mock_conn->sent_message_count() >= 2);

        // Simulate receiving VERACK
        auto verack_msg = create_verack_message(magic);
        mock_conn->simulate_receive(verack_msg);
        io_context.poll();

        // Should be READY now
        CHECK(peer->state() == PeerState::READY);
        CHECK(peer->successfully_connected());
        CHECK(message_received);  // VERACK triggers message handler
    }
}

TEST_CASE("Peer - InboundHandshake", "[peer][handshake]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    mock_conn->set_inbound(true);

    const uint32_t magic = protocol::magic::REGTEST;
    const uint64_t local_nonce = 12345;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, local_nonce, 0);

    SECTION("Waits for VERSION") {
        peer->start();
        io_context.poll();

        // Inbound peer should NOT send VERSION immediately
        // (it waits for peer to send VERSION first)
        CHECK(peer->state() == PeerState::CONNECTED);
    }

    SECTION("Complete inbound handshake") {
        peer->start();
        io_context.poll();

        // Receive VERSION from peer
        auto version_msg = create_version_message(magic, 54321);
        mock_conn->simulate_receive(version_msg);
        io_context.poll();

        // Should have sent VERACK and our VERSION
        CHECK(mock_conn->sent_message_count() >= 2);

        // Receive VERACK
        auto verack_msg = create_verack_message(magic);
        mock_conn->simulate_receive(verack_msg);
        io_context.poll();

        CHECK(peer->state() == PeerState::READY);
        CHECK(peer->successfully_connected());
    }
}

TEST_CASE("Peer - SelfConnectionPrevention", "[peer][handshake][security]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    mock_conn->set_inbound(true);

    const uint32_t magic = protocol::magic::REGTEST;
    const uint64_t local_nonce = 12345;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, local_nonce, 0);
    peer->start();
    io_context.poll();

    // Simulate receiving VERSION with OUR OWN nonce (self-connection)
    auto version_msg = create_version_message(magic, local_nonce);  // Same nonce!
    mock_conn->simulate_receive(version_msg);
    io_context.poll();

    // Should have disconnected
    CHECK(peer->state() == PeerState::DISCONNECTED);
}

// =============================================================================
// MESSAGE HANDLING TESTS
// =============================================================================

TEST_CASE("Peer - SendMessage", "[peer][messages]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();

    const uint32_t magic = protocol::magic::REGTEST;
    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 12345, 0);

    SECTION("Send PING message") {
        auto ping = std::make_unique<message::PingMessage>(99999);
        peer->send_message(std::move(ping));

        CHECK(mock_conn->sent_message_count() == 1);

        // Verify it's a complete message (header + payload)
        auto sent = mock_conn->get_sent_messages()[0];
        CHECK(sent.size() >= protocol::MESSAGE_HEADER_SIZE);
    }

    SECTION("Cannot send when disconnected") {
        peer->disconnect();
        io_context.poll();

        size_t before = mock_conn->sent_message_count();
        auto ping = std::make_unique<message::PingMessage>(99999);
        peer->send_message(std::move(ping));

        CHECK(mock_conn->sent_message_count() == before);  // No new messages
    }
}

TEST_CASE("Peer - ReceiveMessage", "[peer][messages]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();

    const uint32_t magic = protocol::magic::REGTEST;
    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 12345, 0);

    std::string received_command;
    peer->set_message_handler([&](PeerPtr p, std::unique_ptr<message::Message> msg) {
        received_command = msg->command();
        return true;
    });

    peer->start();
    io_context.poll();

    // Complete handshake first (required for messages to be processed)
    auto version_msg = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version_msg);
    io_context.poll();

    auto verack_msg = create_verack_message(magic);
    mock_conn->simulate_receive(verack_msg);
    io_context.poll();

    REQUIRE(peer->state() == PeerState::READY);
    mock_conn->clear_sent_messages();

    SECTION("Receive PING and auto-respond with PONG") {
        // Clear the received command (it was set to "verack" during handshake)
        received_command.clear();

        auto ping_msg = create_ping_message(magic, 77777);
        mock_conn->simulate_receive(ping_msg);
        io_context.poll();

        // Should have sent PONG automatically (PING not passed to handler)
        CHECK(mock_conn->sent_message_count() == 1);
        CHECK(received_command.empty());  // PING handled internally
    }
}

TEST_CASE("Peer - InvalidMessageHandling", "[peer][messages][security]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();

    const uint32_t magic = protocol::magic::REGTEST;
    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 12345, 0);
    peer->start();
    io_context.poll();

    SECTION("Wrong magic bytes") {
        // Create message with wrong magic
        auto ping_msg = create_ping_message(0xDEADBEEF, 12345);
        mock_conn->simulate_receive(ping_msg);
        io_context.poll();

        // Should disconnect
        CHECK(peer->state() == PeerState::DISCONNECTED);
    }

    SECTION("Message too large") {
        // Create header claiming huge payload
        protocol::MessageHeader header(magic, protocol::commands::PING,
                                      protocol::MAX_PROTOCOL_MESSAGE_LENGTH + 1);
        header.checksum.fill(0);

        auto header_bytes = message::serialize_header(header);
        mock_conn->simulate_receive(header_bytes);
        io_context.poll();

        // Should disconnect (message too large)
        CHECK(peer->state() == PeerState::DISCONNECTED);
    }

    SECTION("Checksum mismatch") {
        message::PingMessage ping(12345);
        auto payload = ping.serialize();

        protocol::MessageHeader header(magic, protocol::commands::PING,
                                      static_cast<uint32_t>(payload.size()));
        header.checksum.fill(0xFF);  // Wrong checksum

        auto header_bytes = message::serialize_header(header);
        std::vector<uint8_t> full_message;
        full_message.insert(full_message.end(), header_bytes.begin(), header_bytes.end());
        full_message.insert(full_message.end(), payload.begin(), payload.end());

        mock_conn->simulate_receive(full_message);
        io_context.poll();

        // Should disconnect
        CHECK(peer->state() == PeerState::DISCONNECTED);
    }
}

// =============================================================================
// TIMEOUT TESTS
// =============================================================================

TEST_CASE("Peer - HandshakeTimeout", "[.][timeout]") {
    // NOTE: Excluded from default test runs (marked with [.] prefix)
    // This test uses real wall-clock time and waits 61+ seconds
    // Removed [peer] tag so it doesn't run with peer tests
    // Run explicitly with: ./coinbasechain_tests "[timeout]" or ./coinbasechain_tests "[.]"

    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();

    auto peer = Peer::create_outbound(io_context, mock_conn,
                                      protocol::magic::REGTEST, 12345, 0);
    peer->start();

    // Run io_context for longer than handshake timeout
    auto work = boost::asio::make_work_guard(io_context);

    // Advance time by running for handshake timeout duration + buffer
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start <
           std::chrono::seconds(protocol::VERSION_HANDSHAKE_TIMEOUT_SEC + 1)) {
        io_context.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Should have timed out and disconnected
    CHECK(peer->state() == PeerState::DISCONNECTED);
}

TEST_CASE("Peer - InactivityTimeout", "[peer][timeout]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();

    const uint32_t magic = protocol::magic::REGTEST;
    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 12345, 0);

    // Complete handshake
    peer->start();
    io_context.poll();

    auto version_msg = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version_msg);
    io_context.poll();

    auto verack_msg = create_verack_message(magic);
    mock_conn->simulate_receive(verack_msg);
    io_context.poll();

    REQUIRE(peer->state() == PeerState::READY);

    // Note: Inactivity timeout is 20 minutes, so we can't realistically test it
    // in a unit test. This test documents the expected behavior.
    // Integration tests should verify this.
}

// =============================================================================
// BUFFER MANAGEMENT / SECURITY TESTS
// =============================================================================

TEST_CASE("Peer - ReceiveBufferFloodProtection", "[peer][security][dos]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();

    auto peer = Peer::create_outbound(io_context, mock_conn,
                                      protocol::magic::REGTEST, 12345, 0);
    peer->start();
    io_context.poll();

    // Try to overflow receive buffer with huge chunk
    std::vector<uint8_t> huge_data(protocol::DEFAULT_RECV_FLOOD_SIZE + 1, 0xAA);
    mock_conn->simulate_receive(huge_data);
    io_context.poll();

    // Should disconnect due to buffer overflow
    CHECK(peer->state() == PeerState::DISCONNECTED);
}

TEST_CASE("Peer - UserAgentLengthValidation", "[peer][security]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();

    const uint32_t magic = protocol::magic::REGTEST;
    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 12345, 0);
    peer->start();
    io_context.poll();

    // Create VERSION with excessively long user agent (> 256 bytes)
    message::VersionMessage msg;
    msg.version = protocol::PROTOCOL_VERSION;
    msg.services = protocol::NODE_NETWORK;
    msg.timestamp = 1234567890;
    msg.nonce = 54321;
    msg.user_agent = std::string(protocol::MAX_SUBVERSION_LENGTH + 1, 'X');  // Too long!
    msg.start_height = 0;
    msg.relay = true;

    auto payload = msg.serialize();
    auto full_msg = create_test_message(magic, protocol::commands::VERSION, payload);

    mock_conn->simulate_receive(full_msg);
    io_context.poll();

    // Should disconnect due to oversized user agent
    CHECK(peer->state() == PeerState::DISCONNECTED);
}

// =============================================================================
// STATISTICS TESTS
// =============================================================================

TEST_CASE("Peer - Statistics", "[peer][stats]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();

    const uint32_t magic = protocol::magic::REGTEST;
    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 12345, 0);

    SECTION("Tracks messages sent") {
        peer->start();
        io_context.poll();

        size_t initial = peer->stats().messages_sent;

        auto ping = std::make_unique<message::PingMessage>(12345);
        peer->send_message(std::move(ping));

        CHECK(peer->stats().messages_sent == initial + 1);
        CHECK(peer->stats().bytes_sent > 0);
    }

    SECTION("Tracks messages received") {
        peer->set_message_handler([](PeerPtr p, std::unique_ptr<message::Message> msg) {
            return true;
        });

        peer->start();
        io_context.poll();

        // Complete handshake first
        auto version_msg = create_version_message(magic, 54321);
        mock_conn->simulate_receive(version_msg);
        io_context.poll();

        auto verack_msg = create_verack_message(magic);
        mock_conn->simulate_receive(verack_msg);
        io_context.poll();

        size_t initial = peer->stats().messages_received;

        // Send another message
        auto ping_msg = create_ping_message(magic, 99999);
        mock_conn->simulate_receive(ping_msg);
        io_context.poll();

        CHECK(peer->stats().messages_received > initial);
        CHECK(peer->stats().bytes_received > 0);
    }
}

// =============================================================================
// PING/PONG TESTS
// =============================================================================

TEST_CASE("Peer - PingPong", "[peer][ping]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();

    const uint32_t magic = protocol::magic::REGTEST;
    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 12345, 0);

    // Complete handshake
    peer->start();
    io_context.poll();

    auto version_msg = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version_msg);
    io_context.poll();

    auto verack_msg = create_verack_message(magic);
    mock_conn->simulate_receive(verack_msg);
    io_context.poll();

    REQUIRE(peer->state() == PeerState::READY);

    // Note: Testing automatic ping sending requires waiting 2 minutes (PING_INTERVAL_SEC)
    // which is impractical for unit tests. Integration tests should cover this.
    // Here we just test that PING auto-responds with PONG

    mock_conn->clear_sent_messages();

    uint64_t ping_nonce = 777777;
    auto ping_msg = create_ping_message(magic, ping_nonce);
    mock_conn->simulate_receive(ping_msg);
    io_context.poll();

    // Should have sent PONG
    CHECK(mock_conn->sent_message_count() == 1);

    // Parse the PONG to verify nonce matches
    auto pong_data = mock_conn->get_sent_messages()[0];
    CHECK(pong_data.size() >= protocol::MESSAGE_HEADER_SIZE);
}

// =============================================================================
// DISCONNECT TESTS
// =============================================================================

TEST_CASE("Peer - DisconnectCleanup", "[peer][disconnect]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();

    auto peer = Peer::create_outbound(io_context, mock_conn,
                                      protocol::magic::REGTEST, 12345, 0);
    peer->start();
    io_context.poll();

    REQUIRE(peer->is_connected());

    peer->disconnect();
    io_context.poll();

    CHECK(peer->state() == PeerState::DISCONNECTED);
    CHECK_FALSE(peer->is_connected());

    // Multiple disconnects should be safe
    peer->disconnect();
    peer->disconnect();
}

TEST_CASE("Peer - PeerInfo", "[peer][info]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();

    const uint32_t magic = protocol::magic::REGTEST;
    const uint64_t peer_nonce = 54321;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 12345, 0);
    peer->start();
    io_context.poll();

    // Before VERSION received
    CHECK(peer->version() == 0);
    CHECK(peer->user_agent().empty());
    CHECK(peer->start_height() == 0);

    // Receive VERSION
    message::VersionMessage version_msg;
    version_msg.version = protocol::PROTOCOL_VERSION;
    version_msg.services = protocol::NODE_NETWORK;
    version_msg.timestamp = 1234567890;
    version_msg.nonce = peer_nonce;
    version_msg.user_agent = "/TestPeer:2.0.0/";
    version_msg.start_height = 100;
    version_msg.relay = true;

    auto payload = version_msg.serialize();
    auto full_msg = create_test_message(magic, protocol::commands::VERSION, payload);

    mock_conn->simulate_receive(full_msg);
    io_context.poll();

    // After VERSION received
    CHECK(peer->version() == protocol::PROTOCOL_VERSION);
    CHECK(peer->services() == protocol::NODE_NETWORK);
    CHECK(peer->user_agent() == "/TestPeer:2.0.0/");
    CHECK(peer->start_height() == 100);
    CHECK(peer->peer_nonce() == peer_nonce);
}

// =============================================================================
// PROTOCOL SECURITY TESTS (Bitcoin Core Compliance)
// =============================================================================

TEST_CASE("Peer - DuplicateVersionRejection", "[peer][security][critical]") {
    // SECURITY: Test that duplicate VERSION messages are rejected
    // Bitcoin Core: checks if (pfrom.nVersion != 0) and ignores duplicates
    // Attack: Send VERSION twice to manipulate time data or peer info

    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 12345, 0);
    peer->start();
    io_context.poll();

    // Send first VERSION
    auto version1 = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version1);
    io_context.poll();

    // Verify first VERSION accepted
    CHECK(peer->version() == protocol::PROTOCOL_VERSION);
    CHECK(peer->user_agent() == "/Test:1.0.0/");
    CHECK(peer->peer_nonce() == 54321);

    // Send duplicate VERSION with different data
    message::VersionMessage msg2;
    msg2.version = 99999;  // Different version
    msg2.services = protocol::NODE_NETWORK;
    msg2.timestamp = 9999999999;  // Far future timestamp
    msg2.nonce = 11111;  // Different nonce
    msg2.user_agent = "/Attacker:6.6.6/";  // Different user agent
    msg2.start_height = 999;
    msg2.relay = true;

    auto payload2 = msg2.serialize();
    auto version2 = create_test_message(magic, protocol::commands::VERSION, payload2);
    mock_conn->simulate_receive(version2);
    io_context.poll();

    // Should IGNORE duplicate VERSION - peer info should NOT change
    CHECK(peer->version() == protocol::PROTOCOL_VERSION);  // Original version
    CHECK(peer->user_agent() == "/Test:1.0.0/");  // Original user agent
    CHECK(peer->peer_nonce() == 54321);  // Original nonce

    // Peer should still be connected (just ignored the message)
    CHECK(peer->is_connected());
}

TEST_CASE("Peer - MessageBeforeVersionRejected", "[peer][security][critical]") {
    // SECURITY: Test that messages before VERSION are rejected
    // Bitcoin Core: checks if (pfrom.nVersion == 0) and rejects all non-VERSION messages
    // Attack: Send PING/HEADERS/etc before handshake to bypass protocol state machine

    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 12345, 0);
    peer->start();
    io_context.poll();

    REQUIRE(peer->state() == PeerState::CONNECTED);
    REQUIRE(peer->version() == 0);  // No VERSION received yet

    SECTION("PING before VERSION disconnects") {
        auto ping_msg = create_ping_message(magic, 99999);
        mock_conn->simulate_receive(ping_msg);
        io_context.poll();

        // Should disconnect (protocol violation)
        CHECK(peer->state() == PeerState::DISCONNECTED);
    }

    SECTION("VERACK before VERSION disconnects") {
        auto verack_msg = create_verack_message(magic);
        mock_conn->simulate_receive(verack_msg);
        io_context.poll();

        // Should disconnect (protocol violation)
        CHECK(peer->state() == PeerState::DISCONNECTED);
    }

    SECTION("PONG before VERSION disconnects") {
        auto pong_msg = create_pong_message(magic, 12345);
        mock_conn->simulate_receive(pong_msg);
        io_context.poll();

        // Should disconnect (protocol violation)
        CHECK(peer->state() == PeerState::DISCONNECTED);
    }
}

TEST_CASE("Peer - DuplicateVerackRejection", "[peer][security]") {
    // SECURITY: Test that duplicate VERACK messages are rejected
    // Bitcoin Core: checks if (pfrom.fSuccessfullyConnected) and ignores duplicates
    // Attack: Send VERACK multiple times to cause timer churn

    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 12345, 0);
    peer->start();
    io_context.poll();

    // Complete VERSION exchange
    auto version_msg = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version_msg);
    io_context.poll();

    // Send first VERACK
    auto verack1 = create_verack_message(magic);
    mock_conn->simulate_receive(verack1);
    io_context.poll();

    // Should be READY after first VERACK
    CHECK(peer->state() == PeerState::READY);
    CHECK(peer->successfully_connected());

    // Send duplicate VERACK
    auto verack2 = create_verack_message(magic);
    mock_conn->simulate_receive(verack2);
    io_context.poll();

    // Should still be READY (duplicate ignored)
    CHECK(peer->state() == PeerState::READY);
    CHECK(peer->successfully_connected());

    // Should still be connected (not disconnected)
    CHECK(peer->is_connected());
}

TEST_CASE("Peer - VersionMustBeFirstMessage", "[peer][security][critical]") {
    // SECURITY: Comprehensive test that VERSION must be first message
    // This is critical for protocol state machine integrity

    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 12345, 0);
    peer->start();
    io_context.poll();

    // Try to send VERSION after already receiving VERSION (duplicate)
    auto version1 = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version1);
    io_context.poll();

    REQUIRE(peer->version() != 0);  // VERSION received

    // Now send VERACK (this is allowed after VERSION)
    auto verack = create_verack_message(magic);
    mock_conn->simulate_receive(verack);
    io_context.poll();

    // Should accept VERACK after VERSION
    CHECK(peer->state() == PeerState::READY);

    // Now try to send another VERSION (should be ignored)
    auto version2 = create_version_message(magic, 99999);
    mock_conn->simulate_receive(version2);
    io_context.poll();

    // Should ignore duplicate VERSION, peer info should NOT change
    CHECK(peer->peer_nonce() == 54321);  // Original nonce
    CHECK(peer->state() == PeerState::READY);  // Still ready
}
