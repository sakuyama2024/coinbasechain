// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_RPC_RPC_SERVER_HPP
#define COINBASECHAIN_RPC_RPC_SERVER_HPP

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <map>
#include <vector>

namespace coinbasechain {

// Forward declarations
namespace chain { class ChainParams; }
namespace network { class NetworkManager; }
namespace mining { class CPUMiner; }
namespace validation { class ChainstateManager; }

namespace rpc {

// Simple RPC server using Unix domain sockets (handles CLI queries from coinbasechain-cli)
class RPCServer {
public:
    using CommandHandler = std::function<std::string(const std::vector<std::string>&)>;

    RPCServer(const std::string& socket_path,
             validation::ChainstateManager& chainstate_manager,
             network::NetworkManager& network_manager,
             mining::CPUMiner* miner,
             const chain::ChainParams& params,
             std::function<void()> shutdown_callback = nullptr);
    ~RPCServer();

    bool Start();
    void Stop();
    bool IsRunning() const { return running_; }

private:
    void ServerThread();
    void HandleClient(int client_fd);
    std::string ExecuteCommand(const std::string& method,
                              const std::vector<std::string>& params);
    void RegisterHandlers();

    // Command handlers - Blockchain
    std::string HandleGetInfo(const std::vector<std::string>& params);
    std::string HandleGetBlockchainInfo(const std::vector<std::string>& params);
    std::string HandleGetBlockCount(const std::vector<std::string>& params);
    std::string HandleGetBlockHash(const std::vector<std::string>& params);
    std::string HandleGetBlockHeader(const std::vector<std::string>& params);
    std::string HandleGetBestBlockHash(const std::vector<std::string>& params);
    std::string HandleGetDifficulty(const std::vector<std::string>& params);

    // Command handlers - Mining
    std::string HandleGetMiningInfo(const std::vector<std::string>& params);
    std::string HandleGetNetworkHashPS(const std::vector<std::string>& params);
    std::string HandleStartMining(const std::vector<std::string>& params);
    std::string HandleStopMining(const std::vector<std::string>& params);
    std::string HandleGenerate(const std::vector<std::string>& params);

    // Command handlers - Network
    std::string HandleGetPeerInfo(const std::vector<std::string>& params);
    std::string HandleAddNode(const std::vector<std::string>& params);

    // Command handlers - Control
    std::string HandleStop(const std::vector<std::string>& params);

    // Command handlers - Testing
    std::string HandleSetMockTime(const std::vector<std::string>& params);
    std::string HandleInvalidateBlock(const std::vector<std::string>& params);

private:
    std::string socket_path_;
    validation::ChainstateManager& chainstate_manager_;
    network::NetworkManager& network_manager_;
    mining::CPUMiner* miner_;  // Optional, can be nullptr
    const chain::ChainParams& params_;
    std::function<void()> shutdown_callback_;

    int server_fd_;
    std::atomic<bool> running_;
    std::thread server_thread_;

    std::map<std::string, CommandHandler> handlers_;
};

} // namespace rpc
} // namespace coinbasechain

#endif // COINBASECHAIN_RPC_RPC_SERVER_HPP
