#include "network/network_manager.hpp"
#include "network/real_transport.hpp"
#include "network/message.hpp"
#include "primitives/block.h"
#include "validation/chainstate_manager.hpp"
#include "chain/block_manager.hpp"
#include "sync/peer_manager.hpp"
#include "util/logging.hpp"
#include "util/time.hpp"
#include <nlohmann/json.hpp>
#include <cstring>
#include <random>
#include <fstream>
#include <filesystem>

namespace coinbasechain {
namespace network {

// Helper to generate random nonce
static uint64_t generate_nonce() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;
    return dis(gen);
}

NetworkManager::NetworkManager(validation::ChainstateManager& chainstate_manager,
                               const Config& config,
                               std::shared_ptr<Transport> transport)
    : config_(config),
      local_nonce_(generate_nonce()),
      transport_(transport),
      chainstate_manager_(chainstate_manager) {

    // Create transport if not provided (use real TCP transport)
    if (!transport_) {
        transport_ = std::make_shared<RealTransport>(config_.io_threads);
    }

    LOG_NET_INFO("NetworkManager initialized with local nonce: {}", local_nonce_);

    // Create components
    addr_manager_ = std::make_unique<AddressManager>();
    peer_manager_ = std::make_unique<PeerManager>(
        io_context_,
        *addr_manager_
    );

    // Create header sync using the chainstate manager
    header_sync_ = std::make_unique<sync::HeaderSync>(
        chainstate_manager_,
        chain::GlobalChainParams::Get()
    );

    // Create BanMan (with datadir for persistent bans)
    ban_man_ = std::make_unique<sync::BanMan>(config_.datadir);
    if (!config_.datadir.empty()) {
        ban_man_->Load();  // Load existing bans from disk
    }
}

NetworkManager::~NetworkManager() {
    stop();
}

bool NetworkManager::start() {
    if (running_.load(std::memory_order_acquire)) {
        return false;
    }

    running_.store(true, std::memory_order_release);

    // Start transport
    if (transport_) {
        transport_->run();
    }

    // Create work guard to keep io_context running (for timers)
    work_guard_ = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(io_context_)
    );

    // Start IO threads (for timers)
    for (size_t i = 0; i < config_.io_threads; ++i) {
        io_threads_.emplace_back([this]() {
            io_context_.run();
        });
    }

    // Setup timers
    connect_timer_ = std::make_unique<boost::asio::steady_timer>(io_context_);
    maintenance_timer_ = std::make_unique<boost::asio::steady_timer>(io_context_);

    // Start listening if enabled (via transport)
    if (config_.listen_enabled && config_.listen_port > 0) {
        auto self_weak = std::weak_ptr<NetworkManager>(std::shared_ptr<NetworkManager>(this, [](NetworkManager*){}));
        bool success = transport_->listen(
            config_.listen_port,
            [this](TransportConnectionPtr connection) {
                handle_inbound_connection(connection);
            }
        );

        if (success) {
            LOG_NET_INFO("Listening on port {}", config_.listen_port);
        } else {
            LOG_NET_ERROR("Failed to start listener");
            // Continue anyway - we can still make outbound connections
        }
    }

    // Load anchor peers (for eclipse attack resistance)
    // Anchors are the last 2-3 outbound peers we connected to before shutdown
    // We try to reconnect to them first to maintain network view consistency
    if (!config_.datadir.empty()) {
        std::string anchors_path = config_.datadir + "/anchors.dat";
        if (LoadAnchors(anchors_path)) {
            LOG_NET_INFO("Loaded anchors, will connect to them first");
        }
    }

    // Schedule periodic tasks
    schedule_next_connection_attempt();
    schedule_next_maintenance();

    return true;
}

