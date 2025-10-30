#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace coinbasechain {
namespace network {

// Abstract transport interface for network communication
// Allows dependency injection of different implementations:
// - RealTransport: TCP sockets via boost::asio
// - NetworkBridgedTransport: In-memory message passing for testing (in test/)


// Forward declarations
class Transport;
class TransportConnection;
using TransportConnectionPtr = std::shared_ptr<TransportConnection>;

// Callback types for transport events
using ConnectCallback = std::function<void(bool success)>;
using ReceiveCallback = std::function<void(const std::vector<uint8_t> &data)>;
using DisconnectCallback = std::function<void()>;

// TransportConnection - Abstract interface for sending/receiving data over a
// connection Implementations handle actual I/O (TCP socket, in-memory, etc.)
class TransportConnection {
public:
  virtual ~TransportConnection() = default;

  // Start receiving data (callbacks invoked when data arrives or connection
  // closes)
  virtual void start() = 0;

  // Send data (returns true if queued successfully, false if connection closed)
  virtual bool send(const std::vector<uint8_t> &data) = 0;

  virtual void close() = 0;
  virtual bool is_open() const = 0;

  virtual std::string remote_address() const = 0;
  virtual uint16_t remote_port() const = 0;
  virtual bool is_inbound() const = 0;
  virtual uint64_t connection_id() const = 0;

  virtual void set_receive_callback(ReceiveCallback callback) = 0;
  virtual void set_disconnect_callback(DisconnectCallback callback) = 0;
};

// Transport - Factory for creating connections
// Implementations provide both outbound connection initiation and inbound
// acceptance
class Transport {
public:
  virtual ~Transport() = default;

  // Initiate outbound connection (callback called on success/fail, returns
  // connection object)
  virtual TransportConnectionPtr connect(const std::string &address,
                                         uint16_t port,
                                         ConnectCallback callback) = 0;

  // Start accepting inbound connections (returns true if listening started
  // successfully)
  virtual bool
  listen(uint16_t port,
         std::function<void(TransportConnectionPtr)> accept_callback) = 0;

  virtual void stop_listening() = 0;

  // Run transport event loop (blocks until stop() called, or returns
  // immediately for sync transports)
  virtual void run() = 0;

  // Stop transport (closes all connections, stops listening)
  virtual void stop() = 0;

  virtual bool is_running() const = 0;
};

} // namespace network
} // namespace coinbasechain


