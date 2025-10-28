// Copyright (c) 2024 Coinbase Chain
// NetworkBridgedTransport implementation

#include "network_bridged_transport.hpp"
#include "chain/logging.hpp"
#include "network/message.hpp"
#include <sstream>

namespace coinbasechain {
namespace test {

// ============================================================================
// BridgedConnection
// ============================================================================

NetworkBridgedTransport::BridgedConnection::BridgedConnection(
    uint64_t id,
    bool is_inbound,
    int peer_node_id,
    NetworkBridgedTransport* transport)
    : id_(id)
    , is_inbound_(is_inbound)
    , peer_node_id_(peer_node_id)
    , transport_(transport)
{
}

NetworkBridgedTransport::BridgedConnection::~BridgedConnection() {
    close();
}

void NetworkBridgedTransport::BridgedConnection::start() {
    // Connection is ready immediately in simulated network
}

bool NetworkBridgedTransport::BridgedConnection::send(const std::vector<uint8_t>& data) {
    protocol::MessageHeader hdr;
    std::string cmd = "<invalid>";
    if (data.size() >= protocol::MESSAGE_HEADER_SIZE && message::deserialize_header(data.data(), data.size(), hdr)) {
        cmd = hdr.get_command();
    }
    if (!open_) {
        LOG_NET_INFO("bridge: send DROP (closed) node={} -> peer_node={} cmd={} size={}",
                     transport_ ? transport_->node_id_ : -1, peer_node_id_, cmd, data.size());
        return false;
    }

    LOG_NET_INFO("bridge: send node={} -> peer_node={} cmd={} size={}",
                 transport_ ? transport_->node_id_ : -1, peer_node_id_, cmd, data.size());

    // Route through SimulatedNetwork
    if (transport_ && transport_->sim_network_) {
        transport_->sim_network_->SendMessage(
            transport_->node_id_,
            peer_node_id_,
            data
        );
    }

    return true;
}

void NetworkBridgedTransport::BridgedConnection::close() {
    if (!open_.exchange(false)) {
        return;  // Already closed
    }

    // Notify local side
    if (disconnect_callback_) {
        disconnect_callback_();
    }

    // Notify remote side by closing their connection too
    if (transport_) {
        transport_->notify_peer_disconnect(peer_node_id_, transport_->node_id_);
    }
}

void NetworkBridgedTransport::BridgedConnection::set_receive_callback(
    network::ReceiveCallback callback)
{
    receive_callback_ = callback;
}

void NetworkBridgedTransport::BridgedConnection::set_disconnect_callback(
    network::DisconnectCallback callback)
{
    disconnect_callback_ = callback;
}

std::string NetworkBridgedTransport::BridgedConnection::remote_address() const {
    std::ostringstream oss;
    oss << "127.0.0." << (peer_node_id_ % 255);
    return oss.str();
}

void NetworkBridgedTransport::BridgedConnection::deliver_data(
    const std::vector<uint8_t>& data)
{
    // Log command being delivered to local peer
    protocol::MessageHeader hdr;
    if (data.size() >= protocol::MESSAGE_HEADER_SIZE && message::deserialize_header(data.data(), data.size(), hdr)) {
        LOG_NET_INFO("bridge: deliver_data to node={} from peer_node={} cmd={}",
                     transport_ ? transport_->node_id_ : -1, peer_node_id_, hdr.get_command());
    }
    if (open_ && receive_callback_) {
        receive_callback_(data);
    }
}

void NetworkBridgedTransport::BridgedConnection::close_from_remote() {
    if (!open_.exchange(false)) {
        return;  // Already closed
    }

    // Notify local side only (don't notify remote - they initiated the close)
    if (disconnect_callback_) {
        disconnect_callback_();
    }
}

// ============================================================================
// NetworkBridgedTransport
// ============================================================================

NetworkBridgedTransport::NetworkBridgedTransport(
    int node_id,
    SimulatedNetwork* sim_network)
    : node_id_(node_id)
    , sim_network_(sim_network)
{
    // Register with SimulatedNetwork to receive messages
    // Note: We don't pass the node pointer here because the node isn't fully constructed yet
    // The node will need to update the registration after construction
    if (sim_network_) {
        sim_network_->RegisterNode(node_id, [this](int from_node_id, const std::vector<uint8_t>& data) {
            this->deliver_message(from_node_id, data);
        }, nullptr);
    }
}

NetworkBridgedTransport::~NetworkBridgedTransport() {
    stop();
    if (sim_network_) {
        sim_network_->UnregisterNode(node_id_);
    }
}

network::TransportConnectionPtr NetworkBridgedTransport::connect(
    const std::string& address,
    uint16_t port,
    network::ConnectCallback callback)
{
    // Extract peer node_id from address (127.0.0.X -> X)
    int peer_node_id = 0;
    size_t last_dot = address.rfind('.');
    if (last_dot != std::string::npos) {
        peer_node_id = std::stoi(address.substr(last_dot + 1));
    }

    // Create outbound connection
    uint64_t conn_id = next_connection_id_++;
    auto connection = std::make_shared<BridgedConnection>(
        conn_id,
        false,  // outbound
        peer_node_id,
        this
    );

    // Register connection
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_[conn_id] = connection;
    }

