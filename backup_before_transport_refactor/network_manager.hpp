#ifndef COINBASECHAIN_NETWORK_MANAGER_HPP
#define COINBASECHAIN_NETWORK_MANAGER_HPP

#include "network/peer_manager.hpp"
#include "network/addr_manager.hpp"
#include "sync/header_sync.hpp"
#include "sync/banman.hpp"
#include "chain/chainparams.hpp"
#include <boost/asio.hpp>
#include <thread>
#include <atomic>
#include <chrono>

namespace coinbasechain {

// Forward declarations
namespace validation {
    class ChainstateManager;
}

namespace network {

/**
 * NetworkManager - Top-level coordinator for all networking
 *
 * Simplified version inspired by Bitcoins's CConnman:
 * - Manages the io_context thread pool
 * - Coordinates PeerManager and AddressManager
 * - Handles outbound connection attempts
 * - Accepts inbound connections (if listening)
 * - Routes messages between components
 * - Periodic maintenance tasks
 *
 * This is much simpler than Bitcoins's CConnman which has complex
 * thread management, bandwidth limiting, whitelisting, etc.
 */
class NetworkManager {
public:
    struct Config {
        uint32_t network_magic;             // Network magic bytes
        uint16_t listen_port;               // Port to listen on (0 = don't listen)
        bool listen_enabled;                // Enable inbound connections
        size_t io_threads;                  // Number of IO threads
        int par_threads;                     // Number of parallel RandomX verification threads (0 = auto)
        std::string datadir;                 // Data directory (for banlist.json)

        std::chrono::seconds connect_interval;  // Time between connection attempts
        std::chrono::seconds maintenance_interval;  // Time between maintenance tasks

        Config()
            : network_magic(protocol::magic::MAINNET)  // Mainnet by default
            , listen_port(protocol::ports::MAINNET)
            , listen_enabled(false)
            , io_threads(4)
            , par_threads(0)                 // Auto-detect by default
            , datadir("")                    // Empty = no persistent bans
            , connect_interval(std::chrono::seconds(5))
            , maintenance_interval(std::chrono::seconds(30))
        {}
    };

    NetworkManager(validation::ChainstateManager& chainstate_manager, const Config& config = Config{});
    ~NetworkManager();

    // Lifecycle
    bool start();
    void stop();
    bool is_running() const { return running_; }

    // Component access
    PeerManager& peer_manager() { return *peer_manager_; }
    AddressManager& address_manager() { return *addr_manager_; }
    sync::HeaderSync& header_sync() { return *header_sync_; }
    sync::BanMan& ban_man() { return *ban_man_; }

    // Manual connection management
    bool connect_to(const std::string& address, uint16_t port);
    void disconnect_from(int peer_id);

    // Block relay
    void relay_block(const uint256& block_hash);

    // Self-connection prevention
    uint64_t get_local_nonce() const { return local_nonce_; }
    bool check_incoming_nonce(uint64_t nonce) const;

    // Stats
    size_t active_peer_count() const;
    size_t outbound_peer_count() const;
    size_t inbound_peer_count() const;

    // Anchors (for eclipse attack resistance)
    std::vector<protocol::NetworkAddress> GetAnchors() const;
    bool SaveAnchors(const std::string& filepath);
    bool LoadAnchors(const std::string& filepath);

private:
    Config config_;
    std::atomic<bool> running_{false};

    // Self-connection prevention: unique nonce for this node
    uint64_t local_nonce_;

    // IO context and threads
    boost::asio::io_context io_context_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard_;
    std::vector<std::thread> io_threads_;

    // Components
    std::unique_ptr<AddressManager> addr_manager_;
    std::unique_ptr<PeerManager> peer_manager_;
    validation::ChainstateManager& chainstate_manager_;  // Reference to Application's ChainstateManager
    std::unique_ptr<sync::HeaderSync> header_sync_;
    std::unique_ptr<sync::BanMan> ban_man_;

    // Inbound acceptor (if listening)
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;

    // Periodic tasks
    std::unique_ptr<boost::asio::steady_timer> connect_timer_;
    std::unique_ptr<boost::asio::steady_timer> maintenance_timer_;

    // Initial sync tracking (matches Bitcoin's nSyncStarted)
    // Thread-safe: accessed from multiple io_context threads
    std::atomic<uint64_t> sync_peer_id_{0};  // 0 = no sync peer, otherwise peer ID we're syncing from
    std::atomic<int64_t> sync_start_time_{0};  // When did sync start? (microseconds since epoch)
    std::atomic<int64_t> last_headers_received_{0};  // Last time we received headers (microseconds since epoch)

    // Connection management
    void attempt_outbound_connections();
    void schedule_next_connection_attempt();

    // Inbound connections
    void start_accepting();
    void handle_accept(const boost::system::error_code& ec,
                      boost::asio::ip::tcp::socket socket);

    // Maintenance
    void run_maintenance();
    void schedule_next_maintenance();

    // Initial sync (matches Bitcoin's initial getheaders logic in SendMessages)
    void check_initial_sync();

    // Message handling
    void setup_peer_message_handler(Peer* peer);
    bool handle_message(PeerPtr peer, std::unique_ptr<message::Message> msg);

    // Header sync helpers
    void request_headers_from_peer(PeerPtr peer);
    bool handle_headers_message(PeerPtr peer, message::HeadersMessage* msg);
    bool handle_getheaders_message(PeerPtr peer, message::GetHeadersMessage* msg);

    // Block relay helpers
    bool handle_inv_message(PeerPtr peer, message::InvMessage* msg);
};

} // namespace network
} // namespace coinbasechain

#endif // COINBASECHAIN_NETWORK_MANAGER_HPP
