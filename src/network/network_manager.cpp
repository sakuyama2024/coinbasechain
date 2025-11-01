#include "network/network_manager.hpp"
#include <algorithm>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <filesystem>
#include <functional>
#include <optional>
#include <random>
#include <utility>
#include "chain/block_manager.hpp"
#include "chain/chainparams.hpp"
#include "chain/chainstate_manager.hpp"
#include "util/logging.hpp"
#include "util/time.hpp"
#include "chain/validation.hpp"
#include "network/addr_manager.hpp"
#include "network/anchor_manager.hpp"
#include "network/banman.hpp"
#include "network/block_relay_manager.hpp"
#include "network/connection_types.hpp"
#include "network/header_sync_manager.hpp"
#include "network/message.hpp"
#include "network/message_router.hpp"
#include "network/nat_manager.hpp"
#include "network/real_transport.hpp"

namespace coinbasechain {
namespace network {

// Helper to generate random nonce
static uint64_t generate_nonce() {
  static std::random_device rd;
  static std::mt19937_64 gen(rd());
  static std::uniform_int_distribution<uint64_t> dis;
  return dis(gen);
}

NetworkManager::NetworkManager(
    validation::ChainstateManager &chainstate_manager, const Config &config,
    std::shared_ptr<Transport> transport,
    boost::asio::io_context* external_io_context)
    : config_(config), local_nonce_(generate_nonce()), transport_(transport),
      owned_io_context_(external_io_context ? nullptr : std::make_unique<boost::asio::io_context>()),
      io_context_(external_io_context ? *external_io_context : *owned_io_context_),
      chainstate_manager_(chainstate_manager) {

  // Create transport if not provided (use real TCP transport)
  if (!transport_) {
    transport_ = std::make_shared<RealTransport>(config_.io_threads);
  }

  LOG_NET_TRACE("NetworkManager initialized (local nonce: {}, external_io_context: {})",
               local_nonce_, external_io_context ? "yes" : "no");

  // Create components
  addr_manager_ = std::make_unique<AddressManager>();
  peer_manager_ = std::make_unique<PeerManager>(io_context_, *addr_manager_);

  // Create BanMan (with datadir for persistent bans)
  ban_man_ = std::make_unique<BanMan>(config_.datadir);
  if (!config_.datadir.empty()) {
    ban_man_->Load(); // Load existing bans from disk
  }

  // Create NAT manager if enabled
  if (config_.enable_nat) {
    nat_manager_ = std::make_unique<NATManager>();
  }

  // Create AnchorManager with callbacks
  anchor_manager_ = std::make_unique<AnchorManager>(
      *peer_manager_,
      // Callback to convert NetworkAddress to IP string
      [this](const protocol::NetworkAddress& addr) -> std::optional<std::string> {
        return network_address_to_string(addr);
      },
      // Callback to connect to an address (mark anchors as NoBan and whitelist in BanMan)
      [this](const protocol::NetworkAddress& addr, bool noban) {
        auto ip_opt = network_address_to_string(addr);
        if (ip_opt && ban_man_) {
          ban_man_->AddToWhitelist(*ip_opt);
        }
        connect_to_with_permissions(addr, noban ? NetPermissionFlags::NoBan : NetPermissionFlags::None);
      }
  );

  // Create HeaderSyncManager
  header_sync_manager_ = std::make_unique<HeaderSyncManager>(
      chainstate_manager, *peer_manager_, *ban_man_);

  // Create BlockRelayManager
  block_relay_manager_ = std::make_unique<BlockRelayManager>(
      chainstate_manager, *peer_manager_, header_sync_manager_.get());

  // Create MessageRouter
  message_router_ = std::make_unique<MessageRouter>(
      addr_manager_.get(), header_sync_manager_.get(), block_relay_manager_.get(), peer_manager_.get());
  
  // Register callback for peer disconnects (Bitcoin Core FinalizeNode equivalent)
  // This allows HeaderSyncManager to clear sync state when sync peer disconnects
  peer_manager_->SetPeerDisconnectCallback([this](int peer_id) {
    if (header_sync_manager_) {
      header_sync_manager_->OnPeerDisconnected(static_cast<uint64_t>(peer_id));
    }
    if (block_relay_manager_) {
      block_relay_manager_->OnPeerDisconnected(peer_id);
    }
    if (message_router_) {
      message_router_->OnPeerDisconnected(peer_id);
    }
  });
}

NetworkManager::~NetworkManager() {
  // Prevent callbacks from firing into partially destroyed objects
  if (peer_manager_) {
    peer_manager_->SetPeerDisconnectCallback({});
  }
  stop();
}

bool NetworkManager::start() {
  // Fast path: check without lock
  if (running_.load(std::memory_order_acquire)) {
    return false;
  }

  // Acquire lock for initialization
  std::lock_guard<std::mutex> lock(start_stop_mutex_);

  // Double-check after acquiring lock (another thread may have started)
  if (running_.load(std::memory_order_acquire)) {
    return false;
  }

  running_.store(true, std::memory_order_release);

  // Start transport
  if (transport_) {
    transport_->run();
  }

  // Create work guard and timers only if we own the io_context threads
  // When using external io_context (tests), the external code controls event processing
  if (config_.io_threads > 0) {
    // Create work guard to keep io_context running (for timers)
    work_guard_ = std::make_unique<
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(io_context_));

    // Setup timers
    connect_timer_ = std::make_unique<boost::asio::steady_timer>(io_context_);
    maintenance_timer_ = std::make_unique<boost::asio::steady_timer>(io_context_);
    feeler_timer_ = std::make_unique<boost::asio::steady_timer>(io_context_);
    sendmessages_timer_ = std::make_unique<boost::asio::steady_timer>(io_context_);
  }

