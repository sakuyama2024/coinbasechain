// Copyright (c) 2024 Coinbase Chain
// SimulatedNode implementation - Uses REAL P2P components with simulated transport

#include "simulated_node.hpp"
#include "chain/sha256.hpp"
#include "chain/block.hpp"
#include "chain/chainstate_manager.hpp"
#include "chain/logging.hpp"
#include "chain/pow.hpp"
#include "chain/randomx_pow.hpp"
#include <sstream>
#include <random>

namespace coinbasechain {
namespace test {

using coinbasechain::validation::ValidationState;

SimulatedNode::SimulatedNode(int node_id,
                             SimulatedNetwork* network,
                             const chain::ChainParams* params)
    : node_id_(node_id)
    , port_(protocol::ports::REGTEST + node_id)
    , sim_network_(network)
{
    // Generate node address
    std::ostringstream oss;
    oss << "127.0.0." << (node_id % 255);
    address_ = oss.str();

    // Setup chain params
    if (params) {
        params_ = params;
    } else {
        params_owned_ = chain::ChainParams::CreateRegTest();
        params_ = params_owned_.get();
    }

    // Initialize chainstate with genesis
    chainstate_ = std::make_unique<TestChainstateManager>(*params_);
    chainstate_->Initialize(params_->GenesisBlock());

    // Create work guard to keep io_context alive
    work_guard_ = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(io_context_)
    );

    // Initialize networking
    InitializeNetworking();

