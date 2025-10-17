#ifndef COINBASECHAIN_REAL_TRANSPORT_HPP
#define COINBASECHAIN_REAL_TRANSPORT_HPP

#include "network/transport.hpp"
#include <boost/asio.hpp>
#include <memory>
#include <atomic>
#include <mutex>
#include <queue>

namespace coinbasechain {
namespace network {

/**
 * RealTransportConnection - TCP socket implementation of TransportConnection
 *
 * Wraps boost::asio::ip::tcp::socket and provides the abstract interface.
 */
class RealTransportConnection : public TransportConnection,
                                 public std::enable_shared_from_this<RealTransportConnection> {
public:
    /**
     * Create outbound connection (will connect to remote)
     */
    static TransportConnectionPtr create_outbound(
        boost::asio::io_context& io_context,
        const std::string& address,
        uint16_t port,
        ConnectCallback callback
    );

    /**
     * Create inbound connection (already connected socket)
     */
    static TransportConnectionPtr create_inbound(
        boost::asio::io_context& io_context,
        boost::asio::ip::tcp::socket socket
    );

    ~RealTransportConnection() override;

    // TransportConnection interface
    void start() override;
    bool send(const std::vector<uint8_t>& data) override;
    void close() override;
    bool is_open() const override;
    std::string remote_address() const override;
    uint16_t remote_port() const override;
    bool is_inbound() const override { return is_inbound_; }
    uint64_t connection_id() const override { return id_; }
    void set_receive_callback(ReceiveCallback callback) override;
    void set_disconnect_callback(DisconnectCallback callback) override;

private:
    RealTransportConnection(
        boost::asio::io_context& io_context,
        bool is_inbound
    );

    void do_connect(const std::string& address, uint16_t port, ConnectCallback callback);
    void start_read();
    void do_write();

    boost::asio::io_context& io_context_;
    boost::asio::ip::tcp::socket socket_;
    bool is_inbound_;
    uint64_t id_;
    static std::atomic<uint64_t> next_id_;

    ReceiveCallback receive_callback_;
    DisconnectCallback disconnect_callback_;

    // Send queue (protected by mutex)
    std::mutex send_mutex_;
    std::queue<std::vector<uint8_t>> send_queue_;
    bool writing_ = false;

    // Receive buffer (single large buffer for efficiency)
    static constexpr size_t RECV_BUFFER_SIZE = 256 * 1024;  // 256 KB
    std::vector<uint8_t> recv_buffer_;

    // Connection state
    std::atomic<bool> open_{false};
    std::string remote_addr_;
    uint16_t remote_port_ = 0;
};

/**
 * RealTransport - boost::asio implementation of Transport
 *
 * Manages io_context and provides connection factory methods.
 */
class RealTransport : public Transport {
public:
    /**
     * Create transport with specified number of IO threads
     */
    explicit RealTransport(size_t io_threads = 4);
    ~RealTransport() override;

    // Transport interface
    TransportConnectionPtr connect(
        const std::string& address,
        uint16_t port,
        ConnectCallback callback
    ) override;

    bool listen(
        uint16_t port,
        std::function<void(TransportConnectionPtr)> accept_callback
    ) override;

    void stop_listening() override;
    void run() override;
    void stop() override;
    bool is_running() const override { return running_; }

    // Access to io_context (for timers, etc.)
    boost::asio::io_context& io_context() { return io_context_; }

private:
    void start_accept();
    void handle_accept(const boost::system::error_code& ec,
                       boost::asio::ip::tcp::socket socket);

    boost::asio::io_context io_context_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard_;
    std::vector<std::thread> io_threads_;
    std::atomic<bool> running_{false};

    // Acceptor for inbound connections
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    std::function<void(TransportConnectionPtr)> accept_callback_;
};

} // namespace network
} // namespace coinbasechain

#endif // COINBASECHAIN_REAL_TRANSPORT_HPP
