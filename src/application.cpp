#include "application.hpp"
#include "chain/randomx_pow.hpp"
#include "chain/fs_lock.hpp"
#include "chain/logging.hpp"
#include "version.hpp"
#include <chrono>
#include <iostream> // Keep for signal handler (async-signal-safe)
#include <thread>

namespace coinbasechain {
namespace app {

// Static instance for signal handling
Application *Application::instance_ = nullptr;

Application::Application(const AppConfig &config) : config_(config) {
  instance_ = this;
}

Application::~Application() {
  stop();
  instance_ = nullptr;
}

Application *Application::instance() { return instance_; }

bool Application::initialize() {
  // Determine chain type name for banner
  std::string chain_name;
  switch (config_.chain_type) {
  case chain::ChainType::MAIN:
    chain_name = "MAINNET";
    break;
  case chain::ChainType::TESTNET:
    chain_name = "TESTNET";
    break;
  case chain::ChainType::REGTEST:
    chain_name = "REGTEST";
    break;
  }

  // Print startup banner (use std::cout for immediate visibility before logger
  // fully initialized)
  std::cout << GetStartupBanner(chain_name) << std::flush;

  LOG_INFO("Initializing CoinbaseChain...");

  // Create data directory
  if (!init_datadir()) {
    LOG_ERROR("Failed to initialize data directory");
    return false;
  }

  // Initialize RandomX
  if (!init_randomx()) {
    LOG_ERROR("Failed to initialize RandomX");
    return false;
  }

  // Initialize blockchain (creates chainstate_manager)
  if (!init_chain()) {
    LOG_ERROR("Failed to initialize blockchain");
    return false;
  }

  // Initialize miner (after chainstate is ready)
  LOG_INFO("Initializing miner...");
  miner_ =
      std::make_unique<mining::CPUMiner>(*chain_params_, *chainstate_manager_);

  // Initialize network manager
  if (!init_network()) {
    LOG_ERROR("Failed to initialize network manager");
    return false;
  }

  // Initialize RPC server
  if (!init_rpc()) {
    LOG_ERROR("Failed to initialize RPC server");
    return false;
  }

  // Subscribe to block notifications to relay new blocks to peers
  block_sub_ = Notifications().SubscribeBlockConnected(
      [this](const CBlockHeader &block, const chain::CBlockIndex *pindex) {
        if (pindex && network_manager_) {
          network_manager_->relay_block(pindex->GetBlockHash());
        }
      });

  // Subscribe to suspicious reorg notifications to trigger shutdown
  reorg_sub_ = Notifications().SubscribeSuspiciousReorg(
      [this](int reorg_depth, int max_allowed) {
        LOG_ERROR(
            "Application: Suspicious reorg detected ({} blocks, max {}). "
            "Initiating graceful shutdown to protect chain integrity.",
            reorg_depth, max_allowed);
        request_shutdown();
      });

  // Subscribe to chain tip changes to invalidate miner block templates
  tip_sub_ = Notifications().SubscribeChainTip(
      [this](const chain::CBlockIndex *pindexNew, int height) {
        if (miner_) {
          miner_->InvalidateTemplate();
        }
      });

  LOG_INFO("Initialization complete");
  return true;
}

bool Application::start() {
  if (running_) {
    LOG_ERROR("Application already running");
    return false;
  }

  LOG_INFO("Starting CoinbaseChain...");

  // Setup signal handlers
  setup_signal_handlers();

  // Start network manager
  if (!network_manager_->start()) {
    LOG_ERROR("Failed to start network manager");
    return false;
  }

  // Start RPC server
  if (!rpc_server_->Start()) {
    LOG_ERROR("Failed to start RPC server");
    return false;
  }

  running_ = true;

  // Start periodic save thread
  start_periodic_saves();

  LOG_INFO("CoinbaseChain started successfully");
  LOG_INFO("Data directory: {}", config_.datadir.string());

  if (config_.network_config.listen_enabled) {
    LOG_INFO("Listening on port: {}", config_.network_config.listen_port);
  } else {
    LOG_INFO("Inbound connections disabled");
  }

  LOG_INFO("Press Ctrl+C to stop");

  return true;
}

void Application::stop() {
  if (!running_) {
    return;
  }

  shutdown();
}

void Application::wait_for_shutdown() {
  // Wait for shutdown signal
  while (running_ && !shutdown_requested_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  if (shutdown_requested_) {
    shutdown();
  }
}

void Application::shutdown() {
  if (!running_) {
    return;
  }

  LOG_INFO("Shutting down CoinbaseChain...");

  running_ = false;

  // Stop periodic saves
  stop_periodic_saves();

  // Stop RPC server first (stop accepting new requests)
  if (rpc_server_) {
    LOG_INFO("Stopping RPC server...");
    rpc_server_->Stop();
  }

  // Stop miner if running
  if (miner_ && miner_->IsMining()) {
    LOG_INFO("Stopping miner...");
    miner_->Stop();
  }

  // Stop network manager
  if (network_manager_) {
    LOG_INFO("Stopping network manager...");
    network_manager_->stop();
  }

  // Save headers, peers, and anchors to disk
  if (chainstate_manager_) {
    LOG_INFO("Saving headers to disk...");
    std::string headers_file = (config_.datadir / "headers.json").string();
    if (!chainstate_manager_->Save(headers_file)) {
      LOG_ERROR("Failed to save headers");
    }
  }

  if (network_manager_) {
    LOG_INFO("Saving peer addresses to disk...");
    std::string peers_file = (config_.datadir / "peers.json").string();
    if (!network_manager_->address_manager().Save(peers_file)) {
      LOG_ERROR("Failed to save peer addresses");
    }

    LOG_INFO("Saving anchor connections to disk...");
    std::string anchors_file = (config_.datadir / "anchors.json").string();
    if (!network_manager_->SaveAnchors(anchors_file)) {
      LOG_DEBUG(
          "No anchors to save (this is normal if no peers were connected)");
    }
  }

  // Shutdown RandomX
  LOG_INFO("Shutting down RandomX...");
  crypto::ShutdownRandomX();

  // Release data directory lock
  LOG_INFO("Releasing data directory lock...");
  util::UnlockDirectory(config_.datadir, ".lock");

  LOG_INFO("Shutdown complete");
}

bool Application::init_datadir() {
  LOG_INFO("Data directory: {}", config_.datadir.string());

  if (!util::ensure_directory(config_.datadir)) {
    LOG_ERROR("Failed to create data directory: {}", config_.datadir.string());
    return false;
  }

  // Lock the data directory to prevent multiple instances
  util::LockResult lock_result = util::LockDirectory(config_.datadir, ".lock");

  if (lock_result == util::LockResult::ErrorWrite) {
    LOG_ERROR("Cannot write to data directory: {}", config_.datadir.string());
    return false;
  }

  if (lock_result == util::LockResult::ErrorLock) {
    LOG_ERROR("Cannot obtain a lock on data directory {}. "
              "CoinbaseChain is probably already running.",
              config_.datadir.string());
    return false;
  }

  LOG_DEBUG("Successfully locked data directory");
  return true;
}

bool Application::init_randomx() {
  LOG_INFO("Initializing RandomX...");

  // Initialize with default cache size (light mode only)
  crypto::InitRandomX(crypto::DEFAULT_RANDOMX_VM_CACHE_SIZE);

  return true;
}

bool Application::init_chain() {
  LOG_INFO("Initializing blockchain...");

  // Select chain type globally (needed by NetworkManager)
  chain::GlobalChainParams::Select(config_.chain_type);

  // Create chain params based on type
  switch (config_.chain_type) {
  case chain::ChainType::MAIN:
    chain_params_ = chain::ChainParams::CreateMainNet();
    LOG_INFO("Using mainnet");
    break;
  case chain::ChainType::TESTNET:
    chain_params_ = chain::ChainParams::CreateTestNet();
    LOG_INFO("Using testnet");
    break;
  case chain::ChainType::REGTEST:
    chain_params_ = chain::ChainParams::CreateRegTest();
    LOG_INFO("Using regtest");
    break;
  }

  // Create chainstate manager (which owns BlockManager)
  chainstate_manager_ = std::make_unique<validation::ChainstateManager>(
      *chain_params_, config_.suspicious_reorg_depth);

  // Try to load headers from disk
  std::string headers_file = (config_.datadir / "headers.json").string();
  bool loaded = chainstate_manager_->Load(headers_file);

  if (!loaded) {
    // No existing headers, initialize with genesis block
    LOG_INFO("No existing headers found, initializing with genesis block");
    const CBlockHeader &genesis = chain_params_->GenesisBlock();
    if (!chainstate_manager_->Initialize(genesis)) {
      LOG_ERROR("Failed to initialize blockchain");
      return false;
    }
  } else {
    LOG_INFO("Loaded headers from disk");
  }

  LOG_INFO("Blockchain initialized at height: {}",
           chainstate_manager_->GetChainHeight());

  return true;
}

bool Application::init_network() {
  LOG_INFO("Initializing network manager...");

  // Pass through datadir from AppConfig to NetworkManager config
  config_.network_config.datadir = config_.datadir.string();

  network_manager_ = std::make_unique<network::NetworkManager>(
      *chainstate_manager_, config_.network_config);

  // Load peer addresses from disk
  std::string peers_file = (config_.datadir / "peers.json").string();
  network_manager_->address_manager().Load(peers_file);

  // Load and reconnect to anchor peers (for eclipse attack resistance)
  // Note: The anchors file is deleted after reading (Bitcoin Core behavior)
  std::string anchors_file = (config_.datadir / "anchors.json").string();
  network_manager_->LoadAnchors(anchors_file);

  return true;
}

bool Application::init_rpc() {
  LOG_INFO("Initializing RPC server...");

  std::string socket_path = (config_.datadir / "node.sock").string();

  // Create shutdown callback
  auto shutdown_callback = [this]() { this->request_shutdown(); };

  rpc_server_ = std::make_unique<rpc::RPCServer>(
      socket_path, *chainstate_manager_, *network_manager_, miner_.get(),
      *chain_params_, shutdown_callback);

  return true;
}

void Application::setup_signal_handlers() {
  std::signal(SIGINT, Application::signal_handler);
  std::signal(SIGTERM, Application::signal_handler);
}

void Application::signal_handler(int signal) {
  if (instance_) {
    std::cout << "\nReceived signal " << signal << std::endl;
    instance_->shutdown_requested_ = true;
  }
}

void Application::start_periodic_saves() {
  LOG_INFO("Starting periodic header saves (every 10 minutes)");
  save_thread_ =
      std::make_unique<std::thread>(&Application::periodic_save_loop, this);
}

void Application::stop_periodic_saves() {
  if (save_thread_ && save_thread_->joinable()) {
    LOG_DEBUG("Stopping periodic save thread");
    save_thread_->join();
    save_thread_.reset();
  }
}

void Application::periodic_save_loop() {
  using namespace std::chrono;

  // Bitcoin Core intervals:
  // - Headers: 10 minutes (our choice for headers-only chain)
  // - Peers: 15 minutes (Bitcoin Core default: DUMP_PEERS_INTERVAL)
  const auto header_interval = minutes(10);
  const auto peer_interval = minutes(15);

  auto last_header_save = steady_clock::now();
  auto last_peer_save = steady_clock::now();

  while (running_) {
    std::this_thread::sleep_for(seconds(1));

    if (!running_)
      break;

    auto now = steady_clock::now();

    // Save headers every 10 minutes
    if (now - last_header_save >= header_interval) {
      save_headers();
      last_header_save = now;
    }

    // Save peers every 15 minutes
    if (now - last_peer_save >= peer_interval) {
      save_peers();
      last_peer_save = now;
    }
  }
}

void Application::save_headers() {
  if (!chainstate_manager_) {
    return;
  }

  std::string headers_file = (config_.datadir / "headers.json").string();
  LOG_DEBUG("Periodic save: saving headers to {}", headers_file);

  if (!chainstate_manager_->Save(headers_file)) {
    LOG_ERROR("Periodic header save failed");
  } else {
    LOG_DEBUG("Periodic header save complete ({} headers at height {})",
              chainstate_manager_->GetBlockCount(),
              chainstate_manager_->GetChainHeight());
  }
}

void Application::save_peers() {
  if (!network_manager_) {
    return;
  }

  std::string peers_file = (config_.datadir / "peers.json").string();
  LOG_DEBUG("Periodic save: saving peer addresses to {}", peers_file);

  if (!network_manager_->address_manager().Save(peers_file)) {
    LOG_ERROR("Periodic peer save failed");
  }
}

} // namespace app
} // namespace coinbasechain