void NetworkManager::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    running_.store(false, std::memory_order_release);

    // Save anchor peers before shutdown (for eclipse attack resistance)
    // This allows us to reconnect to the same peers on next startup
    if (!config_.datadir.empty()) {
        std::string anchors_path = config_.datadir + "/anchors.dat";
        if (SaveAnchors(anchors_path)) {
            LOG_NET_INFO("Saved anchor peers for next startup");
        }
    }

    // Stop transport (stops listening and closes connections)
    if (transport_) {
        transport_->stop();
    }

    // Cancel timers
    if (connect_timer_) {
        connect_timer_->cancel();
    }
    if (maintenance_timer_) {
        maintenance_timer_->cancel();
    }

    // Disconnect all peers
    peer_manager_->disconnect_all();

    // Stop IO threads
    work_guard_.reset();  // Allow io_context to finish

    // Join all threads
    for (auto& thread : io_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    io_threads_.clear();

    // Reset io_context for potential restart
    io_context_.restart();
}

bool NetworkManager::connect_to(const std::string& address, uint16_t port) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    // Check if address is banned
    if (ban_man_ && ban_man_->IsBanned(address)) {
        LOG_NET_INFO("Refusing to connect to banned address: {}", address);
        return false;
    }

    // Check if address is discouraged
    if (ban_man_ && ban_man_->IsDiscouraged(address)) {
        LOG_NET_INFO("Refusing to connect to discouraged address: {}", address);
        return false;
    }

    // Check if we can add more outbound connections
    if (!peer_manager_->needs_more_outbound()) {
        return false;
    }

    // Create transport connection
    auto connection = transport_->connect(
        address,
        port,
        [this, address, port](bool success) {
            if (!success) {
                LOG_NET_DEBUG("Failed to connect to {}:{}", address, port);
            }
        }
    );

    if (!connection) {
        return false;
    }

    // Create outbound peer with connection
    auto peer = Peer::create_outbound(io_context_, connection, config_.network_magic, local_nonce_);
    if (!peer) {
        return false;
    }

    // Setup message handler
    setup_peer_message_handler(peer.get());

    // Start the peer (initiates handshake)
    peer->start();

    // Add to peer manager
    if (!peer_manager_->add_peer(std::move(peer))) {
        return false;
    }

    return true;
}

void NetworkManager::disconnect_from(int peer_id) {
    peer_manager_->remove_peer(peer_id);
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

bool NetworkManager::check_incoming_nonce(uint64_t nonce) const {
    // Check if the nonce matches any of our outbound connections' local nonce
    // This prevents connecting to ourselves
    auto peers = peer_manager_->get_outbound_peers();
    for (const auto& peer : peers) {
        if (peer && !peer->is_connected() && peer->peer_nonce() == nonce) {
            return false;  // Self-connection detected
        }
    }
    return true;  // OK
}

void NetworkManager::attempt_outbound_connections() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    // Check if we need more outbound connections
    while (peer_manager_->needs_more_outbound()) {
        // Select an address from the address manager
        auto maybe_addr = addr_manager_->select();
        if (!maybe_addr) {
            break;  // No addresses available
        }

        auto& addr = *maybe_addr;

        // Convert IP to string
        std::string ip_str;
        // TODO: Proper IP conversion from addr.ip array
        // For now, just skip - this will be implemented when we have real addresses

        // Mark as attempt
        addr_manager_->attempt(addr);

        // Try to connect (this will be properly implemented when IP conversion is done)
        // connect_to(ip_str, addr.port);
    }
}

void NetworkManager::schedule_next_connection_attempt() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    connect_timer_->expires_after(config_.connect_interval);
    connect_timer_->async_wait([this](const boost::system::error_code& ec) {
        if (!ec && running_.load(std::memory_order_acquire)) {
            attempt_outbound_connections();
            schedule_next_connection_attempt();
        }
    });
}

