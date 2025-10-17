#ifndef COINBASECHAIN_APPLICATION_HPP
#define COINBASECHAIN_APPLICATION_HPP

#include "network/network_manager.hpp"
#include "chain/block_manager.hpp"
#include "chain/chainparams.hpp"
#include "validation/chainstate_manager.hpp"
#include "rpc/rpc_server.hpp"
#include "mining/miner.hpp"
#include "util/files.hpp"
#include "notifications.hpp"
#include <memory>
#include <string>
#include <filesystem>
#include <atomic>
#include <csignal>
#include <thread>
#include <chrono>

namespace coinbasechain {
namespace app {

/**
 * Application configuration
 */
struct AppConfig {
    // Data directory
    std::filesystem::path datadir;

    // Network configuration
    network::NetworkManager::Config network_config;

    // Chain type (mainnet, testnet, regtest)
    chain::ChainType chain_type = chain::ChainType::MAIN;

    // Suspicious reorg depth (0 = unlimited, default = 100)
    int suspicious_reorg_depth = 100;

    // Logging
    bool verbose = false;

    AppConfig() {
        // Set default data directory
        datadir = util::get_default_datadir();
    }
};

/**
 * Application - Main application coordinator
 *
 * Responsibilities:
 * - Initialize all components in correct order
 * - Manage lifecycle (start/stop)
 * - Handle signals (SIGINT, SIGTERM)
 * - Coordinate shutdown
 */
class Application {
public:
    explicit Application(const AppConfig& config = AppConfig{});
    ~Application();

    // Lifecycle
    bool initialize();
    bool start();
    void stop();
    void wait_for_shutdown();

    // Component access
    network::NetworkManager& network_manager() { return *network_manager_; }
    validation::ChainstateManager& chainstate_manager() { return *chainstate_manager_; }
    const chain::ChainParams& chain_params() const { return *chain_params_; }

    // Status
    bool is_running() const { return running_; }

    // Shutdown request (for RPC stop command)
    void request_shutdown() { shutdown_requested_ = true; }

    // Signal handling
    static void signal_handler(int signal);
    static Application* instance();

private:
    AppConfig config_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_requested_{false};

    // Components (initialized in order)
    std::unique_ptr<chain::ChainParams> chain_params_;
    std::unique_ptr<validation::ChainstateManager> chainstate_manager_;
    std::unique_ptr<network::NetworkManager> network_manager_;
    std::unique_ptr<mining::CPUMiner> miner_;
    std::unique_ptr<rpc::RPCServer> rpc_server_;

    // Periodic save thread
    std::unique_ptr<std::thread> save_thread_;

    // Notification subscriptions
    ChainNotifications::Subscription block_sub_;

    // Initialization steps
    bool init_datadir();
    bool init_randomx();
    bool init_chain();
    bool init_network();
    bool init_rpc();

    // Periodic saves
    void start_periodic_saves();
    void stop_periodic_saves();
    void periodic_save_loop();
    void save_headers();
    void save_peers();

    // Shutdown
    void shutdown();

    // Signal handling
    static Application* instance_;
    void setup_signal_handlers();
};

} // namespace app
} // namespace coinbasechain

#endif // COINBASECHAIN_APPLICATION_HPP
