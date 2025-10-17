#ifndef COINBASECHAIN_TRANSPORT_HPP
#define COINBASECHAIN_TRANSPORT_HPP

#include <vector>
#include <functional>
#include <memory>
#include <cstdint>
#include <string>

namespace coinbasechain {
namespace network {

/**
 * Abstract transport interface for network communication
 *
 * Allows dependency injection of different transport implementations:
 * - RealTransport: TCP sockets via boost::asio
 * - SimulatedTransport: In-memory message passing for testing
 *
 * This design is inspired by Bitcoin Core's recent CConnman refactoring
 * to support better testing and alternative transports.
 */

// Forward declarations
class Transport;
class TransportConnection;
using TransportConnectionPtr = std::shared_ptr<TransportConnection>;

/**
 * Callback types for transport events
 */
using ConnectCallback = std::function<void(bool success)>;
using ReceiveCallback = std::function<void(const std::vector<uint8_t>& data)>;
using DisconnectCallback = std::function<void()>;

/**
 * TransportConnection - Represents a single connection
 *
 * Abstract interface for sending/receiving data over a connection.
 * Implementations handle the actual I/O (TCP socket, in-memory, etc.)
 */
class TransportConnection {
public:
    virtual ~TransportConnection() = default;

    /**
     * Start receiving data from this connection
     * Callbacks will be invoked when data arrives or connection closes
     */
    virtual void start() = 0;

    /**
     * Send data over this connection
     * Returns true if queued successfully, false if connection is closed
     */
    virtual bool send(const std::vector<uint8_t>& data) = 0;

    /**
     * Close this connection
     */
    virtual void close() = 0;

    /**
     * Check if connection is open
     */
    virtual bool is_open() const = 0;

    /**
     * Get remote address (for logging/debugging)
     */
    virtual std::string remote_address() const = 0;
    virtual uint16_t remote_port() const = 0;

    /**
     * Check if this is an inbound connection (peer connected to us)
     */
    virtual bool is_inbound() const = 0;

    /**
     * Get unique connection ID
     */
    virtual uint64_t connection_id() const = 0;

    /**
     * Set callbacks for events
     */
    virtual void set_receive_callback(ReceiveCallback callback) = 0;
    virtual void set_disconnect_callback(DisconnectCallback callback) = 0;
};

/**
 * Transport - Factory for creating connections
 *
 * Abstract interface for the transport layer. Implementations provide
 * both outbound connection initiation and inbound connection acceptance.
 */
class Transport {
public:
    virtual ~Transport() = default;

    /**
     * Initiate an outbound connection
     *
     * @param address Target IP address or hostname
     * @param port Target port
     * @param callback Called when connection succeeds or fails
     * @return Connection object (may not be connected yet)
     */
    virtual TransportConnectionPtr connect(
        const std::string& address,
        uint16_t port,
        ConnectCallback callback
    ) = 0;

    /**
     * Start accepting inbound connections on specified port
     *
     * @param port Port to listen on
     * @param accept_callback Called when new inbound connection arrives
     * @return true if listening started successfully
     */
    virtual bool listen(
        uint16_t port,
        std::function<void(TransportConnectionPtr)> accept_callback
    ) = 0;

    /**
     * Stop accepting inbound connections
     */
    virtual void stop_listening() = 0;

    /**
     * Run the transport event loop (for async transports)
     * Blocks until stop() is called or returns immediately for sync transports
     */
    virtual void run() = 0;

    /**
     * Stop the transport (closes all connections, stops listening)
     */
    virtual void stop() = 0;

    /**
     * Check if transport is running
     */
    virtual bool is_running() const = 0;
};

} // namespace network
} // namespace coinbasechain

#endif // COINBASECHAIN_TRANSPORT_HPP