  // Start IO threads
  for (size_t i = 0; i < config_.io_threads; ++i) {
    io_threads_.emplace_back([this]() { io_context_.run(); });
  }

  // Start listening if enabled (via transport)
  if (config_.listen_enabled && config_.listen_port > 0) {
    bool success = transport_->listen(
        config_.listen_port, [this](TransportConnectionPtr connection) {
          handle_inbound_connection(connection);
        });

    if (success) {
      // Start NAT traversal if enabled
      if (nat_manager_ && nat_manager_->Start(config_.listen_port)) {
        LOG_NET_TRACE("UPnP NAT traversal enabled - external {}:{}",
                     nat_manager_->GetExternalIP(),
                     nat_manager_->GetExternalPort());
      }
    } else {
      LOG_NET_ERROR("Failed to start listener");
      // Continue anyway - we can still make outbound connections
    }
  }

  // Load anchor peers (for eclipse attack resistance)
  // Anchors are the last 2-3 outbound peers we connected to before shutdown
  // We try to reconnect to them first to maintain network view consistency
  if (!config_.datadir.empty()) {
    std::string anchors_path = config_.datadir + "/anchors.json";
    if (std::filesystem::exists(anchors_path)) {
      if (LoadAnchors(anchors_path)) {
        LOG_NET_TRACE("loaded anchors, will connect to them first");
      } else {
        LOG_NET_DEBUG("No anchors loaded from {}", anchors_path);
      }
    }
  }

  // Bootstrap from fixed seeds if AddressManager is empty
  // (matches Bitcoin's ThreadDNSAddressSeed logic: query all seeds if addrman.Size() == 0)
  if (addr_manager_->size() == 0) {
    bootstrap_from_fixed_seeds(chain::GlobalChainParams::Get());
  }

  // Schedule periodic tasks (only if we own the io_context threads)
  // When using external io_context (tests), the external code controls event processing
  if (config_.io_threads > 0) {
    schedule_next_connection_attempt();
    schedule_next_maintenance();
    schedule_next_feeler();
    schedule_next_sendmessages();
  }

