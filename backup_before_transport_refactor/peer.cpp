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
           uint32_t network_magic,
           bool is_inbound,
           uint64_t local_nonce)
    : io_context_(io_context),
      socket_(io_context),
      handshake_timer_(io_context),
      ping_timer_(io_context),
      inactivity_timer_(io_context),
      network_magic_(network_magic),
      is_inbound_(is_inbound),
      id_(next_id_++),
      local_nonce_(local_nonce),
      state_(PeerState::DISCONNECTED)
{
    recv_header_buffer_.resize(protocol::MESSAGE_HEADER_SIZE);
}

Peer::~Peer() {
    disconnect();
}

PeerPtr Peer::create_outbound(
    boost::asio::io_context& io_context,
    const std::string& address,
    uint16_t port,
    uint32_t network_magic,
    uint64_t local_nonce)
{
    auto peer = PeerPtr(new Peer(io_context, network_magic, false, local_nonce));
    peer->do_connect(address, port);
    return peer;
}

PeerPtr Peer::create_inbound(
    boost::asio::io_context& io_context,
    boost::asio::ip::tcp::socket socket,
    uint32_t network_magic,
    uint64_t local_nonce)
{
    auto peer = PeerPtr(new Peer(io_context, network_magic, true, local_nonce));
    peer->socket_ = std::move(socket);
    peer->state_ = PeerState::CONNECTED;
    return peer;
}

void Peer::start() {
    if (state_ == PeerState::DISCONNECTED) {
        LOG_NET_ERROR("Cannot start disconnected peer");
        return;
    }

    stats_.connected_time = util::GetSteadyTime();

    if (is_inbound_) {
        // Inbound: wait for VERSION from peer
        start_handshake_timeout();
        start_read_header();
    } else {
        // Outbound: send our VERSION
        send_version();
        start_handshake_timeout();
        start_read_header();
    }
}

void Peer::disconnect() {
    if (state_ == PeerState::DISCONNECTED || state_ == PeerState::DISCONNECTING) {
        return;
    }

    state_ = PeerState::DISCONNECTING;

    cancel_all_timers();

    boost::system::error_code ec;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);

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

    // Add to send queue (thread-safe)
    boost::asio::post(io_context_, [self = shared_from_this(), data = std::move(full_message)]() mutable {
        bool should_write = false;
        {
            std::lock_guard<std::mutex> lock(self->send_queue_mutex_);
            self->send_queue_.push(std::move(data));
            if (!self->writing_) {
                self->writing_ = true;
                should_write = true;
            }
        }
        // Call start_write OUTSIDE the mutex to avoid deadlock
        if (should_write) {
            self->do_write();
        }
    });

    stats_.messages_sent++;
}

void Peer::set_message_handler(MessageHandler handler) {
    message_handler_ = std::move(handler);
}

std::string Peer::address() const {
    try {
        return socket_.remote_endpoint().address().to_string();
    } catch (...) {
        return "unknown";
    }
}

uint16_t Peer::port() const {
    try {
        return socket_.remote_endpoint().port();
    } catch (...) {
        return 0;
    }
}

// Private methods

void Peer::do_connect(const std::string& address, uint16_t port) {
    state_ = PeerState::CONNECTING;

    boost::asio::ip::tcp::resolver resolver(io_context_);
    auto endpoints = resolver.resolve(address, std::to_string(port));

    auto self = shared_from_this();
    boost::asio::async_connect(
        socket_,
        endpoints,
        [self](const boost::system::error_code& ec, const boost::asio::ip::tcp::endpoint&) {
            if (!ec) {
                self->on_connected();
            } else {
                LOG_NET_ERROR("Connection failed: {}", ec.message());
                self->on_disconnect();
            }
        }
    );
}

void Peer::on_connected() {
    state_ = PeerState::CONNECTED;
    LOG_NET_INFO("Connected to peer: {}:{}", address(), port());
}

void Peer::on_disconnect() {
    state_ = PeerState::DISCONNECTED;
    LOG_NET_INFO("Peer disconnected: {}:{}", address(), port());
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
    version_msg->user_agent = protocol::USER_AGENT;
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

void Peer::start_read_header() {
    auto self = shared_from_this();
    boost::asio::async_read(
        socket_,
        boost::asio::buffer(recv_header_buffer_),
        [self](const boost::system::error_code& ec, std::size_t bytes_transferred) {
            if (ec) {
                LOG_NET_ERROR("Read header error: {}", ec.message());
                self->disconnect();
                return;
            }

            self->stats_.bytes_received += bytes_transferred;
            self->stats_.last_recv = util::GetSteadyTime();

            // Parse header
            protocol::MessageHeader header;
            if (!message::deserialize_header(self->recv_header_buffer_.data(),
                                            self->recv_header_buffer_.size(),
                                            header)) {
                LOG_NET_ERROR("Invalid message header");
                self->disconnect();
                return;
            }

            // Validate magic
            if (header.magic != self->network_magic_) {
                LOG_NET_ERROR("Invalid network magic");
                self->disconnect();
                return;
            }

            // Validate payload size
            if (header.length > protocol::MAX_MESSAGE_SIZE) {
                LOG_NET_ERROR("Message too large: {}", header.length);
                self->disconnect();
                return;
            }

            // Read payload
            self->start_read_payload(header);
        }
    );
}

void Peer::start_read_payload(const protocol::MessageHeader& header) {
    recv_payload_buffer_.resize(header.length);

    auto self = shared_from_this();
    boost::asio::async_read(
        socket_,
        boost::asio::buffer(recv_payload_buffer_),
        [self, header](const boost::system::error_code& ec, std::size_t bytes_transferred) {
            if (ec) {
                LOG_NET_ERROR("Read payload error: {}", ec.message());
                self->disconnect();
                return;
            }

            self->stats_.bytes_received += bytes_transferred;

            // Verify checksum
            auto checksum = message::compute_checksum(self->recv_payload_buffer_);
            if (checksum != header.checksum) {
                LOG_NET_ERROR("Checksum mismatch");
                self->disconnect();
                return;
            }

            // Process message
            self->process_message(header, self->recv_payload_buffer_);

            // Continue reading
            self->start_read_header();
        }
    );
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

void Peer::do_write() {
    std::lock_guard<std::mutex> lock(send_queue_mutex_);

    if (send_queue_.empty()) {
        writing_ = false;
        return;
    }

    // writing_ should already be true from send_message
    auto& data = send_queue_.front();

    auto self = shared_from_this();
    boost::asio::async_write(
        socket_,
        boost::asio::buffer(data),
        [self](const boost::system::error_code& ec, std::size_t bytes_transferred) {
            if (ec) {
                LOG_NET_ERROR("Write error: {}", ec.message());
                self->disconnect();
                return;
            }

            self->stats_.bytes_sent += bytes_transferred;
            self->stats_.last_send = util::GetSteadyTime();

            {
                std::lock_guard<std::mutex> lock(self->send_queue_mutex_);
                self->send_queue_.pop();
            }
            self->schedule_next_write();
        }
    );
}

void Peer::schedule_next_write() {
    bool should_write = false;
    {
        std::lock_guard<std::mutex> lock(send_queue_mutex_);
        if (!send_queue_.empty()) {
            should_write = true;
            // writing_ stays true
        } else {
            writing_ = false;
        }
    }

    if (should_write) {
        boost::asio::post(io_context_, [self = shared_from_this()]() {
            self->do_write();
        });
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
