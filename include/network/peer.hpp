#ifndef COINBASECHAIN_PEER_HPP
#define COINBASECHAIN_PEER_HPP

#include "network/message.hpp"
#include "network/protocol.hpp"
#include "network/transport.hpp"
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

namespace coinbasechain {
namespace network {

// Forward declarations
class Peer;
using PeerPtr = std::shared_ptr<Peer>;

// Peer connection states
enum class PeerState {
  DISCONNECTED,    // Not connected
  CONNECTING,      // TCP connection in progress
  CONNECTED,       // TCP connected, handshake not started
  VERSION_SENT,    // Sent VERSION message
  VERACK_RECEIVED, // Received VERACK (handshake complete)
  READY,           // Fully connected and ready
  DISCONNECTING    // Shutting down
};

// Peer connection statistics
struct PeerStats {
  uint64_t bytes_sent = 0;
  uint64_t bytes_received = 0;
  uint64_t messages_sent = 0;
  uint64_t messages_received = 0;
  std::chrono::steady_clock::time_point connected_time;
  std::chrono::steady_clock::time_point last_send;
  std::chrono::steady_clock::time_point last_recv;
  int64_t ping_time_ms = -1; // -1 means not measured yet
};

// Message handler callback type (returns true if message handled successfully)
using MessageHandler =
    std::function<bool(PeerPtr peer, std::unique_ptr<message::Message> msg)>;

// Peer class - Represents a single peer connection
// Handles async TCP connection, protocol handshake (VERSION/VERACK),
// message framing/parsing, send/receive queuing, ping/pong keepalive, lifecycle
// management
class Peer : public std::enable_shared_from_this<Peer> {
public:
  // Create outbound peer (we initiate connection)
  static PeerPtr create_outbound(boost::asio::io_context &io_context,
                                 TransportConnectionPtr connection,
                                 uint32_t network_magic, uint64_t local_nonce);

  // Create inbound peer (they connected to us)
  static PeerPtr create_inbound(boost::asio::io_context &io_context,
                                TransportConnectionPtr connection,
                                uint32_t network_magic, uint64_t local_nonce);

  ~Peer();

  // Disable copying
  Peer(const Peer &) = delete;
  Peer &operator=(const Peer &) = delete;

  // Start peer connection (outbound: initiates connection, inbound: starts
  // receiving messages)
  void start();

  void disconnect();
  void send_message(std::unique_ptr<message::Message> msg);
  void set_message_handler(MessageHandler handler);

  // Getters
  PeerState state() const { return state_; }
  bool is_connected() const {
    return state_ != PeerState::DISCONNECTED &&
           state_ != PeerState::DISCONNECTING;
  }
  bool successfully_connected() const {
    return successfully_connected_;
  } // Handshake complete
  const PeerStats &stats() const { return stats_; }
  std::string address() const;
  uint16_t port() const;
  bool is_inbound() const { return is_inbound_; }
  uint64_t id() const { return id_; }

  // Peer information from VERSION message
  int32_t version() const { return peer_version_; }
  uint64_t services() const { return peer_services_; }
  int32_t start_height() const { return peer_start_height_; }
  const std::string &user_agent() const { return peer_user_agent_; }
  uint64_t peer_nonce() const { return peer_nonce_; }

private:
  // Private constructor - use create_outbound/create_inbound
  Peer(boost::asio::io_context &io_context, TransportConnectionPtr connection,
       uint32_t network_magic, bool is_inbound, uint64_t local_nonce);

  // Connection management
  void on_connected();
  void on_disconnect();
  void on_transport_receive(const std::vector<uint8_t> &data);
  void on_transport_disconnect();

  // Handshake
  void send_version();
  void handle_version(const message::VersionMessage &msg);
  void handle_verack();

  // Message I/O
  void process_received_data(std::vector<uint8_t> &buffer);
  void process_message(const protocol::MessageHeader &header,
                       const std::vector<uint8_t> &payload);

  // Ping/Pong
  void schedule_ping();
  void send_ping();
  void handle_pong(const message::PongMessage &msg);

  // Timeouts
  void start_handshake_timeout();
  void start_inactivity_timeout();
  void cancel_all_timers();

  // Member variables
  boost::asio::io_context &io_context_;
  TransportConnectionPtr connection_;
  boost::asio::steady_timer handshake_timer_;
  boost::asio::steady_timer ping_timer_;
  boost::asio::steady_timer inactivity_timer_;

  uint32_t network_magic_;
  bool is_inbound_;
  uint64_t id_;
  static std::atomic<uint64_t> next_id_;

  // Self-connection prevention
  uint64_t local_nonce_; // Our node's nonce

  PeerState state_;
  PeerStats stats_;
  MessageHandler message_handler_;
  bool successfully_connected_{false}; // Set to true after VERACK received

  // Peer info from VERSION
  int32_t peer_version_ = 0;
  uint64_t peer_services_ = 0;
  int32_t peer_start_height_ = 0;
  std::string peer_user_agent_;
  uint64_t peer_nonce_ = 0; // Peer's nonce from their VERSION message

  // Receive buffer (accumulates data until complete message received)
  std::vector<uint8_t> recv_buffer_;

  // Ping tracking
  uint64_t last_ping_nonce_ = 0;
  std::chrono::steady_clock::time_point ping_sent_time_;
};

} // namespace network
} // namespace coinbasechain

#endif // COINBASECHAIN_PEER_HPP
