// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_RPC_RPC_CLIENT_HPP
#define COINBASECHAIN_RPC_RPC_CLIENT_HPP

#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace coinbasechain {
namespace rpc {

/**
 * Simple JSON-RPC client for querying the node
 *
 * Uses Unix domain sockets for IPC between cli and node
 * (simpler than HTTP/REST for local communication)
 */
class RPCClient {
public:
    /**
     * Constructor
     * @param socket_path Path to Unix domain socket (e.g., ~/.coinbasechain/node.sock)
     */
    explicit RPCClient(const std::string& socket_path);
    ~RPCClient();

    /**
     * Connect to the node
     * @return true if connected successfully
     */
    bool Connect();

    /**
     * Execute RPC command
     * @param method Method name (e.g., "getinfo", "getblockchaininfo")
     * @param params Command parameters
     * @return Response string (JSON)
     */
    std::string ExecuteCommand(const std::string& method,
                               const std::vector<std::string>& params = {});

    /**
     * Check if connected
     */
    bool IsConnected() const { return socket_fd_ >= 0; }

    /**
     * Disconnect from node
     */
    void Disconnect();

private:
    std::string socket_path_;
    int socket_fd_;
};

} // namespace rpc
} // namespace coinbasechain

#endif // COINBASECHAIN_RPC_RPC_CLIENT_HPP
