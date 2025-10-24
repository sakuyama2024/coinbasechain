#include "network/peer.hpp"
#include "chain/logging.hpp"
#include "chain/time.hpp"
#include "chain/timedata.hpp"
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <random>

namespace coinbasechain {
namespace network {

// Peer ID is now assigned by PeerManager::add_peer(), not auto-generated

// Helper to generate random nonce
static uint64_t generate_nonce() {
  static std::random_device rd;
  static std::mt19937_64 gen(rd());
  static std::uniform_int_distribution<uint64_t> dis;
  return dis(gen);
}

// Helper to get current timestamp (uses mockable time for testing)
static int64_t get_timestamp() { return util::GetTime(); }


// Peer implementation

Peer::Peer(boost::asio::io_context &io_context,
           TransportConnectionPtr connection, uint32_t network_magic,
           bool is_inbound, int32_t start_height,
           const std::string &target_address, uint16_t target_port,
           ConnectionType conn_type)
    : io_context_(io_context), connection_(connection),
      handshake_timer_(io_context), ping_timer_(io_context),
      inactivity_timer_(io_context), network_magic_(network_magic),
      is_inbound_(is_inbound), connection_type_(conn_type), id_(-1),
      local_nonce_(generate_nonce()), local_start_height_(start_height),
      target_address_(target_address), target_port_(target_port),
      state_(connection && connection->is_open()
                 ? PeerState::CONNECTED
                 : (connection ? PeerState::CONNECTING : PeerState::DISCONNECTED)) {}

Peer::~Peer() {
  LOG_NET_INFO("Peer destructor called for peer {} address={}", id_, address());
  disconnect();
}

PeerPtr Peer::create_outbound(boost::asio::io_context &io_context,
                              TransportConnectionPtr connection,
                              uint32_t network_magic,
                              int32_t start_height,
                              const std::string &target_address,
                              uint16_t target_port,
                              ConnectionType conn_type) {
  auto peer = PeerPtr(
      new Peer(io_context, connection, network_magic, false,
               start_height, target_address, target_port, conn_type));
  return peer;
}

PeerPtr Peer::create_inbound(boost::asio::io_context &io_context,
                             TransportConnectionPtr connection,
                             uint32_t network_magic,
                             int32_t start_height) {
  // Bitcoin Core pattern: Store the peer's address (from accepted connection)
  // For inbound peers, this is the runtime address they connected from
  std::string addr = connection ? connection->remote_address() : "";
  uint16_t port = connection ? connection->remote_port() : 0;

  auto peer = PeerPtr(
      new Peer(io_context, connection, network_magic, true,
               start_height, addr, port, ConnectionType::INBOUND));
  return peer;
}

void Peer::start() {
  LOG_NET_TRACE("Peer::start() peer={} state={} is_inbound={} address={}",
                id_, static_cast<int>(state_), is_inbound_, address());

  if (state_ == PeerState::DISCONNECTED) {
    LOG_NET_ERROR("Cannot start disconnected peer - id:{}, address:{}, connection:{}",
                  id_, address(), connection_ ? "exists" : "null");
    return;
  }

  // Transition from CONNECTING to CONNECTED if connection is now open
  if (state_ == PeerState::CONNECTING) {
    bool conn_open = connection_ && connection_->is_open();
    LOG_NET_TRACE("Peer {} in CONNECTING state, connection_open={}", id_, conn_open);
    if (conn_open) {
      state_ = PeerState::CONNECTED;
      LOG_NET_TRACE("Peer {} transitioned to CONNECTED", id_);
    } else {
      LOG_NET_ERROR("Cannot start peer in CONNECTING state - connection not open");
      return;
    }
  }

  stats_.connected_time = util::GetSteadyTime();

  // Set up transport callbacks
  auto self = shared_from_this();
  connection_->set_receive_callback([self](const std::vector<uint8_t> &data) {
    self->on_transport_receive(data);
  });
  connection_->set_disconnect_callback(
      [self]() { self->on_transport_disconnect(); });

  // Start receiving data
  connection_->start();
  LOG_NET_DEBUG("Started connection for peer {}", id_);

  if (is_inbound_) {
    // Inbound: wait for VERSION from peer
    LOG_NET_DEBUG("Peer {} is inbound, waiting for VERSION", id_);
    start_handshake_timeout();
  } else {
    // Outbound: send our VERSION
    LOG_NET_DEBUG("Peer {} is outbound, sending VERSION", id_);
    send_version();
    start_handshake_timeout();
  }
}

void Peer::disconnect() {
  LOG_NET_TRACE("Peer::disconnect() peer={} address={} current_state={}",
                id_, address(), static_cast<int>(state_));

  if (state_ == PeerState::DISCONNECTED || state_ == PeerState::DISCONNECTING) {
    LOG_NET_TRACE("Peer {} already disconnected/disconnecting, skipping", id_);
    return;
  }

  state_ = PeerState::DISCONNECTING;
  LOG_NET_DEBUG("Disconnecting peer {} ({})", id_, address());

  cancel_all_timers();

  if (connection_) {
    connection_->close();
  }

  on_disconnect();
}

void Peer::send_message(std::unique_ptr<message::Message> msg) {
  std::string command = msg->command();

  LOG_NET_TRACE("Peer::send_message() peer={} command={} state={}",
                id_, command, static_cast<int>(state_));

  if (state_ == PeerState::DISCONNECTED || state_ == PeerState::DISCONNECTING) {
    LOG_NET_TRACE("Cannot send {} to peer {} - peer is disconnected/disconnecting", command, id_);
    return;
  }

  // Serialize message
  auto payload = msg->serialize();
  auto header = message::create_header(network_magic_, msg->command(), payload);
  auto header_bytes = message::serialize_header(header);

  // Combine header + payload
  std::vector<uint8_t> full_message;
  full_message.reserve(header_bytes.size() + payload.size());
  full_message.insert(full_message.end(), header_bytes.begin(),
                      header_bytes.end());
  full_message.insert(full_message.end(), payload.begin(), payload.end());

  LOG_NET_DEBUG("Sending {} to {} (size: {} bytes, state: {})",
                command, address(), full_message.size(), static_cast<int>(state_));

  // Send via transport
  LOG_NET_TRACE("Peer::send_message() calling connection_->send() (connection={})",
                connection_ ? "exists" : "null");
  bool send_result = connection_ && connection_->send(full_message);
  LOG_NET_TRACE("Peer::send_message() send_result={}", send_result);

  if (send_result) {
    stats_.messages_sent++;
    stats_.bytes_sent += full_message.size();
    stats_.last_send = util::GetSteadyTime();
    LOG_NET_DEBUG("Successfully sent {} to {}", command, address());
  } else {
    LOG_NET_ERROR("Failed to send {} to {}", command, address());
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

void Peer::on_transport_receive(const std::vector<uint8_t> &data) {
  // Enforce DEFAULT_RECV_FLOOD_SIZE to prevent unbounded receive buffer DoS
  // Prevents attackers from sending data faster than we can process, exhausting
  // memory
  if (recv_buffer_.size() + data.size() > protocol::DEFAULT_RECV_FLOOD_SIZE) {
    LOG_NET_WARN("Receive buffer overflow (current: {} bytes, incoming: {} "
                 "bytes, limit: {} bytes), disconnecting from {}",
                 recv_buffer_.size(), data.size(),
                 protocol::DEFAULT_RECV_FLOOD_SIZE, address());
    disconnect();
    return;
  }

  // Accumulate received data into buffer
  recv_buffer_.insert(recv_buffer_.end(), data.begin(), data.end());

  LOG_NET_DEBUG("Peer {} buffer now {} bytes (added {}), processing messages",
                address(), recv_buffer_.size(), data.size());

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
  LOG_NET_TRACE("Peer::send_version() peer={} address={}", id_, address());
  auto version_msg = std::make_unique<message::VersionMessage>();
  version_msg->version = protocol::PROTOCOL_VERSION;
  version_msg->services = protocol::NODE_NETWORK;
  version_msg->timestamp = get_timestamp();

  // Fill in addr_recv (peer's address) and addr_from (our address)
  // addr_recv: The network address of the remote peer
  if (connection_) {
    std::string peer_addr = connection_->remote_address();
    uint16_t peer_port = connection_->remote_port();
    version_msg->addr_recv = protocol::NetworkAddress::from_string(peer_addr, peer_port);
    LOG_NET_DEBUG("VERSION addr_recv set to {}:{}", peer_addr, peer_port);
  } else {
    // No connection yet (shouldn't happen), use empty address
    version_msg->addr_recv = protocol::NetworkAddress();
    LOG_NET_WARN("No connection when sending VERSION, using empty addr_recv");
  }

  // addr_from: Our address as seen by the peer
  // Match Bitcoin Core: sends CService{} (empty/all zeros) for addrMe in VERSION.
  // Peers discover our real address from the connection itself (what IP they see).
  version_msg->addr_from = protocol::NetworkAddress();
  LOG_NET_DEBUG("VERSION addr_from set to empty (matching Bitcoin Core)");

  // Use our local nonce for self-connection prevention
  version_msg->nonce = local_nonce_;
  version_msg->user_agent = protocol::GetUserAgent();
  version_msg->start_height = local_start_height_;

  send_message(std::move(version_msg));
  state_ = PeerState::VERSION_SENT;
}

void Peer::handle_version(const message::VersionMessage &msg) {
  LOG_NET_TRACE("handle_version() peer={} address={} version={} user_agent={} nonce={}",
                id_, address(), msg.version, msg.user_agent, msg.nonce);

  // SECURITY: Reject duplicate VERSION messages 
  // Bitcoin Core: checks if (pfrom.nVersion != 0) and ignores duplicates
  // Prevents: time manipulation via multiple AddTimeData() calls, protocol
  // violations
  if (peer_version_ != 0) {
    LOG_NET_WARN(
        "Duplicate VERSION from peer {} (already have version {}), ignoring",
        address(), peer_version_);
    return;
  }

  // SECURITY: Reject obsolete protocol versions
  // Bitcoin Core: rejects version < MIN_PROTO_VERSION (209)
  // Prevents: compatibility issues, potential exploits in old protocol versions
  if (msg.version < static_cast<int32_t>(protocol::MIN_PROTOCOL_VERSION)) {
    LOG_NET_WARN(
        "Peer {} using obsolete protocol version {} (min: {}), disconnecting",
        address(), msg.version, protocol::MIN_PROTOCOL_VERSION);
    disconnect();
    return;
  }

  peer_version_ = msg.version;
  peer_services_ = msg.services;
  peer_start_height_ = msg.start_height;
  peer_user_agent_ = msg.user_agent;
  peer_nonce_ = msg.nonce;

  LOG_NET_INFO(
      "Received VERSION from {} - version: {}, user_agent: {}, nonce: {}",
      address(), peer_version_, peer_user_agent_, peer_nonce_);

  // Check for self-connection (inbound only, outbound is checked by
  // NetworkManager)
  if (is_inbound_ && peer_nonce_ == local_nonce_) {
    LOG_NET_WARN(
        "Self-connection detected (nonce match), disconnecting from {}",
        address());
    disconnect();
    return;
  }

  // Add peer's time sample for network time adjustment
  // Calculate offset: peer_time - our_time
  // This helps us maintain accurate network-adjusted time even if local clock
  // is wrong
  int64_t now = util::GetTime();
  int64_t time_offset = msg.timestamp - now;
  util::AddTimeData(address(), time_offset);

  // If we're inbound, send our VERSION first (BEFORE VERACK)
  // This is critical: peer must receive VERSION before VERACK to avoid protocol
  // violation
  if (is_inbound_ && state_ == PeerState::CONNECTED) {
    send_version();
  }

  // Send VERACK
  send_message(std::make_unique<message::VerackMessage>());
}

void Peer::handle_verack() {
  LOG_NET_TRACE("handle_verack() peer={} address={} successfully_connected={}",
                id_, address(), successfully_connected_);

  // SECURITY: Reject duplicate VERACK messages
  // Bitcoin Core: checks if (pfrom.fSuccessfullyConnected) and ignores
  // duplicates Prevents: timer churn from repeated schedule_ping() and
  // start_inactivity_timeout() calls
  if (successfully_connected_) {
    LOG_NET_WARN("Duplicate VERACK from peer {}, ignoring", address());
    return;
  }

  LOG_NET_DEBUG("Received VERACK from {} - handshake complete", address());

  // FEELER connections: Disconnect immediately after handshake completes
  // Bitcoin Core pattern (net_processing.cpp:3606): "feeler connection completed peer=%d; disconnecting"
  // Purpose: Test address liveness without consuming an outbound slot
  if (is_feeler()) {
    LOG_NET_INFO("Feeler connection completed to peer {} ({}); disconnecting",
                 id_, address());
    disconnect();
    return;
  }

  state_ = PeerState::READY;
  successfully_connected_ = true; // Mark handshake as complete
  handshake_timer_.cancel();

  // Start ping timer and inactivity timeout
  schedule_ping();
  start_inactivity_timeout();

  LOG_NET_TRACE("Peer {} now READY, ping and inactivity timers started", id_);
}

void Peer::process_received_data(std::vector<uint8_t> &buffer) {
  // Process as many complete messages as we have in the buffer
  while (buffer.size() >= protocol::MESSAGE_HEADER_SIZE) {
    // Try to parse header
    protocol::MessageHeader header;
    if (!message::deserialize_header(buffer.data(),
                                     protocol::MESSAGE_HEADER_SIZE, header)) {
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

    // Validate payload size (already checked in deserialize_header, but
    // double-check for safety)
    if (header.length > protocol::MAX_PROTOCOL_MESSAGE_LENGTH) {
      LOG_NET_ERROR("Message too large: {} bytes (max: {})", header.length,
                    protocol::MAX_PROTOCOL_MESSAGE_LENGTH);
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
    std::vector<uint8_t> payload(buffer.begin() + protocol::MESSAGE_HEADER_SIZE,
                                 buffer.begin() + total_message_size);

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

void Peer::process_message(const protocol::MessageHeader &header,
                           const std::vector<uint8_t> &payload) {
  stats_.messages_received++;

  std::string command = header.get_command();

  LOG_NET_DEBUG("Received {} from {} (payload size: {} bytes, peer_version: {})",
                command, address(), payload.size(), peer_version_);

  // SECURITY: Enforce VERSION must be first message (critical)
  // Bitcoin Core: checks if (pfrom.nVersion == 0) and rejects all non-VERSION
  // messages Prevents: protocol state violations, handshake bypass attacks
  if (peer_version_ == 0 && command != protocol::commands::VERSION) {
    LOG_NET_WARN("Received {} before VERSION from peer {}, disconnecting "
                 "(protocol violation)",
                 command, address());
    disconnect();
    return;
  }

  // Create message object
  auto msg = message::create_message(command);
  if (!msg) {
    LOG_NET_WARN("Unknown message type: {}", command);
    return;
  }

  // Deserialize
  if (!msg->deserialize(payload.data(), payload.size())) {
    LOG_NET_ERROR("Failed to deserialize message: {} - disconnecting peer "
                  "(protocol violation)",
                  command);
    // Malformed messages indicate protocol violation or malicious peer
    disconnect();
    return;
  }

  // Handle protocol messages internally
  if (command == protocol::commands::VERSION) {
    handle_version(static_cast<const message::VersionMessage &>(*msg));
    // Also notify handler for duplicate detection (Bitcoin Core pattern)
    if (message_handler_) {
      message_handler_(shared_from_this(), std::move(msg));
    }
  } else if (command == protocol::commands::VERACK) {
    handle_verack();
    // Also notify handler so NetworkManager knows peer is ready
    if (message_handler_) {
      message_handler_(shared_from_this(), std::move(msg));
    }
  } else if (command == protocol::commands::PING) {
    auto &ping = static_cast<const message::PingMessage &>(*msg);
    auto pong = std::make_unique<message::PongMessage>(ping.nonce);
    send_message(std::move(pong));
    // PING/PONG handled internally only
  } else if (command == protocol::commands::PONG) {
    handle_pong(static_cast<const message::PongMessage &>(*msg));
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
  ping_timer_.async_wait([self](const boost::system::error_code &ec) {
    if (!ec) {
      // Check if peer timed out (no PONG to previous PING)
      if (self->last_ping_nonce_ != 0) {
        // We sent a ping but haven't received PONG yet
        auto now = util::GetSteadyTime();
        auto ping_age = std::chrono::duration_cast<std::chrono::seconds>(
            now - self->ping_sent_time_);

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

void Peer::handle_pong(const message::PongMessage &msg) {
  if (msg.nonce == last_ping_nonce_) {
    auto now = util::GetSteadyTime();
    auto ping_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - ping_sent_time_);
    stats_.ping_time_ms = ping_time.count();
    LOG_NET_DEBUG("Ping time for {}: {}ms", address(), stats_.ping_time_ms);

    // Clear nonce to indicate we received the PONG
    last_ping_nonce_ = 0;
  }
}

void Peer::start_handshake_timeout() {
  auto self = shared_from_this();
  handshake_timer_.expires_after(
      std::chrono::seconds(protocol::VERSION_HANDSHAKE_TIMEOUT_SEC));
  handshake_timer_.async_wait([self](const boost::system::error_code &ec) {
    if (!ec && self->state_ != PeerState::READY) {
      LOG_NET_WARN("Handshake timeout");
      self->disconnect();
    }
  });
}

void Peer::start_inactivity_timeout() {
  auto self = shared_from_this();
  inactivity_timer_.expires_after(
      std::chrono::seconds(protocol::INACTIVITY_TIMEOUT_SEC));
  inactivity_timer_.async_wait([self](const boost::system::error_code &ec) {
    if (!ec) {
      auto now = util::GetSteadyTime();
      auto last_activity =
          std::max(self->stats_.last_send, self->stats_.last_recv);
      auto idle_time =
          std::chrono::duration_cast<std::chrono::seconds>(now - last_activity);

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