void NetworkManager::handle_inbound_connection(TransportConnectionPtr connection) {
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

    // Check if address is discouraged (probabilistic check)
    if (ban_man_ && ban_man_->IsDiscouraged(remote_address)) {
        LOG_NET_INFO("Rejected discouraged address: {}", remote_address);
        connection->close();
        return;
    }

    // Check if we can accept more inbound connections
    if (!peer_manager_->can_accept_inbound()) {
        // Reject connection - too many inbound peers
        LOG_NET_DEBUG("Rejecting inbound connection from {} (at capacity)", remote_address);
        connection->close();
        return;
    }

    // Create inbound peer with our local nonce
    auto peer = Peer::create_inbound(io_context_, connection, config_.network_magic, local_nonce_);
    if (peer) {
        // Setup message handler
        setup_peer_message_handler(peer.get());

        // Start the peer (waits for VERSION from peer)
        peer->start();

        // Add to peer manager
        peer_manager_->add_peer(std::move(peer));
    }
}

void NetworkManager::run_maintenance() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    // Run periodic cleanup
    peer_manager_->process_periodic();

    // Sweep expired bans
    if (ban_man_) {
        ban_man_->SweepBanned();
    }

    // Periodically announce our tip to peers (matches Bitcoin's SendMessages logic)
    announce_tip_to_peers();

    // Check for sync timeout (if we're syncing)
    uint64_t current_sync_peer = sync_peer_id_.load(std::memory_order_acquire);
    if (current_sync_peer != 0) {
        auto now = std::chrono::steady_clock::now();
        int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        int64_t last_headers_us = last_headers_received_.load(std::memory_order_acquire);

        int64_t time_since_last_headers_sec = (now_us - last_headers_us) / 1000000;

        // Timeout: 60 seconds without receiving headers
        // (Bitcoin uses dynamic timeout based on chain age, we use simple fixed timeout)
        constexpr int64_t SYNC_TIMEOUT_SECONDS = 60;

        if (time_since_last_headers_sec > SYNC_TIMEOUT_SECONDS) {
            LOG_NET_WARN("Header sync timeout! No headers received for {} seconds from peer {}",
                         time_since_last_headers_sec, current_sync_peer);

            // Reset sync peer to try another peer (atomic store)
            sync_peer_id_.store(0, std::memory_order_release);

            // Optionally disconnect stalled peer
            // peer_manager_->remove_peer(current_sync_peer);
        }
    }

    // Check if we need to start initial sync (matches Bitcoin's SendMessages logic)
    check_initial_sync();
}

void NetworkManager::schedule_next_maintenance() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    maintenance_timer_->expires_after(config_.maintenance_interval);
    maintenance_timer_->async_wait([this](const boost::system::error_code& ec) {
        if (!ec && running_.load(std::memory_order_acquire)) {
            run_maintenance();
            schedule_next_maintenance();
        }
    });
}

void NetworkManager::check_initial_sync() {
    // Matches Bitcoin's initial headers sync logic in SendMessages
    // Only actively request headers from a single peer (like Bitcoin's nSyncStarted check)

    if (!running_.load(std::memory_order_acquire) || !header_sync_) {
        return;
    }

    // If we already have a sync peer, nothing to do (atomic load)
    if (sync_peer_id_.load(std::memory_order_acquire) != 0) {
        return;
    }

    // Try outbound peers first (preferred for initial sync )
    auto outbound_peers = peer_manager_->get_outbound_peers();

    for (const auto& peer : outbound_peers) {
        if (!peer || !peer->successfully_connected()) {
            continue;  // Skip peers that haven't completed handshake
        }

        // We found a ready outbound peer! Start initial sync with this peer
        LOG_NET_INFO("Starting initial headers sync with outbound peer {}", peer->id());

        auto now = std::chrono::steady_clock::now();
        int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

        // Atomic stores with proper ordering
        sync_peer_id_.store(peer->id(), std::memory_order_release);
        sync_start_time_.store(now_us, std::memory_order_release);
        last_headers_received_.store(now_us, std::memory_order_release);

        // Send GETHEADERS to initiate sync (like Bitcoin's "initial getheaders")
        request_headers_from_peer(peer);

        return;  // Only sync from one peer at a time
    }

    // Fallback: If no outbound peers available, try inbound peers
    // (Bitcoin allows inbound peers if m_num_preferred_download_peers == 0)
    auto inbound_peers = peer_manager_->get_inbound_peers();

    for (const auto& peer : inbound_peers) {
        if (!peer || !peer->successfully_connected()) {
            continue;  // Skip peers that haven't completed handshake
        }

        // We found a ready inbound peer! Use it as fallback for initial sync
        LOG_NET_INFO("Starting initial headers sync with inbound peer {} (no outbound peers available)", peer->id());

        auto now = std::chrono::steady_clock::now();
        int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

        // Atomic stores with proper ordering
        sync_peer_id_.store(peer->id(), std::memory_order_release);
        sync_start_time_.store(now_us, std::memory_order_release);
        last_headers_received_.store(now_us, std::memory_order_release);

        // Send GETHEADERS to initiate sync
        request_headers_from_peer(peer);

        return;  // Only sync from one peer at a time
    }
}

