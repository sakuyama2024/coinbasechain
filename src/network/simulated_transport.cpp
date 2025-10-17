// Copyright (c) 2024 Coinbase Chain
// Simulated transport implementation for testing

#include "network/simulated_transport.hpp"
#include <random>
#include <algorithm>

namespace coinbasechain {
namespace network {

// ============================================================================
// SimulatedTransportConnection
// ============================================================================

SimulatedTransportConnection::SimulatedTransportConnection(
    uint64_t id,
    bool is_inbound,
    const std::string& remote_addr,
    uint16_t remote_port,
    SimulatedTransport* transport)
    : id_(id)
    , is_inbound_(is_inbound)
    , remote_addr_(remote_addr)
    , remote_port_(remote_port)
    , transport_(transport)
{
}

SimulatedTransportConnection::~SimulatedTransportConnection() {
    close();
}

void SimulatedTransportConnection::start() {
    // Nothing to do - simulated connections are ready immediately
}

bool SimulatedTransportConnection::send(const std::vector<uint8_t>& data) {
    if (!open_) return false;

    // Route through transport
    if (transport_) {
        transport_->route_message(id_, data);
    }

    return true;
}

void SimulatedTransportConnection::close() {
    if (!open_.exchange(false)) {
        return;  // Already closed
    }

    if (disconnect_callback_) {
        disconnect_callback_();
    }
}

void SimulatedTransportConnection::set_receive_callback(ReceiveCallback callback) {
    receive_callback_ = callback;
}

void SimulatedTransportConnection::set_disconnect_callback(DisconnectCallback callback) {
    disconnect_callback_ = callback;
}

void SimulatedTransportConnection::deliver_data(const std::vector<uint8_t>& data) {
    if (open_ && receive_callback_) {
        receive_callback_(data);
    }
}

void SimulatedTransportConnection::set_peer_connection(std::weak_ptr<SimulatedTransportConnection> peer) {
    peer_connection_ = peer;
}

// ============================================================================
// SimulatedTransport
// ============================================================================

SimulatedTransport::SimulatedTransport() {
}

SimulatedTransport::~SimulatedTransport() {
    stop();
}

TransportConnectionPtr SimulatedTransport::connect(
    const std::string& address,
    uint16_t port,
    ConnectCallback callback)
{
    // Create outbound connection
    uint64_t conn_id = next_connection_id_++;
    auto connection = std::make_shared<SimulatedTransportConnection>(
        conn_id,
        false,  // outbound
        address,
        port,
        this
    );

    // Register connection
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_[conn_id] = connection;
    }

    // Simulate connection success (or failure based on conditions)
    bool success = true;

    // Check if there's a listener on this port
    if (listen_port_ != port) {
        success = false;  // No listener
    }

    if (callback) {
        callback(success);
    }

    if (success && accept_callback_) {
        // Create inbound connection for the listener
        uint64_t peer_conn_id = next_connection_id_++;
        auto peer_connection = std::make_shared<SimulatedTransportConnection>(
            peer_conn_id,
            true,  // inbound
            "simulated_peer",
            conn_id,  // Use connection ID as "port"
            this
        );

        // Register peer connection
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_[peer_conn_id] = peer_connection;
        }

        // Link the two connections
        connection->set_peer_connection(peer_connection);
        peer_connection->set_peer_connection(connection);

        // Notify listener
        accept_callback_(peer_connection);
    }

    return connection;
}

bool SimulatedTransport::listen(
    uint16_t port,
    std::function<void(TransportConnectionPtr)> accept_callback)
{
    listen_port_ = port;
    accept_callback_ = accept_callback;
    return true;
}

void SimulatedTransport::stop_listening() {
    listen_port_ = 0;
    accept_callback_ = nullptr;
}

void SimulatedTransport::run() {
    running_ = true;
}

void SimulatedTransport::stop() {
    running_ = false;

    // Close all connections
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (auto& [id, weak_conn] : connections_) {
        if (auto conn = weak_conn.lock()) {
            conn->close();
        }
    }
    connections_.clear();
}

void SimulatedTransport::set_network_conditions(const NetworkConditions& conditions) {
    conditions_ = conditions;
}

void SimulatedTransport::advance_time(uint64_t ms) {
    current_time_ms_ += ms;
    process_pending_messages();
}

void SimulatedTransport::route_message(uint64_t from_conn_id, const std::vector<uint8_t>& data) {
    // Find the peer connection
    std::shared_ptr<SimulatedTransportConnection> from_conn;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(from_conn_id);
        if (it != connections_.end()) {
            from_conn = it->second.lock();
        }
    }

    if (!from_conn) return;

    // Get peer
    auto peer = from_conn->get_peer_connection().lock();
    if (!peer) return;

    // Apply network conditions
    bool should_drop = false;
    if (conditions_.packet_loss_rate > 0.0) {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_real_distribution<> dis(0.0, 1.0);
        should_drop = dis(gen) < conditions_.packet_loss_rate;
    }

    if (should_drop) {
        return;  // Packet lost
    }

    // Calculate delivery time
    uint64_t delivery_time = current_time_ms_ + conditions_.latency_ms;

    // Queue message for delivery
    {
        std::lock_guard<std::mutex> lock(messages_mutex_);
        pending_messages_.push({delivery_time, peer->connection_id(), data});
    }
}

void SimulatedTransport::process_pending_messages() {
    std::vector<PendingMessage> ready_messages;

    // Collect messages ready for delivery
    {
        std::lock_guard<std::mutex> lock(messages_mutex_);
        while (!pending_messages_.empty() &&
               pending_messages_.front().delivery_time_ms <= current_time_ms_) {
            ready_messages.push_back(pending_messages_.front());
            pending_messages_.pop();
        }
    }

    // Deliver messages
    for (const auto& msg : ready_messages) {
        std::shared_ptr<SimulatedTransportConnection> conn;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            auto it = connections_.find(msg.to_conn_id);
            if (it != connections_.end()) {
                conn = it->second.lock();
            }
        }

        if (conn) {
            conn->deliver_data(msg.data);
        }
    }
}

} // namespace network
} // namespace coinbasechain
