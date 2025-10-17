#include "network/peer.hpp"
#include "util/logging.hpp"
#include "util/time.hpp"
#include "util/timedata.hpp"
#include <random>

namespace coinbasechain {
namespace network {

// Static member initialization
std::atomic<uint64_t> Peer::next_id_{1};

// Helper to generate random nonce
static uint64_t generate_nonce() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;
    return dis(gen);
}

// Helper to get current timestamp (uses mockable time for testing)
static int64_t get_timestamp() {
    return util::GetTime();
}

// Peer implementation

Peer::Peer(boost::asio::io_context& io_context,
           TransportConnectionPtr connection,
           uint32_t network_magic,
           bool is_inbound,
           uint64_t local_nonce)
    : io_context_(io_context),
      connection_(connection),
      handshake_timer_(io_context),
      ping_timer_(io_context),
      inactivity_timer_(io_context),
      network_magic_(network_magic),
      is_inbound_(is_inbound),
      id_(next_id_++),
      local_nonce_(local_nonce),
      state_(connection->is_open() ? PeerState::CONNECTED : PeerState::DISCONNECTED)
{
}

Peer::~Peer() {
    disconnect();
}

PeerPtr Peer::create_outbound(
    boost::asio::io_context& io_context,
    TransportConnectionPtr connection,
    uint32_t network_magic,
    uint64_t local_nonce)
{
    auto peer = PeerPtr(new Peer(io_context, connection, network_magic, false, local_nonce));
    return peer;
}

PeerPtr Peer::create_inbound(
    boost::asio::io_context& io_context,
    TransportConnectionPtr connection,
    uint32_t network_magic,
    uint64_t local_nonce)
{
    auto peer = PeerPtr(new Peer(io_context, connection, network_magic, true, local_nonce));
    return peer;
}

void Peer::start() {
    if (state_ == PeerState::DISCONNECTED) {
        LOG_NET_ERROR("Cannot start disconnected peer");
        return;
    }

    stats_.connected_time = util::GetSteadyTime();

    // Set up transport callbacks
    auto self = shared_from_this();
    connection_->set_receive_callback([self](const std::vector<uint8_t>& data) {
        self->on_transport_receive(data);
    });
    connection_->set_disconnect_callback([self]() {
        self->on_transport_disconnect();
    });

    // Start receiving data
    connection_->start();

    if (is_inbound_) {
        // Inbound: wait for VERSION from peer
        start_handshake_timeout();
    } else {
        // Outbound: send our VERSION
        send_version();
        start_handshake_timeout();
    }
}

void Peer::disconnect() {
    if (state_ == PeerState::DISCONNECTED || state_ == PeerState::DISCONNECTING) {
        return;
    }

    state_ = PeerState::DISCONNECTING;

    cancel_all_timers();

    if (connection_) {
        connection_->close();
    }

    on_disconnect();
}

void Peer::send_message(std::unique_ptr<message::Message> msg) {
    if (state_ == PeerState::DISCONNECTED || state_ == PeerState::DISCONNECTING) {
        return;
    }

    // Serialize message
    auto payload = msg->serialize();
    auto header = message::create_header(network_magic_, msg->command(), payload);
    auto header_bytes = message::serialize_header(header);

    // Combine header + payload
    std::vector<uint8_t> full_message;
    full_message.reserve(header_bytes.size() + payload.size());
    full_message.insert(full_message.end(), header_bytes.begin(), header_bytes.end());
    full_message.insert(full_message.end(), payload.begin(), payload.end());

    // Send via transport
    if (connection_ && connection_->send(full_message)) {
        stats_.messages_sent++;
        stats_.bytes_sent += full_message.size();
        stats_.last_send = util::GetSteadyTime();
    } else {
        LOG_NET_ERROR("Failed to send message");
        disconnect();
    }
}

void Peer::set_message_handler(MessageHandler handler) {
    message_handler_ = std::move(handler);
}

std::string Peer::address() const {
    return connection_ ? connection_->remote_address() : "unknown";
}

uint16_t Peer::port() const {
    return connection_ ? connection_->remote_port() : 0;
}

// Private methods

void Peer::on_connected() {
    state_ = PeerState::CONNECTED;
    LOG_NET_INFO("Connected to peer: {}:{}", address(), port());
}

void Peer::on_disconnect() {
    state_ = PeerState::DISCONNECTED;
    LOG_NET_INFO("Peer disconnected: {}:{}", address(), port());
}

void Peer::on_transport_receive(const std::vector<uint8_t>& data) {
    // SECURITY: Enforce DEFAULT_RECV_FLOOD_SIZE to prevent unbounded receive buffer DoS
    // Bitcoin Core: src/net.cpp CNode::ReceiveMsgBytes() enforces receive buffer limits
    // Prevents attackers from sending data faster than we can process, exhausting memory
    if (recv_buffer_.size() + data.size() > protocol::DEFAULT_RECV_FLOOD_SIZE) {
        LOG_NET_WARN("Receive buffer overflow (current: {} bytes, incoming: {} bytes, limit: {} bytes), disconnecting from {}",
                     recv_buffer_.size(), data.size(), protocol::DEFAULT_RECV_FLOOD_SIZE, address());
        disconnect();
        return;
    }

    // Accumulate received data into buffer
    recv_buffer_.insert(recv_buffer_.end(), data.begin(), data.end());

    // Update stats
    stats_.bytes_received += data.size();
    stats_.last_recv = util::GetSteadyTime();

    // Try to process complete messages
    process_received_data(recv_buffer_);
}

void Peer::on_transport_disconnect() {
    LOG_NET_INFO("Transport disconnected: {}:{}", address(), port());
    disconnect();
}

void Peer::send_version() {
    auto version_msg = std::make_unique<message::VersionMessage>();
    version_msg->version = protocol::PROTOCOL_VERSION;
    version_msg->services = protocol::NODE_NETWORK;
    version_msg->timestamp = get_timestamp();

    // Fill in addr_recv (peer's address) and addr_from (our address)
    // For now, use placeholder values
    version_msg->addr_recv = protocol::NetworkAddress();
    version_msg->addr_from = protocol::NetworkAddress();

    // Use our local nonce for self-connection prevention
    version_msg->nonce = local_nonce_;
    version_msg->user_agent = protocol::GetUserAgent();
    version_msg->start_height = 0;  // TODO: Get from blockchain
    version_msg->relay = true;

    send_message(std::move(version_msg));
    state_ = PeerState::VERSION_SENT;
}

void Peer::handle_version(const message::VersionMessage& msg) {
    peer_version_ = msg.version;
    peer_services_ = msg.services;
    peer_start_height_ = msg.start_height;
    peer_user_agent_ = msg.user_agent;
    peer_nonce_ = msg.nonce;

    LOG_NET_INFO("Received VERSION from {} - version: {}, user_agent: {}, nonce: {}",
                 address(), peer_version_, peer_user_agent_, peer_nonce_);

    // Check for self-connection (inbound only, outbound is checked by NetworkManager)
    if (is_inbound_ && peer_nonce_ == local_nonce_) {
        LOG_NET_WARN("Self-connection detected (nonce match), disconnecting from {}", address());
        disconnect();
        return;
    }

    // Add peer's time sample for network time adjustment
    // Calculate offset: peer_time - our_time
    // This helps us maintain accurate network-adjusted time even if local clock is wrong
    int64_t now = util::GetTime();
    int64_t time_offset = msg.timestamp - now;
    util::AddTimeData(address(), time_offset);

    // Send VERACK
    send_message(std::make_unique<message::VerackMessage>());

    // If we're inbound, also send our VERSION
    if (is_inbound_ && state_ == PeerState::CONNECTED) {
        send_version();
    }
}

void Peer::handle_verack() {
    LOG_NET_DEBUG("Received VERACK from {}", address());

    state_ = PeerState::READY;
    successfully_connected_ = true;  // Mark handshake as complete (matches Bitcoins's fSuccessfullyConnected)
    handshake_timer_.cancel();

    // Start ping timer and inactivity timeout
    schedule_ping();
    start_inactivity_timeout();
}

void Peer::process_received_data(std::vector<uint8_t>& buffer) {
    // Process as many complete messages as we have in the buffer
    while (buffer.size() >= protocol::MESSAGE_HEADER_SIZE) {
        // Try to parse header
        protocol::MessageHeader header;
        if (!message::deserialize_header(buffer.data(), protocol::MESSAGE_HEADER_SIZE, header)) {
            LOG_NET_ERROR("Invalid message header");
            disconnect();
            return;
        }

        // Validate magic
        if (header.magic != network_magic_) {
            LOG_NET_ERROR("Invalid network magic");
            disconnect();
            return;
        }

        // Validate payload size
        if (header.length > protocol::MAX_MESSAGE_SIZE) {
            LOG_NET_ERROR("Message too large: {}", header.length);
            disconnect();
            return;
        }

        // Check if we have the complete message (header + payload)
        size_t total_message_size = protocol::MESSAGE_HEADER_SIZE + header.length;
        if (buffer.size() < total_message_size) {
            // Don't have complete message yet, wait for more data
            return;
        }

        // Extract payload
        std::vector<uint8_t> payload(
            buffer.begin() + protocol::MESSAGE_HEADER_SIZE,
            buffer.begin() + total_message_size
        );

        // Verify checksum
        auto checksum = message::compute_checksum(payload);
        if (checksum != header.checksum) {
            LOG_NET_ERROR("Checksum mismatch");
            disconnect();
            return;
        }

        // Process the complete message
        process_message(header, payload);

        // Remove processed message from buffer
        buffer.erase(buffer.begin(), buffer.begin() + total_message_size);
    }
}

void Peer::process_message(const protocol::MessageHeader& header,
                          const std::vector<uint8_t>& payload)
{
    stats_.messages_received++;

    std::string command = header.get_command();

    // Create message object
    auto msg = message::create_message(command);
    if (!msg) {
        LOG_NET_WARN("Unknown message type: {}", command);
        return;
    }

    // Deserialize
    if (!msg->deserialize(payload.data(), payload.size())) {
        LOG_NET_ERROR("Failed to deserialize message: {} - disconnecting peer (protocol violation)", command);
        // Malformed messages indicate protocol violation or malicious peer
        // Bitcoin Core disconnect immediately for such violations
        disconnect();
        return;
    }

    // Handle protocol messages internally
    if (command == protocol::commands::VERSION) {
        handle_version(static_cast<const message::VersionMessage&>(*msg));
        // VERSION is handled internally only
    } else if (command == protocol::commands::VERACK) {
        handle_verack();
        // Also notify handler so NetworkManager knows peer is ready
        if (message_handler_) {
            message_handler_(shared_from_this(), std::move(msg));
        }
    } else if (command == protocol::commands::PING) {
        auto& ping = static_cast<const message::PingMessage&>(*msg);
        auto pong = std::make_unique<message::PongMessage>(ping.nonce);
        send_message(std::move(pong));
        // PING/PONG handled internally only
    } else if (command == protocol::commands::PONG) {
        handle_pong(static_cast<const message::PongMessage&>(*msg));
        // PING/PONG handled internally only
    } else {
        // Pass to handler
        if (message_handler_) {
            message_handler_(shared_from_this(), std::move(msg));
        }
    }
}

void Peer::schedule_ping() {
    auto self = shared_from_this();
    ping_timer_.expires_after(std::chrono::seconds(protocol::PING_INTERVAL_SEC));
    ping_timer_.async_wait([self](const boost::system::error_code& ec) {
        if (!ec) {
            // Check if peer timed out (no PONG to previous PING)
            if (self->last_ping_nonce_ != 0) {
                // We sent a ping but haven't received PONG yet
                auto now = util::GetSteadyTime();
                auto ping_age = std::chrono::duration_cast<std::chrono::seconds>(now - self->ping_sent_time_);

                if (ping_age.count() > protocol::PING_TIMEOUT_SEC) {
                    LOG_NET_WARN("Ping timeout (no PONG for {}s), disconnecting from {}",
                                 ping_age.count(), self->address());
                    self->disconnect();
                    return;
                }
            }

            self->send_ping();
            self->schedule_ping();
        }
    });
}

void Peer::send_ping() {
    last_ping_nonce_ = generate_nonce();
    ping_sent_time_ = util::GetSteadyTime();

    auto ping = std::make_unique<message::PingMessage>(last_ping_nonce_);
    send_message(std::move(ping));
}

void Peer::handle_pong(const message::PongMessage& msg) {
    if (msg.nonce == last_ping_nonce_) {
        auto now = util::GetSteadyTime();
        auto ping_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - ping_sent_time_
        );
        stats_.ping_time_ms = ping_time.count();
        LOG_NET_DEBUG("Ping time for {}: {}ms", address(), stats_.ping_time_ms);

        // Clear nonce to indicate we received the PONG
        last_ping_nonce_ = 0;
    }
}

