#pragma once

#include <stddef.h>
#include <stdint.h>
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "network/connection_types.hpp"
#include "network/peer.hpp"
#include "network/peer_manager.hpp"
#include "network/protocol.hpp"
#include "network/transport.hpp"

// Forward declarations
class uint256;

namespace boost {
namespace asio {
template <typename Executor, typename, typename>
class executor_work_guard;
}
} // namespace boost

namespace coinbasechain {

namespace chain {
class ChainParams;
}

namespace message {
class GetHeadersMessage;
class HeadersMessage;
class InvMessage;
class Message;
}

namespace validation {
class ChainstateManager;
}

namespace network {

// Forward declarations
class AddressManager;
class AnchorManager;
class BanMan;
class BlockRelayManager;
class HeaderSyncManager;
class MessageRouter;
class NATManager;

// NetworkManager - Top-level coordinator for all networking (inspired by
// Bitcoin's CConnman) Manages io_context, coordinates
// PeerManager/AddressManager, handles connections, routes messages
class NetworkManager {
public:
  struct Config {
    uint32_t network_magic; // Network magic bytes (REQUIRED - must be set based on chain type)
    uint16_t listen_port;   // Port to listen on (REQUIRED - must be set based on chain type, 0 = don't listen)
    bool listen_enabled;    // Enable inbound connections
    bool enable_nat;        // Enable UPnP NAT traversal
    size_t io_threads;      // Number of IO threads
    std::string datadir;    // Data directory

    std::chrono::seconds connect_interval; // Time between connection attempts
    std::chrono::seconds maintenance_interval; // Time between maintenance tasks

    // SECURITY: network_magic and listen_port have NO defaults
    // They must be explicitly set based on chain type to prevent
    // accidental mainnet/testnet/regtest network confusion
    Config()
        : network_magic(0),
          listen_port(0),
          listen_enabled(true), 
          enable_nat(true), io_threads(4), datadir(""),
          connect_interval(std::chrono::seconds(5)),
          maintenance_interval(std::chrono::seconds(30)) {}
  };

  explicit NetworkManager(validation::ChainstateManager &chainstate_manager,
                           const Config &config = Config{},
                           std::shared_ptr<Transport> transport = nullptr,
                           boost::asio::io_context* external_io_context = nullptr);
  ~NetworkManager();

  // Lifecycle
  bool start();
  void stop();
  bool is_running() const { return running_; }

  // Component access
  PeerManager &peer_manager() { return *peer_manager_; }
  AddressManager &address_manager() { return *addr_manager_; }
  BanMan &ban_man() { return *ban_man_; }

  // Manual connection management
  bool connect_to(const protocol::NetworkAddress &addr,
                  ConnectionType conn_type = ConnectionType::OUTBOUND);
  void disconnect_from(int peer_id);

  // Block relay
  void relay_block(const uint256 &block_hash);

  // Periodic tip announcements (public for testing/simulation)
  void announce_tip_to_peers();
  
  // Announce tip to a single peer (called when peer becomes READY)
  void announce_tip_to_peer(Peer* peer);
  
  // Flush pending block announcements from all peers' queues
  void flush_block_announcements();

  // Test-only hook: trigger initial sync selection (normally run via timers)
  void test_hook_check_initial_sync();

  // Test-only hook: trigger headers sync timeout processing (stall detection)
  void test_hook_header_sync_process_timers();

  // Self-connection prevention
  uint64_t get_local_nonce() const { return local_nonce_; }

  // Bitcoin Core CheckIncomingNonce pattern - checks if incoming nonce matches
  // any of our outbound peers' local nonces (indicates self-connection)
  // Returns true if nonce is OK (not a self-connection), false if self-connection detected
  bool check_incoming_nonce(uint64_t nonce);

  // Test-only: Set default permissions for inbound connections
  void set_default_inbound_permissions(NetPermissionFlags flags) {
    default_inbound_permissions_ = flags;
  }