void NetworkManager::setup_peer_message_handler(Peer* peer) {
    peer->set_message_handler([this](PeerPtr peer, std::unique_ptr<message::Message> msg) {
        return handle_message(peer, std::move(msg));
    });

    // Register peer in HeaderSync's PeerManager for DoS tracking
    if (header_sync_) {
        header_sync_->GetPeerManager().AddPeer(peer->id(), peer->address());
    }
}

bool NetworkManager::handle_message(PeerPtr peer, std::unique_ptr<message::Message> msg) {
    if (!msg || !peer) {
        return false;
    }

    const std::string& command = msg->command();

    // Handle peer ready notification (VERACK received)
    // Peer class handles VERACK internally and also passes it here so we know peer is ready
    if (command == protocol::commands::VERACK) {
        // Peer has completed handshake (VERSION/VERACK exchange)

        if (peer->successfully_connected()) {
            // Announce our tip to this peer (matching Bitcoin Core behavior)
            // This allows peers to discover our chain and request headers if we're ahead
            const chain::CBlockIndex* tip = chainstate_manager_.GetTip();
            if (tip && tip->nHeight > 0) {
                auto inv_msg = std::make_unique<message::InvMessage>();
                protocol::InventoryVector inv;
                inv.type = protocol::InventoryType::MSG_BLOCK;
                uint256 tip_hash = tip->GetBlockHash();
                std::memcpy(inv.hash.data(), tip_hash.data(), 32);
                inv_msg->inventory.push_back(inv);

                LOG_NET_DEBUG("Announcing our tip (height={}) to peer {}", tip->nHeight, peer->id());
                peer->send_message(std::move(inv_msg));
            }

            // Initiate header sync if needed (only if we don't have a sync peer yet)
            // Use atomic compare-and-exchange to avoid race condition
            uint64_t expected = 0;
            if (sync_peer_id_.compare_exchange_strong(expected, peer->id(), std::memory_order_release, std::memory_order_acquire)) {
                LOG_NET_INFO("Starting initial headers sync with peer {}", peer->id());

                auto now = std::chrono::steady_clock::now();
                int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

                sync_start_time_.store(now_us, std::memory_order_release);
                last_headers_received_.store(now_us, std::memory_order_release);

                // Send GETHEADERS to initiate sync (like Bitcoin's "initial getheaders")
                request_headers_from_peer(peer);
            }
        }
        return true;
    }

    // Handle address messages
    if (command == protocol::commands::ADDR) {
        auto* addr_msg = dynamic_cast<message::AddrMessage*>(msg.get());
        if (addr_msg) {
            addr_manager_->add_multiple(addr_msg->addresses);
        }
        return true;
    }

    if (command == protocol::commands::GETADDR) {
        auto response = std::make_unique<message::AddrMessage>();
        response->addresses = addr_manager_->get_addresses(1000);
        peer->send_message(std::move(response));
        return true;
    }

    // Handle inventory messages
    if (command == protocol::commands::INV) {
        auto* inv_msg = dynamic_cast<message::InvMessage*>(msg.get());
        if (inv_msg) {
            return handle_inv_message(peer, inv_msg);
        }
        return false;
    }

    // Handle header sync messages
    if (command == protocol::commands::HEADERS) {
        auto* headers_msg = dynamic_cast<message::HeadersMessage*>(msg.get());
        if (headers_msg) {
            return handle_headers_message(peer, headers_msg);
        }
        return false;
    }

    if (command == protocol::commands::GETHEADERS) {
        auto* getheaders_msg = dynamic_cast<message::GetHeadersMessage*>(msg.get());
        if (getheaders_msg) {
            return handle_getheaders_message(peer, getheaders_msg);
        }
        return false;
    }

    return true;
}

