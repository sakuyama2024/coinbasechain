// Peer regression tests - Bug fix validation (ported to test2)

#include "catch_amalgamated.hpp"
#include "network/peer.hpp"
#include "network/transport.hpp"
#include "network/protocol.hpp"
#include "network/message.hpp"
#include "infra/mock_transport.hpp"
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>

using namespace coinbasechain;
using namespace coinbasechain::network;

// =============================================================================
// HELPERS
// =============================================================================

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

static std::vector<uint8_t> create_version_message(uint32_t magic, uint64_t nonce, int32_t version = protocol::PROTOCOL_VERSION) {
    message::VersionMessage msg;
    msg.version = version;
    msg.services = protocol::NODE_NETWORK;
    msg.timestamp = 1234567890;
    msg.nonce = nonce;
    msg.user_agent = "/Test:1.0.0/";
    msg.start_height = 0;
    auto payload = msg.serialize();
    return create_test_message(magic, protocol::commands::VERSION, payload);
}

static std::vector<uint8_t> create_verack_message(uint32_t magic) {
    message::VerackMessage msg;
    auto payload = msg.serialize();
    return create_test_message(magic, protocol::commands::VERACK, payload);
}

static std::vector<uint8_t> create_ping_message(uint32_t magic, uint64_t nonce) {
    message::PingMessage msg(nonce);
    auto payload = msg.serialize();
    return create_test_message(magic, protocol::commands::PING, payload);
}

// =============================================================================
// DISCONNECT REGRESSION TESTS
// =============================================================================

TEST_CASE("Peer - DisconnectRaceCondition", "[peer][disconnect][regression]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    auto version_msg = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version_msg);
    io_context.poll();

    auto verack_msg = create_verack_message(magic);
    mock_conn->simulate_receive(verack_msg);
    io_context.poll();

    REQUIRE(peer->state() == PeerState::READY);

    peer->disconnect();
    io_context.poll();

    CHECK(peer->state() == PeerState::DISCONNECTED);
    CHECK_FALSE(peer->is_connected());

    peer->disconnect();
    peer->disconnect();
    CHECK(peer->state() == PeerState::DISCONNECTED);
}

// =============================================================================
// STATISTICS REGRESSION TESTS
// =============================================================================

TEST_CASE("Peer - StatsInitialization", "[peer][stats][regression]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);

    peer->start();
    io_context.poll();

    auto stats = peer->stats();

    CHECK(stats.connected_time != std::chrono::steady_clock::time_point{});
    CHECK(stats.last_send >= stats.connected_time);
    CHECK(stats.last_recv >= stats.connected_time);

    auto now = std::chrono::steady_clock::now();
    auto idle_time = std::chrono::duration_cast<std::chrono::seconds>(
        now - stats.last_recv);

    CHECK(idle_time.count() < 1);
}

TEST_CASE("Peer - MessageCounters", "[peer][stats][regression]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    auto version_msg = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version_msg);
    io_context.poll();

    auto verack_msg = create_verack_message(magic);
    mock_conn->simulate_receive(verack_msg);
    io_context.poll();

    REQUIRE(peer->state() == PeerState::READY);

    auto stats_before = peer->stats();

    for (int i = 0; i < 5; i++) {
        auto ping = create_ping_message(magic, 2000 + i);
        mock_conn->simulate_receive(ping);
        io_context.poll();
    }

    auto stats_after = peer->stats();

    CHECK(stats_after.messages_received > stats_before.messages_received);
    CHECK(stats_after.messages_sent > stats_before.messages_sent);
    CHECK(stats_after.bytes_received > stats_before.bytes_received);
    CHECK(stats_after.bytes_sent > stats_before.bytes_sent);
}

// =============================================================================
// THREADING REGRESSION TESTS
// =============================================================================

TEST_CASE("Peer - StateThreadSafety", "[peer][threading][regression]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    std::atomic<bool> stop{false};
    std::atomic<int> read_count{0};
    std::vector<std::thread> readers;

    for (int i = 0; i < 4; i++) {
        readers.emplace_back([&]() {
            while (!stop) {
                auto state = peer->state();
                (void)state;
                read_count++;
                std::this_thread::yield();
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    peer->disconnect();
    io_context.poll();

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stop = true;

    for (auto& t : readers) {
        t.join();
    }

    CHECK(read_count > 0);
    CHECK(peer->state() == PeerState::DISCONNECTED);
}

// =============================================================================
// PROTOCOL REGRESSION TESTS
// =============================================================================

TEST_CASE("Peer - FeelerConnectionLifecycle", "[peer][feeler][regression]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0,
                                      "127.0.0.1", 9590, ConnectionType::FEELER);

    CHECK(peer->is_feeler());
    CHECK_FALSE(peer->successfully_connected());

    peer->start();
    io_context.poll();

    auto version_msg = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version_msg);
    io_context.poll();

    auto verack_msg = create_verack_message(magic);
    mock_conn->simulate_receive(verack_msg);
    io_context.poll();

    CHECK(peer->state() == PeerState::DISCONNECTED);
    CHECK_FALSE(peer->is_connected());
}

TEST_CASE("Peer - ObsoleteProtocolVersion", "[peer][security][regression]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    auto version_msg = create_version_message(magic, 54321, 0);
    mock_conn->simulate_receive(version_msg);
    io_context.poll();

    CHECK(peer->state() == PeerState::DISCONNECTED);
    CHECK(peer->version() == 0);
}

TEST_CASE("Peer - ReceiveBufferOptimization", "[peer][performance][regression]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    auto version_msg = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version_msg);
    io_context.poll();

    auto verack_msg = create_verack_message(magic);
    mock_conn->simulate_receive(verack_msg);
    io_context.poll();

    REQUIRE(peer->state() == PeerState::READY);

    for (int i = 0; i < 10; i++) {
        auto ping = create_ping_message(magic, 1000 + i);
        mock_conn->simulate_receive(ping);
        io_context.poll();
    }

    CHECK(peer->is_connected());
    CHECK(peer->stats().messages_received >= 12);
}