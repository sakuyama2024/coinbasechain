#ifndef COINBASECHAIN_SIMULATED_TRANSPORT_HPP
#define COINBASECHAIN_SIMULATED_TRANSPORT_HPP

#include "network/transport.hpp"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>

namespace coinbasechain {
namespace network {

// Forward declaration
class SimulatedTransport;

/**
 * SimulatedTransportConnection - In-memory connection for testing
 *
 * Routes messages through SimulatedTransport's message queue instead of real
 * sockets.
 */
class SimulatedTransportConnection
    : public TransportConnection,
      public std::enable_shared_from_this<SimulatedTransportConnection> {
public:
  SimulatedTransportConnection(uint64_t id, bool is_inbound,
                               const std::string &remote_addr,
                               uint16_t remote_port,
                               SimulatedTransport *transport);

  ~SimulatedTransportConnection() override;

  // TransportConnection interface
  void start() override;
  bool send(const std::vector<uint8_t> &data) override;
  void close() override;
  bool is_open() const override { return open_; }
  std::string remote_address() const override { return remote_addr_; }
  uint16_t remote_port() const override { return remote_port_; }
  bool is_inbound() const override { return is_inbound_; }
  uint64_t connection_id() const override { return id_; }
  void set_receive_callback(ReceiveCallback callback) override;
  void set_disconnect_callback(DisconnectCallback callback) override;

  // Simulated delivery (called by SimulatedTransport)
  void deliver_data(const std::vector<uint8_t> &data);

  // Set peer connection (for routing messages)
  void set_peer_connection(std::weak_ptr<SimulatedTransportConnection> peer);
  std::weak_ptr<SimulatedTransportConnection> get_peer_connection() const {
    return peer_connection_;
  }

private:
  uint64_t id_;
  bool is_inbound_;
  std::string remote_addr_;
  uint16_t remote_port_;
  SimulatedTransport *transport_;
  std::atomic<bool> open_{true};

  ReceiveCallback receive_callback_;
  DisconnectCallback disconnect_callback_;

  std::weak_ptr<SimulatedTransportConnection> peer_connection_;
};

/**
 * SimulatedTransport - In-memory transport for testing
 *
 * Routes all messages through internal queues. Supports:
 * - Simulated network conditions (latency, packet loss)
 * - Time-based message delivery
 * - Full control over message routing for testing
 */
class SimulatedTransport : public Transport {
public:
  SimulatedTransport();
  ~SimulatedTransport() override;

  // Transport interface
  TransportConnectionPtr connect(const std::string &address, uint16_t port,
                                 ConnectCallback callback) override;

  bool
  listen(uint16_t port,
         std::function<void(TransportConnectionPtr)> accept_callback) override;

  void stop_listening() override;
  void run() override;
  void stop() override;
  bool is_running() const override { return running_; }

  // Testing interface
  struct NetworkConditions {
    uint64_t latency_ms = 0;       // Fixed latency
    double packet_loss_rate = 0.0; // 0.0 to 1.0
    uint64_t bandwidth_limit = 0;  // Bytes per second (0 = unlimited)
  };

  void set_network_conditions(const NetworkConditions &conditions);
  void advance_time(uint64_t ms); // Advance simulated time
  uint64_t get_current_time() const { return current_time_ms_; }

  // Route message between connections
  void route_message(uint64_t from_conn_id, const std::vector<uint8_t> &data);

private:
  struct PendingMessage {
    uint64_t delivery_time_ms;
    uint64_t to_conn_id;
    std::vector<uint8_t> data;
  };

  std::atomic<bool> running_{false};
  uint64_t current_time_ms_ = 0;
  NetworkConditions conditions_;

  // Listening state
  uint16_t listen_port_ = 0;
  std::function<void(TransportConnectionPtr)> accept_callback_;

  // Connections registry
  std::mutex connections_mutex_;
  std::map<uint64_t, std::weak_ptr<SimulatedTransportConnection>> connections_;
  std::atomic<uint64_t> next_connection_id_{1};

  // Message queue
  std::mutex messages_mutex_;
  std::queue<PendingMessage> pending_messages_;

  // Helper to process pending messages
  void process_pending_messages();
};

} // namespace network
} // namespace coinbasechain

#endif // COINBASECHAIN_SIMULATED_TRANSPORT_HPP