void NetworkManager::request_headers_from_peer(PeerPtr peer) {
    if (!peer || !header_sync_) {
        return;
    }

    // Get block locator from header sync
    // For initial sync, use pprev trick 
    // This ensures we get non-empty response even if peer is at same tip
    CBlockLocator locator = header_sync_->GetLocatorFromPrev();

    // Create GETHEADERS message
    auto msg = std::make_unique<message::GetHeadersMessage>();
    msg->version = protocol::PROTOCOL_VERSION;

    // Convert locator hashes from uint256 to std::array<uint8_t, 32>
    for (const auto& hash : locator.vHave) {
        std::array<uint8_t, 32> hash_array;
        std::memcpy(hash_array.data(), hash.data(), 32);
        msg->block_locator_hashes.push_back(hash_array);
    }

    // hash_stop is all zeros (get as many as possible)
    msg->hash_stop.fill(0);

    LOG_NET_DEBUG("Requesting headers from peer {} (locator size: {})",
                  peer->id(), msg->block_locator_hashes.size());

    peer->send_message(std::move(msg));
}

bool NetworkManager::handle_headers_message(PeerPtr peer, message::HeadersMessage* msg) {
    if (!peer || !msg || !header_sync_) {
        return false;
    }

    LOG_NET_INFO("Received {} headers from peer {}", msg->headers.size(), peer->id());

    // Update last headers received timestamp (atomic store)
    auto now = std::chrono::steady_clock::now();
    int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    last_headers_received_.store(now_us, std::memory_order_release);

    // Process headers through header sync
    bool success = header_sync_->ProcessHeaders(msg->headers, peer->id());


    if (!success) {
        LOG_NET_ERROR("Failed to process headers from peer {}", peer->id());
        // Reset sync peer so we can try another peer (atomic store)
        sync_peer_id_.store(0, std::memory_order_release);

        // Punish peer: Check if HeaderSync's PeerManager marked them for disconnect
        auto& peer_sync_manager = header_sync_->GetPeerManager();
        if (peer_sync_manager.ShouldDisconnect(peer->id())) {
            // Peer misbehaved - discourage their address
            if (ban_man_) {
                ban_man_->Discourage(peer->address());
            }
            // Disconnect the peer
            peer_manager_->remove_peer(peer->id());
        }

        return false;
    }

    // Check if we should request more headers
    bool should_request = header_sync_->ShouldRequestMore();
    bool is_synced = header_sync_->IsSynced();

    if (should_request) {
        request_headers_from_peer(peer);
    } else if (is_synced) {
        // Reset sync peer - we're done syncing (atomic store)
        sync_peer_id_.store(0, std::memory_order_release);
    }

    return true;
}