  return true;
}

void NetworkManager::stop() {
  // Fast path: check without lock
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  // Acquire lock for shutdown
  std::lock_guard<std::mutex> lock(start_stop_mutex_);

  // Double-check after acquiring lock (another thread may have stopped)
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  running_.store(false, std::memory_order_release);

  // Cancel timers first to prevent new connections/operations from starting
  if (connect_timer_) {
    connect_timer_->cancel();
  }
  if (maintenance_timer_) {
    maintenance_timer_->cancel();
  }
  if (feeler_timer_) {
    feeler_timer_->cancel();
  }
  if (sendmessages_timer_) {
    sendmessages_timer_->cancel();
  }

  // Save anchors BEFORE stopping io_context (which closes all connections)
  // We need active peer connections to query their addresses
  if (!config_.datadir.empty()) {
    std::string anchors_path = config_.datadir + "/anchors.json";
    if (SaveAnchors(anchors_path)) {
      LOG_NET_TRACE("saved anchors for next startup");
    }
  }

  // CRITICAL: Stop io_context after saving anchors
  // This aborts all async I/O operations and closes connections
  // This MUST be done BEFORE explicitly disconnecting peers to prevent their async
  // disconnect callbacks from being processed during shutdown
  // Bitcoin Core pattern: Stop async processing before cleanup
  io_context_.stop();

  // Reset work guard (no longer needed after stopping io_context)
  work_guard_.reset();

  // Now disconnect peers (their callbacks won't be processed since io_context is stopped)
  peer_manager_->disconnect_all();

  // Stop NAT traversal after peers are disconnected
  if (nat_manager_) {
    nat_manager_->Stop();
  }

  // Stop transport (can't post new async operations since io_context is stopped)
  if (transport_) {
    transport_->stop();
  }

  // Join all threads (should return quickly now that io_context is stopped)
  for (auto &thread : io_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  io_threads_.clear();

  // Reset io_context for potential restart
  io_context_.restart();
}

bool NetworkManager::connect_to(const protocol::NetworkAddress &addr) {
  return connect_to_with_permissions(addr, NetPermissionFlags::None);
}

bool NetworkManager::connect_to_with_permissions(const protocol::NetworkAddress &addr, NetPermissionFlags permissions) {
  LOG_NET_TRACE("connect_to() called");

  if (!running_.load(std::memory_order_acquire)) {
    LOG_NET_TRACE("connect_to() failed: not running");
    return false;
  }

  // Convert NetworkAddress to IP string for transport layer
  auto ip_opt = network_address_to_string(addr);
  if (!ip_opt) {
    LOG_NET_ERROR("Failed to convert NetworkAddress to IP string");
    return false;
  }
  const std::string &address = *ip_opt;
  uint16_t port = addr.port;

  // Check if address is banned
  if (ban_man_ && ban_man_->IsBanned(address)) {
    LOG_NET_TRACE("refusing to connect to banned address: {}", address);
    return false;
  }

  // Check if address is discouraged
  if (ban_man_ && ban_man_->IsDiscouraged(address)) {
    LOG_NET_TRACE("refusing to connect to discouraged address: {}", address);
    return false;
  }

  // SECURITY: Prevent duplicate outbound connections to same peer
  // Bitcoin Core: AlreadyConnectedToAddress() check at net.cpp:2881
  // This prevents wasting connection slots and eclipse attack vulnerabilities
  if (already_connected_to_address(address, port)) {
    LOG_NET_TRACE("already connected to {}:{}, skipping duplicate", address, port);
    return false;
  }

  // Check if we can add more outbound connections
  bool needs_more = peer_manager_->needs_more_outbound();
  size_t outbound = peer_manager_->outbound_count();
  LOG_NET_TRACE("connect_to(): needs_more_outbound={}, outbound_count={}", needs_more, outbound);
  if (!needs_more) {
    LOG_NET_TRACE("connect_to() failed: don't need more outbound connections");
    return false;
  }

  // We need to capture the peer_id in the connection callback, but we don't
  // have it yet. Create connection first (won't complete immediately), then
  // create peer, add to manager, THEN store peer_id so callback can find it.
  auto peer_id_ptr = std::make_shared<std::optional<int>>(std::nullopt);

  // Create async transport connection with callback
  // The connection will be in "not open" state until the callback fires
  auto connection = transport_->connect(
      address, port, [this, peer_id_ptr, address, port, addr](bool success) {
        // This callback fires when TCP connection completes (can be very fast!)
        // We need to ensure the peer is in peer_manager before this fires
        LOG_NET_TRACE("CALLBACK FIRED for {}:{}, success={}", address, port, success);

        // Wait briefly to ensure peer has been added to manager
        // (This handles the race where connection completes before we add peer)
        boost::asio::post(io_context_, [this, peer_id_ptr, address, port, addr, success]() {
          if (!peer_id_ptr->has_value()) {
            LOG_NET_TRACE("peer_id_ptr has no value, returning");
            return;
          }

          int peer_id = peer_id_ptr->value();
          LOG_NET_TRACE("Got peer_id={}", peer_id);

          auto peer_ptr = peer_manager_->get_peer(peer_id);
          if (!peer_ptr) {
            LOG_NET_TRACE("Could not get peer {} from manager", peer_id);
            return;
          }

          if (success) {
            // Connection succeeded - mark address as good and start peer protocol
            LOG_NET_TRACE("Connected to {}:{}, starting peer {}", address, port,
                          peer_id);
            addr_manager_->good(addr);
            peer_ptr->start();
          } else {
            // Connection failed - record attempt but keep address for retry (Bitcoin Core pattern)
            LOG_NET_TRACE("Failed to connect to {}:{}, recording attempt and removing peer {}", address,
                          port, peer_id);
            addr_manager_->attempt(addr);
            peer_manager_->remove_peer(peer_id);
          }
        });
      });

  if (!connection) {
    LOG_NET_ERROR("Failed to create connection to {}:{}", address, port);
    return false;
  }

  // Get current blockchain height for VERSION message
  int32_t current_height = chainstate_manager_.GetChainHeight();

  // Create outbound peer with the connection (will be in CONNECTING state)
  // Store target address for duplicate connection prevention (Bitcoin Core pattern)
  auto peer = Peer::create_outbound(io_context_, connection, config_.network_magic,
                                     current_height, address, port);
  if (!peer) {
    LOG_NET_ERROR("Failed to create peer for {}:{}", address, port);
    return false;
  }

  // Setup message handler before adding to manager
  setup_peer_message_handler(peer.get());

  // Add to peer manager and get the assigned peer_id
  int peer_id = peer_manager_->add_peer(std::move(peer), permissions);
  if (peer_id < 0) {
    LOG_NET_ERROR("Failed to add peer to peer manager");
    return false;
  }

  // Store the peer_id so the callback can use it
  *peer_id_ptr = peer_id;
  LOG_NET_TRACE("Added peer {} to manager, stored in callback ptr", peer_id);

  return true;
}

void NetworkManager::disconnect_from(int peer_id) {
  peer_manager_->remove_peer(peer_id);
}

bool NetworkManager::already_connected_to_address(const std::string& address, uint16_t port) {
  // Bitcoin Core pattern: Check existing connections to prevent duplicates
  // Uses peer_manager's find_peer_by_address which iterates through all peers
  // Returns true if we already have a connection to this address:port
  return peer_manager_->find_peer_by_address(address, port) != -1;
}

size_t NetworkManager::active_peer_count() const {
  return peer_manager_->peer_count();
}

size_t NetworkManager::outbound_peer_count() const {
  return peer_manager_->outbound_count();
}

size_t NetworkManager::inbound_peer_count() const {
  return peer_manager_->inbound_count();
}

void NetworkManager::bootstrap_from_fixed_seeds(const chain::ChainParams &params) {
  // Bootstrap AddressManager from hardcoded seed nodes
  // (follows Bitcoin's ThreadDNSAddressSeed logic when addrman is empty)

  const auto &fixed_seeds = params.FixedSeeds();

  if (fixed_seeds.empty()) {
  LOG_NET_TRACE("no fixed seeds available for bootstrap");
    return;
  }

  LOG_NET_INFO("Bootstrapping from {} fixed seed nodes", fixed_seeds.size());

  // Use AddressManager's time format (seconds since epoch via system_clock)
  uint32_t current_time = static_cast<uint32_t>(
      std::chrono::system_clock::now().time_since_epoch().count() / 1000000000);
  size_t added_count = 0;

  // Parse each "IP:port" string and add to AddressManager
  for (const auto &seed_str : fixed_seeds) {
    // Parse IP:port format (e.g., "178.18.251.16:9590")
    size_t colon_pos = seed_str.find(':');
    if (colon_pos == std::string::npos) {
      LOG_NET_WARN("Invalid seed format (missing port): {}", seed_str);
      continue;
    }

    std::string ip_str = seed_str.substr(0, colon_pos);
    std::string port_str = seed_str.substr(colon_pos + 1);

    // Parse port
    uint16_t port = 0;
    try {
      int port_int = std::stoi(port_str);
      if (port_int <= 0 || port_int > 65535) {
        LOG_NET_WARN("Invalid port in seed: {}", seed_str);
        continue;
      }
      port = static_cast<uint16_t>(port_int);
    } catch (const std::exception &e) {
      LOG_NET_WARN("Failed to parse port in seed {}: {}", seed_str, e.what());
      continue;
    }

    // Parse IP address using boost::asio
    try {
      boost::system::error_code ec;
      auto ip_addr = boost::asio::ip::make_address(ip_str, ec);

      if (ec) {
        LOG_NET_WARN("Failed to parse IP in seed {}: {}", seed_str, ec.message());
        continue;
      }

      // Create NetworkAddress
      protocol::NetworkAddress addr;
      addr.services = protocol::ServiceFlags::NODE_NETWORK;
      addr.port = port;

      // Convert to 16-byte IPv6 format (IPv4-mapped if needed)
      if (ip_addr.is_v4()) {
        // Convert IPv4 to IPv4-mapped IPv6 (::FFFF:x.x.x.x)
        auto v6_mapped = boost::asio::ip::make_address_v6(
            boost::asio::ip::v4_mapped, ip_addr.to_v4());
        auto bytes = v6_mapped.to_bytes();
        std::copy(bytes.begin(), bytes.end(), addr.ip.begin());
      } else {
        // Pure IPv6
        auto bytes = ip_addr.to_v6().to_bytes();
        std::copy(bytes.begin(), bytes.end(), addr.ip.begin());
      }

      // Add to AddressManager with current timestamp
      if (addr_manager_->add(addr, current_time)) {
        added_count++;
        LOG_NET_DEBUG("Added seed node: {}", seed_str);
      }

    } catch (const std::exception &e) {
      LOG_NET_WARN("Exception parsing seed {}: {}", seed_str, e.what());
      continue;
    }
  }

  LOG_NET_INFO("Successfully added {} seed nodes to AddressManager", added_count);
}

std::optional<std::string> NetworkManager::network_address_to_string(
    const protocol::NetworkAddress& addr) {
  try {
    // Convert 16-byte array to boost::asio IP address
    boost::asio::ip::address_v6::bytes_type bytes;
    std::copy(addr.ip.begin(), addr.ip.end(), bytes.begin());
    auto v6_addr = boost::asio::ip::make_address_v6(bytes);

    // Check if it's IPv4-mapped and convert if needed
    if (v6_addr.is_v4_mapped()) {
      return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6_addr)
                 .to_string();
    } else {
      return v6_addr.to_string();
    }
  } catch (const std::exception &e) {
    LOG_NET_WARN("Failed to convert NetworkAddress to string: {}", e.what());
    return std::nullopt;
  }
}

void NetworkManager::attempt_outbound_connections() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  const int nTries = 100;

