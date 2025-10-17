// Copyright (c) 2024 Coinbase Chain
// AttackSimulatedNode implementation - Sends malicious P2P messages for testing

#include "attack_simulated_node.hpp"
#include "network/protocol.hpp"
#include "network/message.hpp"
#include "validation/validation.hpp"
#include <random>

namespace coinbasechain {
namespace test {

CBlockHeader AttackSimulatedNode::CreateDummyHeader(const uint256& prev_hash, uint32_t nBits) {
    CBlockHeader header;
    header.nVersion = 1;
    header.hashPrevBlock = prev_hash;
    header.nTime = static_cast<uint32_t>(sim_network_->GetCurrentTime() / 1000);
    header.nBits = nBits;

    // Random nonce and miner address
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint32_t> dis_nonce;
    header.nNonce = dis_nonce(gen);

    std::uniform_int_distribution<uint8_t> dis_byte(0, 255);
    for (int i = 0; i < 20; i++) {
        header.minerAddress.data()[i] = dis_byte(gen);
    }

    // Set dummy RandomX hash (needed for commitment check)
    header.hashRandomX.SetHex("0000000000000000000000000000000000000000000000000000000000000000");

    return header;
}

void AttackSimulatedNode::SendOrphanHeaders(int peer_node_id, size_t count) {
    printf("[Attack] Node %d sending %zu orphan headers to node %d\n",
           GetId(), count, peer_node_id);

    // Create headers with random prev_hash (parents unknown)
    std::vector<CBlockHeader> headers;
    std::random_device rd;
    std::mt19937_64 gen(rd());

    for (size_t i = 0; i < count; i++) {
        // Random prev_hash - won't exist in victim's chain
        uint256 random_prev_hash;
        std::uniform_int_distribution<uint8_t> dis_byte(0, 255);
        for (int j = 0; j < 32; j++) {
            random_prev_hash.data()[j] = dis_byte(gen);
        }

        CBlockHeader header = CreateDummyHeader(random_prev_hash, params_->GenesisBlock().nBits);
        headers.push_back(header);
    }

    // Serialize HEADERS message
    message::HeadersMessage msg;
    msg.headers = headers;
    auto payload = msg.serialize();

    // Create message header
    auto header = message::create_header(
        protocol::magic::REGTEST,
        protocol::commands::HEADERS,
        payload
    );
    auto header_bytes = message::serialize_header(header);

    // Combine header + payload
    std::vector<uint8_t> full_message;
    full_message.insert(full_message.end(), header_bytes.begin(), header_bytes.end());
    full_message.insert(full_message.end(), payload.begin(), payload.end());

    // Inject directly into SimulatedNetwork, bypassing normal P2P validation
    sim_network_->SendMessage(GetId(), peer_node_id, full_message);

    printf("[Attack] Injected %zu orphan headers into network\n", count);
}

void AttackSimulatedNode::SendInvalidPoWHeaders(int peer_node_id, const uint256& prev_hash, size_t count) {
    printf("[Attack] Node %d sending %zu invalid PoW headers to node %d\n",
           GetId(), count, peer_node_id);

    std::vector<CBlockHeader> headers;
    for (size_t i = 0; i < count; i++) {
        CBlockHeader header = CreateDummyHeader(prev_hash, 0x00000001);  // Impossible difficulty
        header.hashRandomX.SetNull();  // Invalid: NULL RandomX hash
        headers.push_back(header);
    }

    // Serialize and inject
    message::HeadersMessage msg;
    msg.headers = headers;
    auto payload = msg.serialize();

    auto header = message::create_header(protocol::magic::REGTEST, protocol::commands::HEADERS, payload);
    auto header_bytes = message::serialize_header(header);

    std::vector<uint8_t> full_message;
    full_message.insert(full_message.end(), header_bytes.begin(), header_bytes.end());
    full_message.insert(full_message.end(), payload.begin(), payload.end());

    sim_network_->SendMessage(GetId(), peer_node_id, full_message);
    printf("[Attack] Injected %zu invalid PoW headers\n", count);
}

void AttackSimulatedNode::SendNonContinuousHeaders(int peer_node_id, const uint256& prev_hash) {
    printf("[Attack] Node %d sending non-continuous headers to node %d\n",
           GetId(), peer_node_id);

    // Create two headers that don't connect
    CBlockHeader header1 = CreateDummyHeader(prev_hash, params_->GenesisBlock().nBits);
    CBlockHeader header2 = CreateDummyHeader(uint256(), params_->GenesisBlock().nBits);  // Wrong prev_hash!

    std::vector<CBlockHeader> headers = {header1, header2};

    // Serialize and inject
    message::HeadersMessage msg;
    msg.headers = headers;
    auto payload = msg.serialize();

    auto header = message::create_header(protocol::magic::REGTEST, protocol::commands::HEADERS, payload);
    auto header_bytes = message::serialize_header(header);

    std::vector<uint8_t> full_message;
    full_message.insert(full_message.end(), header_bytes.begin(), header_bytes.end());
    full_message.insert(full_message.end(), payload.begin(), payload.end());

    sim_network_->SendMessage(GetId(), peer_node_id, full_message);
    printf("[Attack] Injected non-continuous headers\n");
}

void AttackSimulatedNode::SendOversizedHeaders(int peer_node_id, size_t count) {
    printf("[Attack] Node %d sending %zu oversized headers to node %d\n",
           GetId(), count, peer_node_id);

    if (count <= protocol::MAX_HEADERS_SIZE) {
        printf("[Attack] WARNING: count must be > %d for oversized attack\n", protocol::MAX_HEADERS_SIZE);
        return;
    }

    std::vector<CBlockHeader> headers;
    uint256 prev_hash = GetTipHash();

    for (size_t i = 0; i < count; i++) {
        CBlockHeader header = CreateDummyHeader(prev_hash, params_->GenesisBlock().nBits);
        headers.push_back(header);
        prev_hash = header.GetHash();
    }

    // Serialize and inject oversized message
    message::HeadersMessage msg;
    msg.headers = headers;
    auto payload = msg.serialize();

    auto header = message::create_header(protocol::magic::REGTEST, protocol::commands::HEADERS, payload);
    auto header_bytes = message::serialize_header(header);

    std::vector<uint8_t> full_message;
    full_message.insert(full_message.end(), header_bytes.begin(), header_bytes.end());
    full_message.insert(full_message.end(), payload.begin(), payload.end());

    sim_network_->SendMessage(GetId(), peer_node_id, full_message);
    printf("[Attack] Injected oversized message with %zu headers\n", count);
}

uint256 AttackSimulatedNode::MineBlockPrivate(const std::string& miner_address) {
    printf("[Attack] Node %d mining block PRIVATELY (not broadcasting)\n", GetId());

    // Create block header (same as normal MineBlock)
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

    header.hashRandomX.SetNull();  // Bypass PoW

    // Add to chainstate
    validation::ValidationState state;
    auto& chainstate = GetChainstate();
    auto* pindex = chainstate.AcceptBlockHeader(header, state, GetId());

    if (pindex) {
        chainstate.TryAddBlockIndexCandidate(pindex);
        chainstate.ActivateBestChain();

        uint256 block_hash = header.GetHash();
        printf("[Attack] Mined private block at height %d, hash=%s (NOT broadcasting)\n",
               pindex->nHeight, block_hash.ToString().substr(0, 16).c_str());

        // DO NOT call relay_block() - keep it private!
        return block_hash;
    }

    return uint256();  // Failed
}

void AttackSimulatedNode::BroadcastBlock(const uint256& block_hash, int peer_node_id) {
    printf("[Attack] Node %d broadcasting previously private block: %s to peer %d\n",
           GetId(), block_hash.ToString().substr(0, 16).c_str(), peer_node_id);

    // Look up the block header from our chainstate
    auto& chainstate = GetChainstate();
    const chain::CBlockIndex* pindex = chainstate.LookupBlockIndex(block_hash);

    if (!pindex) {
        printf("[Attack] ERROR: Cannot find block %s in chainstate\n",
               block_hash.ToString().substr(0, 16).c_str());
        return;
    }

    // Get the block header
    CBlockHeader header = pindex->GetBlockHeader();

    // Send as HEADERS message directly to peer
    std::vector<CBlockHeader> headers = {header};
    message::HeadersMessage msg;
    msg.headers = headers;
    auto payload = msg.serialize();

    auto msg_header = message::create_header(
        protocol::magic::REGTEST,
        protocol::commands::HEADERS,
        payload
    );
    auto header_bytes = message::serialize_header(msg_header);

    std::vector<uint8_t> full_message;
    full_message.insert(full_message.end(), header_bytes.begin(), header_bytes.end());
    full_message.insert(full_message.end(), payload.begin(), payload.end());

    sim_network_->SendMessage(GetId(), peer_node_id, full_message);

    printf("[Attack] Broadcast complete for block at height %d\n", pindex->nHeight);
}

} // namespace test
} // namespace coinbasechain