    // Register peer mapping
    {
        std::lock_guard<std::mutex> lock(peer_map_mutex_);
        peer_to_connection_[peer_node_id] = conn_id;
    }

    // Register connection with simulated network (for disconnect purging)
    if (sim_network_) {
        sim_network_->RegisterConnection(node_id_, peer_node_id);
    }

    // Connection succeeds immediately in simulated network
    if (callback) {
        callback(true);
    }

    return connection;
}

bool NetworkBridgedTransport::listen(
    uint16_t port,
    std::function<void(network::TransportConnectionPtr)> accept_callback)
{
    listen_port_ = port;
    accept_callback_ = accept_callback;
    return true;
}

void NetworkBridgedTransport::stop_listening() {
    listen_port_ = 0;
    accept_callback_ = nullptr;
}

void NetworkBridgedTransport::run() {
    running_ = true;
}

void NetworkBridgedTransport::stop() {
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

void NetworkBridgedTransport::deliver_message(
    int from_node_id,
    const std::vector<uint8_t>& data)
{
    // Decode command for logging
    protocol::MessageHeader hdr;
    std::string cmd = "<invalid>";
    if (data.size() >= protocol::MESSAGE_HEADER_SIZE && message::deserialize_header(data.data(), data.size(), hdr)) {
        cmd = hdr.get_command();
    }
    LOG_NET_INFO("bridge: deliver_message from={} to={} cmd={} size={}", from_node_id, node_id_, cmd, data.size());

    // Find connection for this peer
    uint64_t conn_id = 0;
    {
        std::lock_guard<std::mutex> lock(peer_map_mutex_);
        auto it = peer_to_connection_.find(from_node_id);
        if (it != peer_to_connection_.end()) {
            conn_id = it->second;
        }
    }

    std::shared_ptr<BridgedConnection> connection;

    if (conn_id > 0) {
        // Found existing connection
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(conn_id);
        if (it != connections_.end()) {
            connection = it->second.lock();
        }
    } else {
        // No existing connection - this must be an inbound connection
        // Create it now
        if (accept_callback_) {
            conn_id = next_connection_id_++;
            connection = std::make_shared<BridgedConnection>(
                conn_id,
                true,  // inbound
                from_node_id,
                this
            );

            // Register connection
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                connections_[conn_id] = connection;
            }

            // Register peer mapping
            {
                std::lock_guard<std::mutex> lock(peer_map_mutex_);
                peer_to_connection_[from_node_id] = conn_id;
            }

            // Register connection with simulated network (for disconnect purging)
            if (sim_network_) {
                sim_network_->RegisterConnection(from_node_id, node_id_);
            }

            // Notify listener
            accept_callback_(connection);
        }
    }

    // Deliver data to connection
    if (connection) {
        connection->deliver_data(data);
    }
}

void NetworkBridgedTransport::notify_peer_disconnect(int peer_node_id, int disconnecting_node_id) {
    // Route through SimulatedNetwork to notify the remote peer
    if (sim_network_) {
        sim_network_->NotifyDisconnect(node_id_, peer_node_id);
    }
}

void NetworkBridgedTransport::handle_remote_disconnect(int disconnecting_node_id) {
    // Find the connection for this peer
    uint64_t conn_id = 0;
    {
        std::lock_guard<std::mutex> lock(peer_map_mutex_);
        auto it = peer_to_connection_.find(disconnecting_node_id);
        if (it != peer_to_connection_.end()) {
            conn_id = it->second;
        }
    }

    if (conn_id > 0) {
        std::shared_ptr<BridgedConnection> connection;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            auto it = connections_.find(conn_id);
            if (it != connections_.end()) {
                connection = it->second.lock();
            }
        }

        // Close the connection from the remote side
        if (connection && connection->is_open()) {

            // Clean up from maps
            {
                std::lock_guard<std::mutex> lock(peer_map_mutex_);
                peer_to_connection_.erase(disconnecting_node_id);
            }
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                connections_.erase(conn_id);
            }

            // Close connection (this will trigger disconnect_callback_ without notifying remote again)
            connection->close_from_remote();
        }
    }
}

} // namespace test
} // namespace coinbasechain