  // Check if we need more outbound connections
  for (int i = 0; i < nTries && peer_manager_->needs_more_outbound(); i++) {
    // Select an address from the address manager
    auto maybe_addr = addr_manager_->select();
    if (!maybe_addr) {
      break; // No addresses available
    }

    auto &addr = *maybe_addr;

    // Convert NetworkAddress to IP string for logging
    auto maybe_ip_str = network_address_to_string(addr);
    if (!maybe_ip_str) {
      LOG_NET_WARN("Failed to convert address to string, marking as failed");
      addr_manager_->failed(addr);
      continue;
    }

    const std::string &ip_str = *maybe_ip_str;

    if (already_connected_to_address(ip_str, addr.port)) {
      continue;
    }

    LOG_NET_TRACE("Attempting outbound connection to {}:{}", ip_str, addr.port);

    // Mark as attempt (connection may still fail)
    addr_manager_->attempt(addr);

    // Try to connect
    // Bitcoin Core: Don't mark as failed here for other failure cases
    // The connection callback will mark as failed for actual network errors
    if (!connect_to(addr)) {
      LOG_NET_TRACE("Failed to initiate connection to {}:{}", ip_str, addr.port);
      // Note: Don't call addr_manager_->failed() here
      // Real connection failures are handled in the connection callback
    }
  }
}