bool NetworkManager::handle_getheaders_message(PeerPtr peer, message::GetHeadersMessage* msg) {
    if (!peer || !msg) {
        return false;
    }

    LOG_NET_DEBUG("Peer {} requested headers (locator size: {})",
                  peer->id(), msg->block_locator_hashes.size());

    // Find the fork point using the block locator
    // IMPORTANT: Only consider blocks on the ACTIVE chain, not side chains
    // Otherwise we might find a fork point on a side chain we know about
    const chain::CBlockIndex* fork_point = nullptr;
    for (const auto& hash_array : msg->block_locator_hashes) {
        // Convert std::array<uint8_t, 32> to uint256
        uint256 hash;
        std::memcpy(hash.data(), hash_array.data(), 32);

        const chain::CBlockIndex* pindex = chainstate_manager_.LookupBlockIndex(hash);
        if (chainstate_manager_.IsOnActiveChain(pindex)) {
            // Found a block that exists AND is on our active chain
            fork_point = pindex;
            LOG_NET_INFO("Found fork point at height {} (hash={}) on active chain", fork_point->nHeight, hash.ToString().substr(0, 16));
            break;
        }
    }

    if (!fork_point) {
        // No common blocks - start from genesis
        fork_point = chainstate_manager_.GetTip();
        while (fork_point && fork_point->pprev) {
            fork_point = fork_point->pprev;
        }
        if (fork_point) {
            LOG_NET_INFO("No common blocks in locator - using genesis at height {}", fork_point->nHeight);
        }
    }

    if (!fork_point) {
        LOG_NET_WARN("No blocks to send to peer {}", peer->id());
        return false;
    }

    const chain::CBlockIndex* tip = chainstate_manager_.GetTip();
    LOG_NET_INFO("Preparing headers: fork_point height={}, tip height={}", fork_point->nHeight, tip ? tip->nHeight : -1);

    // Build HEADERS response
    auto response = std::make_unique<message::HeadersMessage>();

    // Start from the block after fork point and collect headers
    const chain::CBlockIndex* pindex = chainstate_manager_.GetBlockAtHeight(fork_point->nHeight + 1);

    while (pindex && response->headers.size() < protocol::MAX_HEADERS_SIZE) {
        CBlockHeader hdr = pindex->GetBlockHeader();
        response->headers.push_back(hdr);

        if (pindex == tip) {
            break;
        }

        // Get next block in active chain
        pindex = chainstate_manager_.GetBlockAtHeight(pindex->nHeight + 1);
    }

    LOG_NET_INFO("Sending {} headers to peer {} (from height {} to {})",
                  response->headers.size(), peer->id(), fork_point->nHeight + 1,
                  fork_point->nHeight + response->headers.size());

    peer->send_message(std::move(response));
    return true;
}

// Anchors implementation for eclipse attack resistance
std::vector<protocol::NetworkAddress> NetworkManager::GetAnchors() const {
    std::vector<protocol::NetworkAddress> anchors;

    if (!peer_manager_) {
        return anchors;
    }

    // Get all connected outbound peers
    auto outbound_peers = peer_manager_->get_outbound_peers();

    // Filter for connected peers only
    std::vector<PeerPtr> connected_peers;
    for (const auto& peer : outbound_peers) {
        if (peer && peer->is_connected() && peer->state() == PeerState::READY) {
            connected_peers.push_back(peer);
        }
    }

    // Limit to 2-3 anchors (Bitcoin Core uses 2)
    const size_t MAX_ANCHORS = 2;
    size_t count = std::min(connected_peers.size(), MAX_ANCHORS);

    // Convert peer information to NetworkAddress
    for (size_t i = 0; i < count; ++i) {
        const auto& peer = connected_peers[i];

        protocol::NetworkAddress addr;
        addr.services = peer->services();
        addr.port = peer->port();

        // Convert IP address string to bytes using boost::asio
        std::string ip_str = peer->address();
        try {
            boost::system::error_code ec;
            auto ip_addr = boost::asio::ip::make_address(ip_str, ec);

            if (ec) {
                LOG_NET_WARN("Failed to parse IP address '{}': {}", ip_str, ec.message());
                continue;
            }

            // Convert to 16-byte format (IPv4-mapped if needed)
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

            anchors.push_back(addr);
        } catch (const std::exception& e) {
            LOG_NET_WARN("Exception parsing IP address '{}': {}", ip_str, e.what());
            continue;
        }
    }

    LOG_NET_INFO("Selected {} anchor peers", anchors.size());
    return anchors;
}