  // Test-only: Manually trigger a feeler connection attempt
  void attempt_feeler_connection();

  // Stats (used primarily in tests, but useful for monitoring/debugging)
  size_t active_peer_count() const;
  size_t outbound_peer_count() const;
  size_t inbound_peer_count() const;

  // Anchors
  std::vector<protocol::NetworkAddress> GetAnchors() const;
  bool SaveAnchors(const std::string &filepath);
  bool LoadAnchors(const std::string &filepath);

private:
  Config config_;
  std::atomic<bool> running_{false};
  mutable std::mutex start_stop_mutex_;  // Protects start/stop from race conditions

  // Self-connection prevention: unique nonce for this node
  uint64_t local_nonce_;

  // Test-only: Default permissions for inbound connections
  NetPermissionFlags default_inbound_permissions_{NetPermissionFlags::None};

  // Transport layer (either real TCP or simulated for testing)
  std::shared_ptr<Transport> transport_;

  // IO context (may be external or owned locally)
  std::unique_ptr<boost::asio::io_context> owned_io_context_;  // Only used if no external io_context provided
  boost::asio::io_context& io_context_;  // Reference to either external or owned
  std::unique_ptr<
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
      work_guard_;
  std::vector<std::thread> io_threads_;

  // Components
  std::unique_ptr<AddressManager> addr_manager_;
  std::unique_ptr<PeerManager> peer_manager_;
  validation::ChainstateManager &chainstate_manager_; 
  std::unique_ptr<BanMan> ban_man_;
  std::unique_ptr<NATManager> nat_manager_;
  std::unique_ptr<AnchorManager> anchor_manager_;
  std::unique_ptr<HeaderSyncManager> header_sync_manager_;
  std::unique_ptr<BlockRelayManager> block_relay_manager_;
  std::unique_ptr<MessageRouter> message_router_;

  // Periodic tasks
  std::unique_ptr<boost::asio::steady_timer> connect_timer_;
  std::unique_ptr<boost::asio::steady_timer> maintenance_timer_;
  std::unique_ptr<boost::asio::steady_timer> feeler_timer_;
  std::unique_ptr<boost::asio::steady_timer> sendmessages_timer_;  // Bitcoin-like SendMessages loop
  static constexpr std::chrono::minutes FEELER_INTERVAL{2};
  static constexpr std::chrono::seconds SENDMESSAGES_INTERVAL{1};  // Flush announcements every 1s (Bitcoin pattern)

  // Tip announcement tracking (for periodic re-announcements)
  int64_t last_tip_announcement_time_{
      0}; // Last time we announced (mockable time)

  // Connection management
  void bootstrap_from_fixed_seeds(const chain::ChainParams &params);
  void attempt_outbound_connections();
  void schedule_next_connection_attempt();
  void schedule_next_feeler();

  std::optional<std::string> network_address_to_string(const protocol::NetworkAddress& addr);
  bool already_connected_to_address(const std::string& address, uint16_t port);

  // Inbound connections (handled via transport callback)
  void handle_inbound_connection(TransportConnectionPtr connection);

  // Maintenance
  void run_maintenance();
  void schedule_next_maintenance();

  // Bitcoin-like SendMessages loop (flushes block announcements)
  void run_sendmessages();
  void schedule_next_sendmessages();

  // Initial sync
  void check_initial_sync();

  // Message handling
  void setup_peer_message_handler(Peer *peer);
  bool handle_message(PeerPtr peer, std::unique_ptr<message::Message> msg);

  // Header sync helpers (delegated to HeaderSyncManager)
  void request_headers_from_peer(PeerPtr peer);
  bool handle_headers_message(PeerPtr peer, message::HeadersMessage *msg);
  bool handle_getheaders_message(PeerPtr peer, message::GetHeadersMessage *msg);
  bool handle_inv_message(PeerPtr peer, message::InvMessage *msg);
};

} // namespace network
} // namespace coinbasechain