void NetworkManager::schedule_next_connection_attempt() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  connect_timer_->expires_after(config_.connect_interval);
  connect_timer_->async_wait([this](const boost::system::error_code &ec) {
    if (!ec && running_.load(std::memory_order_acquire)) {
      attempt_outbound_connections();
      schedule_next_connection_attempt();
    }
  });
}

void NetworkManager::handle_inbound_connection(
    TransportConnectionPtr connection) {
  if (!running_.load(std::memory_order_acquire) || !connection) {
    return;
  }

  // Get remote address for ban checking
  std::string remote_address = connection->remote_address();

  // Check if address is banned
  if (ban_man_ && ban_man_->IsBanned(remote_address)) {
    LOG_NET_INFO("Rejected banned address: {}", remote_address);
    connection->close();
    return;
  }

  // Check if address is discouraged 
  if (ban_man_ && ban_man_->IsDiscouraged(remote_address)) {
    LOG_NET_INFO("Rejected discouraged address: {}", remote_address);
    connection->close();
    return;
  }

  // Check if we can accept more inbound connections (global and per-IP)
  if (!peer_manager_->can_accept_inbound_from(remote_address)) {
    LOG_NET_TRACE("Rejecting inbound connection from {} (inbound limit reached)",
                  remote_address);
    connection->close();
    return;
  }

  // Get current blockchain height for VERSION message
  int32_t current_height = chainstate_manager_.GetChainHeight();

  // Create inbound peer
  auto peer = Peer::create_inbound(io_context_, connection,
                                   config_.network_magic, current_height);
  if (peer) {
    // Setup message handler
    setup_peer_message_handler(peer.get());

    // Start the peer (waits for VERSION from peer)
    peer->start();

    // Add to peer manager (use default_inbound_permissions_ for testing)
    int peer_id = peer_manager_->add_peer(std::move(peer), default_inbound_permissions_);
    if (peer_id < 0) {
      LOG_NET_ERROR("Failed to add inbound peer to manager");
    }
  }
}