bool NetworkManager::SaveAnchors(const std::string& filepath) {
    using json = nlohmann::json;

    try {
        auto anchors = GetAnchors();

        if (anchors.empty()) {
            LOG_NET_DEBUG("No anchors to save");
            return true;  // Not an error
        }

        LOG_NET_INFO("Saving {} anchor addresses to {}", anchors.size(), filepath);

        json root;
        root["version"] = 1;
        root["count"] = anchors.size();

        json anchors_array = json::array();
        for (const auto& addr : anchors) {
            json anchor;
            anchor["ip"] = json::array();
            for (size_t i = 0; i < 16; ++i) {
                anchor["ip"].push_back(addr.ip[i]);
            }
            anchor["port"] = addr.port;
            anchor["services"] = addr.services;
            anchors_array.push_back(anchor);
        }
        root["anchors"] = anchors_array;

        // Write to file
        std::ofstream file(filepath);
        if (!file.is_open()) {
            LOG_NET_ERROR("Failed to open anchors file for writing: {}", filepath);
            return false;
        }

        file << root.dump(2);
        file.close();

        LOG_NET_DEBUG("Successfully saved {} anchors", anchors.size());
        return true;

    } catch (const std::exception& e) {
        LOG_NET_ERROR("Exception during SaveAnchors: {}", e.what());
        return false;
    }
}

bool NetworkManager::LoadAnchors(const std::string& filepath) {
    using json = nlohmann::json;

    try {
        // Check if file exists
        std::ifstream file(filepath);
        if (!file.is_open()) {
            LOG_NET_DEBUG("No anchors file found at {}", filepath);
            return true;  // Not an error - first run
        }

        // Parse JSON
        json root;
        file >> root;
        file.close();

        // Validate version
        if (root["version"] != 1) {
            LOG_NET_WARN("Unsupported anchors file version: {}", root["version"].get<int>());

            // Delete the file since it's incompatible
            std::filesystem::remove(filepath);
            return false;
        }

        // Load anchors
        json anchors_array = root["anchors"];
        std::vector<protocol::NetworkAddress> anchors;

        for (const auto& anchor_json : anchors_array) {
            protocol::NetworkAddress addr;

            json ip_array = anchor_json["ip"];
            for (size_t i = 0; i < 16 && i < ip_array.size(); ++i) {
                addr.ip[i] = ip_array[i].get<uint8_t>();
            }

            addr.port = anchor_json["port"].get<uint16_t>();
            addr.services = anchor_json["services"].get<uint64_t>();

            anchors.push_back(addr);
        }

        LOG_NET_INFO("Loaded {} anchor addresses from {}", anchors.size(), filepath);

        // Try to reconnect to anchors
        for (const auto& addr : anchors) {
            try {
                // Convert 16-byte array to boost::asio IP address
                boost::asio::ip::address_v6::bytes_type bytes;
                std::copy(addr.ip.begin(), addr.ip.end(), bytes.begin());
                auto v6_addr = boost::asio::ip::make_address_v6(bytes);

                // Check if it's IPv4-mapped and convert if needed
                std::string ip_str;
                if (v6_addr.is_v4_mapped()) {
                    ip_str = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6_addr).to_string();
                } else {
                    ip_str = v6_addr.to_string();
                }

                LOG_NET_INFO("Reconnecting to anchor: {}:{}", ip_str, addr.port);

                // Attempt to connect (this will be async so we don't block startup)
                connect_to(ip_str, addr.port);

            } catch (const std::exception& e) {
                LOG_NET_WARN("Failed to reconnect to anchor: {}", e.what());
                continue;
            }
        }

        // Delete the anchors file after reading (Bitcoin Core behavior)
        // This prevents using stale anchors after a crash
        std::filesystem::remove(filepath);
        LOG_NET_DEBUG("Deleted anchors file after reading");

        return true;

    } catch (const std::exception& e) {
        LOG_NET_ERROR("Exception during LoadAnchors: {}", e.what());

        // Try to delete the file if it's corrupted
        try {
            std::filesystem::remove(filepath);
        } catch (...) {
            // Ignore
        }

        return false;
    }
}