    // Re-register with SimulatedNetwork to include node pointer for event processing and transport
    if (sim_network_ && transport_) {
        sim_network_->RegisterNode(node_id_, [this](int from_node_id, const std::vector<uint8_t>& data) {
            if (transport_) {
                transport_->deliver_message(from_node_id, data);
            }
        }, this, transport_.get());
    }
}

SimulatedNode::~SimulatedNode() {
    // Stop networking
    if (network_manager_) {
        network_manager_->stop();
    }

    // Release work guard to allow io_context to finish
    work_guard_.reset();

    // Process remaining events
    io_context_.run();
}

void SimulatedNode::InitializeNetworking() {
    // Create bridged transport that routes through SimulatedNetwork
    transport_ = std::make_shared<NetworkBridgedTransport>(node_id_, sim_network_);

    // Create NetworkManager with our transport
    network::NetworkManager::Config config;
    config.network_magic = params_->GetNetworkMagic();
    config.listen_enabled = true;
    config.listen_port = port_;
    config.io_threads = 0;  // Use external io_context
    config.enable_nat = false;  // Disable NAT/UPnP in tests (would block trying to discover devices)

    network_manager_ = std::make_unique<network::NetworkManager>(
        *chainstate_,  // Pass TestChainstateManager (inherits from ChainstateManager)
        config,
        transport_,
        &io_context_  // Pass our io_context so boost::asio::post() uses it
    );

    // Start networking
    if (!network_manager_->start()) {
        throw std::runtime_error("Failed to start NetworkManager");
    }
}

std::string SimulatedNode::GetAddress() const {
    return address_;
}

bool SimulatedNode::ConnectTo(int peer_node_id, const std::string& address, uint16_t port) {
    // Prevent self-connection
    if (peer_node_id == node_id_) {
        return false;
    }

    // Generate peer address
    std::string peer_addr = address;
    if (peer_addr.empty()) {
        std::ostringstream oss;
        oss << "127.0.0." << (peer_node_id % 255);
        peer_addr = oss.str();
    }

    // Construct NetworkAddress for new API
    protocol::NetworkAddress net_addr;
    net_addr.services = protocol::ServiceFlags::NODE_NETWORK;
    net_addr.port = port;

    // Convert IP string to bytes (IPv4-mapped IPv6)
    boost::system::error_code ec;
    auto ip_addr = boost::asio::ip::make_address(peer_addr, ec);

    if (ec) {
      LOG_ERROR("SimulatedNode::ConnectToPeer: Invalid IP address: {}", peer_addr);
      return false;
    }

    if (ip_addr.is_v4()) {
      auto v6_mapped = boost::asio::ip::make_address_v6(
          boost::asio::ip::v4_mapped, ip_addr.to_v4());
      auto bytes = v6_mapped.to_bytes();
      std::copy(bytes.begin(), bytes.end(), net_addr.ip.begin());
    } else {
      auto bytes = ip_addr.to_v6().to_bytes();
      std::copy(bytes.begin(), bytes.end(), net_addr.ip.begin());
    }

    // Use real NetworkManager to connect
    bool success = network_manager_->connect_to(net_addr);
    if (success) {
        stats_.connections_made++;
    }

    // Process events to ensure connection is initiated
    // This is important in fast (non-TSAN) builds where async operations complete quickly
    ProcessEvents();

    return success;
}

void SimulatedNode::DisconnectFrom(int peer_node_id) {
    // Convert node_id to IP address, then find and disconnect the peer
    if (!network_manager_) {
        return;
    }

    // Generate the peer's address (same logic as ConnectTo)
    std::ostringstream oss;
    oss << "127.0.0." << (peer_node_id % 255);
    std::string peer_addr = oss.str();

    // Get peer ID by address (this returns the PeerManager map key)
    auto& peer_mgr = network_manager_->peer_manager();

    // Search all peers to find one matching this address
    // We can't use find_peer_by_address() because:
    // - For outbound peers: target_port = protocol::ports::REGTEST
    // - For inbound peers: target_port = ephemeral source port (unknown)
    // Since each node has a unique IP (127.0.0.X), search by address only
    int peer_manager_id = -1;
    auto all_peers = peer_mgr.get_all_peers();
    for (const auto& peer : all_peers) {
        if (peer && peer->target_address() == peer_addr) {
            peer_manager_id = peer->id();
            break;
        }
    }

    if (peer_manager_id >= 0) {
        network_manager_->disconnect_from(peer_manager_id);
        stats_.disconnections++;

        // Process events to ensure disconnect is processed locally
        ProcessEvents();

        // NOTE: The remote node won't know about the disconnect until it processes
        // the connection close event. The test should call AdvanceTime() and ProcessEvents()
        // on the remote node after calling DisconnectFrom().
    } else {
    }
}

uint256 SimulatedNode::MineBlock(const std::string& miner_address) {
    // Create block header
    CBlockHeader header;
    header.nVersion = 1;
    header.hashPrevBlock = GetTipHash();
    header.nTime = static_cast<uint32_t>(sim_network_->GetCurrentTime() / 1000);
    header.nBits = params_->GenesisBlock().nBits;

    // Random nonce
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint32_t> dis_nonce;
    header.nNonce = dis_nonce(gen);

    // Random miner address
    std::uniform_int_distribution<uint8_t> dis_byte(0, 255);
    for (int i = 0; i < 20; i++) {
        header.minerAddress.data()[i] = dis_byte(gen);
    }

    // Set dummy RandomX hash (PoW bypass enabled by default)
    header.hashRandomX.SetHex("0000000000000000000000000000000000000000000000000000000000000000");

    // Add to chainstate
    validation::ValidationState state;
    auto* pindex = chainstate_->AcceptBlockHeader(header, state, node_id_);

    if (pindex) {
        chainstate_->TryAddBlockIndexCandidate(pindex);
        chainstate_->ActivateBestChain();
        stats_.blocks_mined++;

        // Broadcast the block to peers via NetworkManager
        uint256 block_hash = header.GetHash();
        if (network_manager_) {
            size_t peer_count = network_manager_->active_peer_count();
            network_manager_->relay_block(block_hash);
        }

        // Process events to ensure the block relay messages are queued
        // This is important in fast (non-TSAN) builds where async operations complete quickly
        ProcessEvents();

        return block_hash;
    }

    return uint256();  // Failed
}

int SimulatedNode::GetTipHeight() const {
    const auto* tip = chainstate_->GetTip();
    return tip ? tip->nHeight : 0;
}

uint256 SimulatedNode::GetTipHash() const {
    const auto* tip = chainstate_->GetTip();
    return tip ? tip->GetBlockHash() : params_->GenesisBlock().GetHash();
}

const chain::CBlockIndex* SimulatedNode::GetTip() const {
    return chainstate_->GetTip();
}

bool SimulatedNode::GetIsIBD() const {
    return chainstate_->IsInitialBlockDownload();
}

size_t SimulatedNode::GetPeerCount() const {
    if (network_manager_) {
        return network_manager_->active_peer_count();
    }
    return 0;
}

size_t SimulatedNode::GetOutboundPeerCount() const {
    if (network_manager_) {
        return network_manager_->outbound_peer_count();
    }
    return 0;
}

size_t SimulatedNode::GetInboundPeerCount() const {
    if (network_manager_) {
        return network_manager_->inbound_peer_count();
    }
    return 0;
}

bool SimulatedNode::IsBanned(const std::string& address) const {
    if (network_manager_) {
        return const_cast<network::NetworkManager*>(network_manager_.get())->ban_man().IsBanned(address);
    }
    return false;
}

void SimulatedNode::Ban(const std::string& address, int64_t ban_time_seconds) {
    if (network_manager_) {
        network_manager_->ban_man().Ban(address, ban_time_seconds);
    }
}

void SimulatedNode::Unban(const std::string& address) {
    if (network_manager_) {
        network_manager_->ban_man().Unban(address);
    }
}

network::BanMan& SimulatedNode::GetBanMan() {
    if (!network_manager_) {
        throw std::runtime_error("NetworkManager not initialized");
    }
    return network_manager_->ban_man();
}

void SimulatedNode::ProcessEvents() {
    // Process pending async operations
    // poll() runs all ready handlers, which may post new work
    // Keep polling until no more work is immediately ready
    while (true) {
        size_t executed = io_context_.poll();
        if (executed == 0) {
            break;
        }
    }

    // Flush pending block announcements after processing events
    // This ensures announcements are sent regardless of how tests trigger event processing
    // (matches Bitcoin's SendMessages loop which flushes after processing events)
    if (network_manager_) {
        network_manager_->flush_block_announcements();
    }
}

void SimulatedNode::ProcessPeriodic() {
    // Run periodic maintenance tasks
    // In a real node, these run on timers, but in simulation they're triggered by AdvanceTime()
    if (network_manager_) {
        network_manager_->peer_manager().process_periodic();
        // Call announce_tip_to_peers() to add blocks to announcement queues
        // The actual flushing happens in ProcessEvents() (like Bitcoin's SendMessages loop)
        network_manager_->announce_tip_to_peers();
    }
}

} // namespace test
} // namespace coinbasechain