void NetworkManager::run_maintenance() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  // Run periodic cleanup
  peer_manager_->process_periodic();

  // Headers sync timeouts and maintenance
  if (header_sync_manager_) {
    header_sync_manager_->ProcessTimers();
  }

  // Sweep expired bans and discouraged entries
  if (ban_man_) {
    ban_man_->SweepBanned();
    ban_man_->SweepDiscouraged();
  }

  // Periodically announce our tip to peers (Bitcoin Core pattern: queue only, SendMessages loop flushes)
  if (block_relay_manager_) {
    block_relay_manager_->AnnounceTipToAllPeers();
  }

  // TODO: Sync timeout logic should be moved to HeaderSyncManager
  // For now, HeaderSyncManager handles its own timeout tracking internally

  // Check if we need to start initial sync
  check_initial_sync();
}

void NetworkManager::schedule_next_maintenance() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  maintenance_timer_->expires_after(config_.maintenance_interval);
  maintenance_timer_->async_wait([this](const boost::system::error_code &ec) {
    if (!ec && running_.load(std::memory_order_acquire)) {
      run_maintenance();
      schedule_next_maintenance();
    }
  });
}

void NetworkManager::run_sendmessages() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  // Check for initial sync opportunities (Bitcoin Core SendMessages pattern)
  if (header_sync_manager_) {
    header_sync_manager_->CheckInitialSync();
  }

  // Flush queued block announcements (Bitcoin Core SendMessages pattern)
  if (block_relay_manager_) {
    LOG_NET_TRACE("SendMessages: flushing block announcements");
    block_relay_manager_->FlushBlockAnnouncements();
  }
}

void NetworkManager::schedule_next_sendmessages() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  sendmessages_timer_->expires_after(SENDMESSAGES_INTERVAL);
  sendmessages_timer_->async_wait([this](const boost::system::error_code &ec) {
    if (!ec && running_.load(std::memory_order_acquire)) {
      run_sendmessages();
      schedule_next_sendmessages();
    }
  });
}

void NetworkManager::test_hook_check_initial_sync() {
  // Expose initial sync trigger for tests when io_threads==0
  check_initial_sync();
}

void NetworkManager::test_hook_header_sync_process_timers() {
  // Expose header sync stall/timeout processing for tests (io_threads==0)
  if (header_sync_manager_) {
    header_sync_manager_->ProcessTimers();
  }
}

void NetworkManager::schedule_next_feeler() {
  LOG_NET_TRACE("schedule_next_feeler: ENTER (running={}, has_timer={})",
                running_.load(std::memory_order_acquire), (feeler_timer_ != nullptr));

  if (!running_.load(std::memory_order_acquire) || !feeler_timer_) {
    LOG_NET_TRACE("schedule_next_feeler: ABORT (not running or no timer)");
    return;
  }

  // Exponential/Poisson scheduling around mean FEELER_INTERVAL
  static thread_local std::mt19937 rng(std::random_device{}());
  std::exponential_distribution<double> exp(1.0 / std::chrono::duration_cast<std::chrono::seconds>(FEELER_INTERVAL).count());
  double delay_s = exp(rng);
  auto delay = std::chrono::seconds(std::max(1, static_cast<int>(delay_s)));

  feeler_timer_->expires_after(delay);
  LOG_NET_TRACE("schedule_next_feeler: Timer set for {}s (Poisson)", delay.count());

  feeler_timer_->async_wait([this](const boost::system::error_code &ec) {
    LOG_NET_TRACE("schedule_next_feeler: Timer fired (ec={}, running={})",
                  ec.message(), running_.load(std::memory_order_acquire));
    if (!ec && running_.load(std::memory_order_acquire)) {
      attempt_feeler_connection();
      schedule_next_feeler();
    }
  });
}

