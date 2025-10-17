// Copyright (c) 2024 Coinbase Chain
// Simulated node implementation for P2P network testing

#include "simulated_node.hpp"
#include "crypto/sha256.h"
#include "primitives/block.h"
#include "validation/chainstate_manager.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <random>

namespace coinbasechain {
namespace test {

using coinbasechain::validation::ValidationState;

SimulatedNode::SimulatedNode(int node_id,
                             SimulatedNetwork* network,
                             const chain::ChainParams* params)
    : node_id_(node_id),
      address_("127.0.0." + std::to_string(node_id % 255)),
      port_(8333 + node_id),
      sim_network_(network)
{
    // Generate random nonce
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    nonce_ = dis(gen);

    // Setup chain params
    if (params) {
        params_ = params;
    } else {
        params_owned_ = chain::ChainParams::CreateRegTest();
        params_ = params_owned_.get();
    }

    // Initialize TestChainstateManager with genesis block
    chainstate_ = std::make_unique<TestChainstateManager>(*params_);
    chainstate_->Initialize(params_->GenesisBlock());

    // Initialize BanMan (no persistence in tests)
    ban_man_ = std::make_unique<sync::BanMan>();

    // Initialize NetworkManager
    // NOTE: This is simplified - in real implementation we'd need to:
    // 1. Create a simulated socket abstraction
    // 2. Pass it to NetworkManager instead of boost::asio::io_context
    // 3. Hook up message callbacks
    //
    // For now, we'll stub this out and implement the core functionality
    // directly in SimulatedNode

    SetupMessageHandlers();
}

SimulatedNode::~SimulatedNode() {
    // Cleanup
}

bool SimulatedNode::ConnectTo(int peer_node_id, const std::string& address, uint16_t port) {
    // Prevent self-connection
    if (peer_node_id == node_id_) {
        return false;
    }

    // Check if already connected
    for (const auto& peer : peers_) {
        if (peer.node_id == peer_node_id) {
            return false;  // Already connected
        }
    }

    // Check ban list
    std::string peer_addr = "127.0.0." + std::to_string(peer_node_id % 255);
    if (ban_man_->IsBanned(peer_addr)) {
        return false;  // Banned
    }

    // Check connection limits
    size_t outbound_count = GetOutboundPeerCount();
    const size_t MAX_OUTBOUND = 8;
    if (outbound_count >= MAX_OUTBOUND) {
        return false;  // At capacity
    }

    // Create peer connection
    PeerConnection peer;
    peer.node_id = peer_node_id;
    peer.address = peer_addr;
    peer.port = port;
    peer.is_outbound = true;
    peer.connected_time = sim_network_->GetCurrentTime();

    peers_.push_back(peer);
    stats_.connections_made++;

    // Send VERSION message to initiate handshake
    SendVersionMessage(peer_node_id);

    return true;
}

void SimulatedNode::DisconnectFrom(int peer_id) {
    auto it = std::find_if(peers_.begin(), peers_.end(),
        [peer_id](const PeerConnection& p) { return p.node_id == peer_id; });

    if (it != peers_.end()) {
        // Send disconnect message with our node_id
        std::vector<uint8_t> disconnect_msg = {'D', 'I', 'S', 'C'};
        disconnect_msg.push_back((node_id_ >> 24) & 0xFF);
        disconnect_msg.push_back((node_id_ >> 16) & 0xFF);
        disconnect_msg.push_back((node_id_ >> 8) & 0xFF);
        disconnect_msg.push_back(node_id_ & 0xFF);

        sim_network_->SendMessage(node_id_, peer_id, disconnect_msg);

        peers_.erase(it);
        stats_.disconnections++;
    }
}

uint256 SimulatedNode::MineBlock(const std::string& miner_address) {
    // Create a new block header
    CBlockHeader header;
    header.nVersion = 1;
    header.hashPrevBlock = GetTipHash();
    header.nTime = static_cast<uint32_t>(sim_network_->GetCurrentTime() / 1000);
    header.nBits = params_->GenesisBlock().nBits;  // Use genesis difficulty

    // Generate random nonce
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint32_t> dis_nonce;
    header.nNonce = dis_nonce(gen);

    // Set miner address (simplified - just use random address for testing)
    std::uniform_int_distribution<uint8_t> dis_byte(0, 255);
    for (int i = 0; i < 20; i++) {
        header.minerAddress.data()[i] = dis_byte(gen);
    }

    header.hashRandomX.SetNull();  // Test bypasses PoW

    // Add to chainstate using real API
    validation::ValidationState state;
    auto* pindex = chainstate_->AcceptBlockHeader(header, state, node_id_);

    if (pindex) {
        chainstate_->TryAddBlockIndexCandidate(pindex);
        chainstate_->ActivateBestChain();

        stats_.blocks_mined++;

        uint256 block_hash = header.GetHash();

        // Mark as known (we mined it)
        known_blocks_.insert(block_hash);

        // Broadcast to all peers
        BroadcastBlock(header);

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

size_t SimulatedNode::GetPeerCount() const {
    return peers_.size();
}

size_t SimulatedNode::GetOutboundPeerCount() const {
    size_t count = 0;
    for (const auto& peer : peers_) {
        if (peer.is_outbound) count++;
    }
    return count;
}

size_t SimulatedNode::GetInboundPeerCount() const {
    size_t count = 0;
    for (const auto& peer : peers_) {
        if (!peer.is_outbound) count++;
    }
    return count;
}

bool SimulatedNode::IsBanned(const std::string& address) const {
    return ban_man_->IsBanned(address);
}

void SimulatedNode::Ban(const std::string& address, int64_t ban_time_seconds) {
    ban_man_->Ban(address, ban_time_seconds);

    // Disconnect any existing connections to this address
    auto it = peers_.begin();
    while (it != peers_.end()) {
        if (it->address == address) {
            DisconnectFrom(it->node_id);
            it = peers_.begin();  // Restart iteration after modification
        } else {
            ++it;
        }
    }
}

void SimulatedNode::Unban(const std::string& address) {
    ban_man_->Unban(address);
}

network::PeerManager& SimulatedNode::GetPeerManager() {
    // TODO: Return real PeerManager once integrated
    // For now, throw to indicate not implemented
    throw std::runtime_error("PeerManager not yet integrated with SimulatedNode");
}

sync::BanMan& SimulatedNode::GetBanMan() {
    return *ban_man_;
}

void SimulatedNode::OnMessage(const std::vector<uint8_t>& data) {
    stats_.messages_received++;
    stats_.bytes_received += data.size();

    // Simple message parsing (in real implementation, use P2P protocol)
    if (data.size() < 4) return;

    std::string msg_type(data.begin(), data.begin() + 4);

    if (msg_type == "VERS") {
        HandleVersionMessage(data);
    } else if (msg_type == "VACK") {
        HandleVerackMessage(data);
    } else if (msg_type == "BLOC") {
        HandleBlockMessage(data);
    } else if (msg_type == "DISC") {
        HandleDisconnectMessage(data);
    } else if (msg_type == "PING") {
        HandlePingMessage(data);
    } else if (msg_type == "PONG") {
        HandlePongMessage(data);
    }
}

std::string SimulatedNode::GetAddress() const {
    return address_;
}

// Private methods

void SimulatedNode::SetupMessageHandlers() {
    // Register this node with the simulated network
    sim_network_->RegisterNode(node_id_, [this](int from_node_id, const std::vector<uint8_t>& data) {
        this->OnMessage(data);
    });
}

void SimulatedNode::SendVersionMessage(int peer_node_id) {
    // Simplified VERSION message
    std::vector<uint8_t> msg = {'V', 'E', 'R', 'S'};

    // Add node_id as payload
    msg.push_back((node_id_ >> 24) & 0xFF);
    msg.push_back((node_id_ >> 16) & 0xFF);
    msg.push_back((node_id_ >> 8) & 0xFF);
    msg.push_back(node_id_ & 0xFF);

    sim_network_->SendMessage(node_id_, peer_node_id, msg);
    stats_.messages_sent++;
    stats_.bytes_sent += msg.size();
}

void SimulatedNode::HandleVersionMessage(const std::vector<uint8_t>& data) {
    if (data.size() < 8) return;

    // Extract sender node_id
    int sender_id = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];

    // Check if we should accept this connection
    std::string sender_addr = "127.0.0." + std::to_string(sender_id % 255);

    if (ban_man_->IsBanned(sender_addr)) {
        // Ignore banned peer
        return;
    }

    // Check inbound connection limits
    size_t inbound_count = GetInboundPeerCount();
    const size_t MAX_INBOUND = 125;

    if (inbound_count >= MAX_INBOUND) {
        // TODO: Implement eviction logic
        // For now, just reject
        return;
    }

    // Accept connection
    PeerConnection peer;
    peer.node_id = sender_id;
    peer.address = sender_addr;
    peer.port = 8333 + sender_id;
    peer.is_outbound = false;
    peer.connected_time = sim_network_->GetCurrentTime();
    peer.version_received = true;

    peers_.push_back(peer);

    // Send VERACK
    std::vector<uint8_t> verack = {'V', 'A', 'C', 'K'};
    sim_network_->SendMessage(node_id_, sender_id, verack);
    stats_.messages_sent++;
    stats_.bytes_sent += verack.size();
}

void SimulatedNode::HandleVerackMessage(const std::vector<uint8_t>& data) {
    // Mark connection as fully established
    // In a real implementation, we'd track handshake state per peer
}

void SimulatedNode::HandleBlockMessage(const std::vector<uint8_t>& data) {
    // Message format: "BLOC" + sender_id (4 bytes) + serialized header (68 bytes)
    if (data.size() < 76) return;  // 4 bytes + 4 bytes sender + 68 bytes header

    size_t offset = 4;  // Skip "BLOC" prefix

    // Extract sender node ID
    int sender_id = (data[offset] << 24) | (data[offset+1] << 16) |
                    (data[offset+2] << 8) | data[offset+3];
    offset += 4;

    // Deserialize block header
    CBlockHeader header;

    // nVersion (4 bytes)
    header.nVersion = (data[offset] << 24) | (data[offset+1] << 16) |
                      (data[offset+2] << 8) | data[offset+3];
    offset += 4;

    // hashPrevBlock (32 bytes)
    for (int i = 0; i < 32; i++) {
        header.hashPrevBlock.data()[i] = data[offset + i];
    }
    offset += 32;

    // minerAddress (20 bytes)
    for (int i = 0; i < 20; i++) {
        header.minerAddress.data()[i] = data[offset + i];
    }
    offset += 20;

    // nTime (4 bytes)
    header.nTime = (data[offset] << 24) | (data[offset+1] << 16) |
                   (data[offset+2] << 8) | data[offset+3];
    offset += 4;

    // nBits (4 bytes)
    header.nBits = (data[offset] << 24) | (data[offset+1] << 16) |
                   (data[offset+2] << 8) | data[offset+3];
    offset += 4;

    // nNonce (4 bytes)
    header.nNonce = (data[offset] << 24) | (data[offset+1] << 16) |
                    (data[offset+2] << 8) | data[offset+3];

    uint256 block_hash = header.GetHash();

    // Check if we've already seen this block
    if (known_blocks_.count(block_hash)) {
        return;  // Already have it, don't relay again
    }

    // Add to chainstate using real API
    validation::ValidationState state;
    auto* pindex = chainstate_->AcceptBlockHeader(header, state, sender_id);

    if (pindex) {
        chainstate_->TryAddBlockIndexCandidate(pindex);
        chainstate_->ActivateBestChain();

        // Mark as known and track source
        known_blocks_.insert(block_hash);
        block_sources_[block_hash].insert(sender_id);

        // Relay to other peers (except sender)
        for (const auto& peer : peers_) {
            if (peer.node_id != sender_id) {
                std::vector<uint8_t> msg = {'B', 'L', 'O', 'C'};

                // Add our node_id as the sender
                msg.push_back((node_id_ >> 24) & 0xFF);
                msg.push_back((node_id_ >> 16) & 0xFF);
                msg.push_back((node_id_ >> 8) & 0xFF);
                msg.push_back(node_id_ & 0xFF);

                // Serialize header
                msg.push_back((header.nVersion >> 24) & 0xFF);
                msg.push_back((header.nVersion >> 16) & 0xFF);
                msg.push_back((header.nVersion >> 8) & 0xFF);
                msg.push_back(header.nVersion & 0xFF);

                for (int i = 0; i < 32; i++) {
                    msg.push_back(header.hashPrevBlock.data()[i]);
                }

                for (int i = 0; i < 20; i++) {
                    msg.push_back(header.minerAddress.data()[i]);
                }

                msg.push_back((header.nTime >> 24) & 0xFF);
                msg.push_back((header.nTime >> 16) & 0xFF);
                msg.push_back((header.nTime >> 8) & 0xFF);
                msg.push_back(header.nTime & 0xFF);

                msg.push_back((header.nBits >> 24) & 0xFF);
                msg.push_back((header.nBits >> 16) & 0xFF);
                msg.push_back((header.nBits >> 8) & 0xFF);
                msg.push_back(header.nBits & 0xFF);

                msg.push_back((header.nNonce >> 24) & 0xFF);
                msg.push_back((header.nNonce >> 16) & 0xFF);
                msg.push_back((header.nNonce >> 8) & 0xFF);
                msg.push_back(header.nNonce & 0xFF);

                sim_network_->SendMessage(node_id_, peer.node_id, msg);
                stats_.messages_sent++;
                stats_.bytes_sent += msg.size();
            }
        }
    }
}

void SimulatedNode::HandleDisconnectMessage(const std::vector<uint8_t>& data) {
    if (data.size() < 8) return;  // Need "DISC" + node_id

    // Extract sender node ID
    int sender_id = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];

    // Remove peer from our list
    auto it = std::find_if(peers_.begin(), peers_.end(),
        [sender_id](const PeerConnection& p) { return p.node_id == sender_id; });

    if (it != peers_.end()) {
        peers_.erase(it);
        stats_.disconnections++;
    }
}

void SimulatedNode::HandlePingMessage(const std::vector<uint8_t>& data) {
    if (data.size() < 12) return;

    // Extract sender and nonce
    int sender_id = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];