void Peer::start_handshake_timeout() {
    auto self = shared_from_this();
    handshake_timer_.expires_after(std::chrono::seconds(protocol::VERSION_HANDSHAKE_TIMEOUT_SEC));
    handshake_timer_.async_wait([self](const boost::system::error_code& ec) {
        if (!ec && self->state_ != PeerState::READY) {
            LOG_NET_WARN("Handshake timeout");
            self->disconnect();
        }
    });
}

void Peer::start_inactivity_timeout() {
    auto self = shared_from_this();
    inactivity_timer_.expires_after(std::chrono::seconds(protocol::INACTIVITY_TIMEOUT_SEC));
    inactivity_timer_.async_wait([self](const boost::system::error_code& ec) {
        if (!ec) {
            auto now = util::GetSteadyTime();
            auto last_activity = std::max(self->stats_.last_send, self->stats_.last_recv);
            auto idle_time = std::chrono::duration_cast<std::chrono::seconds>(now - last_activity);

            if (idle_time.count() > protocol::INACTIVITY_TIMEOUT_SEC) {
                LOG_NET_WARN("Inactivity timeout");
                self->disconnect();
            } else {
                self->start_inactivity_timeout();
            }
        }
    });
}

void Peer::cancel_all_timers() {
    handshake_timer_.cancel();
    ping_timer_.cancel();
    inactivity_timer_.cancel();
}

} // namespace network
} // namespace coinbasechain