void NetworkManager::attempt_feeler_connection() {
  LOG_NET_TRACE("attempt_feeler_connection: ENTER (running={})",
                running_.load(std::memory_order_acquire));

  if (!running_.load(std::memory_order_acquire)) {
    LOG_NET_TRACE("attempt_feeler_connection: ABORT (not running)");
    return;
  }

  // Get address from "new" table (addresses we've heard about but never connected to)
  LOG_NET_TRACE("attempt_feeler_connection: Selecting address from NEW table");
  auto addr = addr_manager_->select_new_for_feeler();
  if (!addr) {
    LOG_NET_TRACE("attempt_feeler_connection: No address found in NEW table");
    return;
  }

  // Convert NetworkAddress to IP string
  auto addr_str_opt = network_address_to_string(*addr);
  if (!addr_str_opt) {
    LOG_NET_TRACE("attempt_feeler_connection: Address conversion failed");
    return;
  }

  std::string address = *addr_str_opt;
  uint16_t port = addr->port;

  LOG_NET_TRACE("attempt_feeler_connection: Selected address={}:{}", address, port);

  // Create peer_id container for async callback
  auto peer_id_ptr = std::make_shared<std::optional<int>>(std::nullopt);

  // Create async transport connection with callback (same pattern as connect_to())
  LOG_NET_TRACE("attempt_feeler_connection: Creating transport connection to {}:{}", address, port);

  auto connection = transport_->connect(
      address, port, [this, peer_id_ptr, address, port, addr](bool success) {
        LOG_NET_TRACE("attempt_feeler_connection: TCP callback fired (success={}, peer_id_set={})",
                      success, peer_id_ptr->has_value());

        // Callback fires when TCP connection completes
        boost::asio::post(io_context_, [this, peer_id_ptr, address, port, addr, success]() {
          LOG_NET_TRACE("attempt_feeler_connection: Posted callback executing (peer_id_set={})",
                        peer_id_ptr->has_value());

          if (!peer_id_ptr->has_value()) {
            LOG_NET_TRACE("attempt_feeler_connection: Callback aborted (peer_id not set)");
            return;
          }

          int peer_id = peer_id_ptr->value();
          LOG_NET_TRACE("attempt_feeler_connection: Looking up peer_id={}", peer_id);
          auto peer_ptr = peer_manager_->get_peer(peer_id);
          if (!peer_ptr) {
            LOG_NET_TRACE("attempt_feeler_connection: Peer {} not found in manager", peer_id);
            return;
          }

          if (success) {
            // Connection succeeded - mark address as good and start peer protocol
            // The peer will auto-disconnect after VERACK (see handle_verack())
            LOG_NET_TRACE("attempt_feeler_connection: Connection SUCCESS - marking address good, starting peer {}",
                          peer_id);
            addr_manager_->good(*addr);
            peer_ptr->start();
          } else {
            // Connection failed - record attempt but keep address for retry
            LOG_NET_TRACE("attempt_feeler_connection: Connection FAILED - recording attempt and removing peer {}",
                          peer_id);
            addr_manager_->attempt(*addr);
            peer_manager_->remove_peer(peer_id);
          }
        });
      });

  if (!connection) {
    LOG_NET_TRACE("attempt_feeler_connection: Transport connection creation FAILED");
    addr_manager_->attempt(*addr);
    return;
  }

  LOG_NET_TRACE("attempt_feeler_connection: Transport connection created successfully");

  // Get current blockchain height for VERSION message
  int32_t current_height = chainstate_manager_.GetChainHeight();
  LOG_NET_TRACE("attempt_feeler_connection: Current chain height={}", current_height);

  // Create peer with FEELER connection type (doesn't count against outbound limit)
  LOG_NET_TRACE("attempt_feeler_connection: Creating FEELER peer object");
  auto peer = Peer::create_outbound(io_context_, connection, config_.network_magic,
                                     current_height, address, port,
                                     ConnectionType::FEELER);

  // Setup message handler before adding to manager
  LOG_NET_TRACE("attempt_feeler_connection: Setting up message handler");
  setup_peer_message_handler(peer.get());

  // Add to peer manager and store ID for callback
  LOG_NET_TRACE("attempt_feeler_connection: Adding peer to manager");
  int peer_id = peer_manager_->add_peer(peer);
  if (peer_id < 0) {
    LOG_NET_TRACE("attempt_feeler_connection: Failed to add peer to manager (peer_id={})", peer_id);
    return;
  }

  // Store peer_id for callback access
  *peer_id_ptr = peer_id;
  LOG_NET_TRACE("attempt_feeler_connection: EXIT (peer_id={}, waiting for TCP callback)", peer_id);
}

void NetworkManager::check_initial_sync() {
  // Delegate to HeaderSyncManager
  if (header_sync_manager_) {
    header_sync_manager_->CheckInitialSync();
  }
}

void NetworkManager::setup_peer_message_handler(Peer *peer) {
  peer->set_message_handler(
      [this](PeerPtr peer, std::unique_ptr<message::Message> msg) {
        return handle_message(peer, std::move(msg));
      });

  // No need to register peer - network::PeerManager already tracks all peers
  // Misbehavior tracking is now handled by the unified PeerManager
}