    // Send PONG response
    std::vector<uint8_t> pong = {'P', 'O', 'N', 'G'};
    pong.insert(pong.end(), data.begin() + 4, data.begin() + 12);  // Echo nonce

    sim_network_->SendMessage(node_id_, sender_id, pong);
    stats_.messages_sent++;
    stats_.bytes_sent += pong.size();
}

void SimulatedNode::HandlePongMessage(const std::vector<uint8_t>& data) {
    // Update latency stats
    // In real implementation, calculate RTT from ping time
}

void SimulatedNode::BroadcastBlock(const CBlockHeader& header) {
    // Serialize block header
    std::vector<uint8_t> msg = {'B', 'L', 'O', 'C'};

    // Add sender node_id (4 bytes)
    msg.push_back((node_id_ >> 24) & 0xFF);
    msg.push_back((node_id_ >> 16) & 0xFF);
    msg.push_back((node_id_ >> 8) & 0xFF);
    msg.push_back(node_id_ & 0xFF);

    // Serialize header fields
    // nVersion (4 bytes)
    msg.push_back((header.nVersion >> 24) & 0xFF);
    msg.push_back((header.nVersion >> 16) & 0xFF);
    msg.push_back((header.nVersion >> 8) & 0xFF);
    msg.push_back(header.nVersion & 0xFF);

    // hashPrevBlock (32 bytes)
    for (int i = 0; i < 32; i++) {
        msg.push_back(header.hashPrevBlock.data()[i]);
    }

    // minerAddress (20 bytes)
    for (int i = 0; i < 20; i++) {
        msg.push_back(header.minerAddress.data()[i]);
    }

    // nTime (4 bytes)
    msg.push_back((header.nTime >> 24) & 0xFF);
    msg.push_back((header.nTime >> 16) & 0xFF);
    msg.push_back((header.nTime >> 8) & 0xFF);
    msg.push_back(header.nTime & 0xFF);

    // nBits (4 bytes)
    msg.push_back((header.nBits >> 24) & 0xFF);
    msg.push_back((header.nBits >> 16) & 0xFF);
    msg.push_back((header.nBits >> 8) & 0xFF);
    msg.push_back(header.nBits & 0xFF);

    // nNonce (4 bytes)
    msg.push_back((header.nNonce >> 24) & 0xFF);
    msg.push_back((header.nNonce >> 16) & 0xFF);
    msg.push_back((header.nNonce >> 8) & 0xFF);
    msg.push_back(header.nNonce & 0xFF);

    for (const auto& peer : peers_) {
        sim_network_->SendMessage(node_id_, peer.node_id, msg);
        stats_.messages_sent++;
        stats_.bytes_sent += msg.size();
    }
}

} // namespace test
} // namespace coinbasechain