bool NetworkManager::handle_inv_message(PeerPtr peer, message::InvMessage* msg) {
    if (!peer || !msg) {
        return false;
    }

    LOG_NET_DEBUG("Received INV with {} items from peer {}", msg->inventory.size(), peer->id());

    // Process each inventory item
    for (const auto& inv : msg->inventory) {
        if (inv.type == protocol::InventoryType::MSG_BLOCK) {
            // Convert array to uint256
            uint256 block_hash;
            std::memcpy(block_hash.data(), inv.hash.data(), 32);

            LOG_NET_INFO("Peer {} announced block: {}", peer->id(), block_hash.GetHex());

            // Check if we already have this block
            const chain::CBlockIndex* pindex = chainstate_manager_.LookupBlockIndex(block_hash);
            if (pindex) {
                LOG_NET_DEBUG("Already have block {}", block_hash.GetHex());
                continue;
            }

            // Request headers to get this new block
            // Since this is headers-only, we request the header via GETHEADERS
            request_headers_from_peer(peer);
        }
    }

    return true;
}

void NetworkManager::announce_tip_to_peers() {
    // Matches Bitcoin Core's SendMessages logic for periodic tip announcements
    // Bitcoin re-announces periodically even if tip hasn't changed (every ~30 seconds)

    const chain::CBlockIndex* tip = chainstate_manager_.GetTip();
    if (!tip || tip->nHeight == 0) {
        return;  // No tip to announce
    }

    uint256 current_tip_hash = tip->GetBlockHash();
    bool tip_changed = (current_tip_hash != last_announced_tip_);

    // Get current time (mockable for tests)
    int64_t now = util::GetTime();

    // Bitcoin's re-announcement interval (30 seconds)
    constexpr int64_t ANNOUNCE_INTERVAL_SECONDS = 30;

    // Check if we should announce:
    // 1. Tip has changed, OR
    // 2. Enough time has passed since last announcement (periodic re-announcement)
    bool should_announce = tip_changed ||
                          (now - last_tip_announcement_time_ >= ANNOUNCE_INTERVAL_SECONDS);

    if (!should_announce) {
        return;  // Too soon to re-announce
    }

    LOG_NET_DEBUG("Announcing tip to peers (height={}, hash={})",
                  tip->nHeight, current_tip_hash.GetHex().substr(0, 16));

    // Update tracking
    last_announced_tip_ = current_tip_hash;
    last_tip_announcement_time_ = now;

    // Create INV message with current tip
    auto inv_msg = std::make_unique<message::InvMessage>();
    protocol::InventoryVector inv;
    inv.type = protocol::InventoryType::MSG_BLOCK;
    std::memcpy(inv.hash.data(), current_tip_hash.data(), 32);
    inv_msg->inventory.push_back(inv);

    // Send to all ready peers
    auto all_peers = peer_manager_->get_all_peers();
    int sent_count = 0;
    for (const auto& peer : all_peers) {
        if (peer && peer->is_connected() && peer->state() == PeerState::READY) {
            // Clone the message for each peer
            auto msg_copy = std::make_unique<message::InvMessage>();
            msg_copy->inventory = inv_msg->inventory;
            peer->send_message(std::move(msg_copy));
            sent_count++;
        }
    }
}

void NetworkManager::relay_block(const uint256& block_hash) {
    // Create INV message with the new block
    auto inv_msg = std::make_unique<message::InvMessage>();

    protocol::InventoryVector inv;
    inv.type = protocol::InventoryType::MSG_BLOCK;
    std::memcpy(inv.hash.data(), block_hash.data(), 32);

    inv_msg->inventory.push_back(inv);

    LOG_NET_INFO("Relaying block {} to {} peers", block_hash.GetHex(), peer_manager_->peer_count());

    // Send to all connected peers
    auto all_peers = peer_manager_->get_all_peers();
    int ready_count = 0;
    int sent_count = 0;
    for (const auto& peer : all_peers) {
        if (peer && peer->is_connected()) {
            if (peer->state() == PeerState::READY) {
                ready_count++;
                // Clone the message for each peer
                auto msg_copy = std::make_unique<message::InvMessage>();
                msg_copy->inventory = inv_msg->inventory;
                peer->send_message(std::move(msg_copy));
                sent_count++;
            }
        }
    }
}

} // namespace network
} // namespace coinbasechain