// Bitcoin Core CheckIncomingNonce pattern (net.cpp:370-378)
// Check if incoming nonce matches any outbound peer's local nonce
// Returns true if OK (not self-connection), false if self-connection detected
bool NetworkManager::check_incoming_nonce(uint64_t nonce) {
  // Loop through all peers looking for outbound peers
  auto peers = peer_manager_->get_all_peers();
  for (const auto& peer : peers) {
    // Only check outbound peers that haven't completed handshake yet
    // Bitcoin Core: !pnode->fSuccessfullyConnected && !pnode->IsInboundConn()
    if (!peer->successfully_connected() && !peer->is_inbound()) {
      // Check if this outbound peer's nonce matches the incoming nonce
      if (peer->get_local_nonce() == nonce) {
        LOG_NET_INFO("Self-connection detected: incoming nonce {} matches outbound peer {}",
                     nonce, peer->address());
        return false;  // Self-connection detected!
      }
    }
  }
  return true;  // Not a self-connection
}

bool NetworkManager::handle_message(PeerPtr peer,
                                    std::unique_ptr<message::Message> msg) {
  // SECURITY: Bitcoin Core pattern - detect SELF-connections in VERSION handler
  // Reference: net_processing.cpp:3453-3459 ProcessMessage() for "version"
  // This ONLY detects when we connect to ourselves (e.g., 127.0.0.1)
  // It does NOT handle bidirectional connections between different nodes
  if (msg->command() == protocol::commands::VERSION) {
    auto* version_msg = static_cast<message::VersionMessage*>(msg.get());

    // Bitcoin Core: Only check INBOUND connections for self-connection
    // If this is an inbound peer, check if their nonce matches any of our
    // outbound peers' nonces (which would mean we connected to ourselves)
    if (peer->is_inbound()) {
      if (!check_incoming_nonce(version_msg->nonce)) {
        // Self-connection detected! Disconnect this peer.
        LOG_NET_INFO("Connected to self at {}, disconnecting", peer->address());
        int peer_id = peer->id();
        peer->disconnect();
        peer_manager_->remove_peer(peer_id);
        return false;  // Don't route the message
      }
    }
  }

  if (message_router_) {
    return message_router_->RouteMessage(peer, std::move(msg));
  }
  return false;
}

void NetworkManager::request_headers_from_peer(PeerPtr peer) {
  if (header_sync_manager_) {
    header_sync_manager_->RequestHeadersFromPeer(peer);
  }
}

bool NetworkManager::handle_headers_message(PeerPtr peer,
                                            message::HeadersMessage *msg) {
  if (header_sync_manager_) {
    return header_sync_manager_->HandleHeadersMessage(peer, msg);
  }
  return false;
}

bool NetworkManager::handle_getheaders_message(
    PeerPtr peer, message::GetHeadersMessage *msg) {
  if (header_sync_manager_) {
    return header_sync_manager_->HandleGetHeadersMessage(peer, msg);
  }
  return false;
}

// Anchors implementation for eclipse attack resistance (delegated to AnchorManager)
std::vector<protocol::NetworkAddress> NetworkManager::GetAnchors() const {
  return anchor_manager_->GetAnchors();
}

bool NetworkManager::SaveAnchors(const std::string &filepath) {
  return anchor_manager_->SaveAnchors(filepath);
}

bool NetworkManager::LoadAnchors(const std::string &filepath) {
  return anchor_manager_->LoadAnchors(filepath);
}

bool NetworkManager::handle_inv_message(PeerPtr peer,
                                        message::InvMessage *msg) {
  if (block_relay_manager_) {
    return block_relay_manager_->HandleInvMessage(peer, msg);
  }
  return false;
}

void NetworkManager::announce_tip_to_peers() {
  if (block_relay_manager_) {
    block_relay_manager_->AnnounceTipToAllPeers();
  }
}

void NetworkManager::announce_tip_to_peer(Peer* peer) {
  if (block_relay_manager_) {
    block_relay_manager_->AnnounceTipToPeer(peer);
  }
}

void NetworkManager::flush_block_announcements() {
  if (block_relay_manager_) {
    block_relay_manager_->FlushBlockAnnouncements();
  }
}

void NetworkManager::relay_block(const uint256 &block_hash) {
  if (block_relay_manager_) {
    block_relay_manager_->RelayBlock(block_hash);
  }
}

} // namespace network
} // namespace coinbasechain
