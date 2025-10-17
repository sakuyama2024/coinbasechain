#ifndef COINBASECHAIN_TEST_SIMULATED_NODE_HPP
#define COINBASECHAIN_TEST_SIMULATED_NODE_HPP

#include "test_chainstate_manager.hpp"
#include "simulated_network.hpp"
#include "network/network_manager.hpp"
#include "network/peer_manager.hpp"
#include "sync/banman.hpp"
#include "chain/chainparams.hpp"
#include <memory>
#include <string>
#include <set>
#include <map>

namespace coinbasechain {
namespace test {

/**
 * SimulatedNode - A lightweight node for P2P testing
 *
 * Combines:
 * - TestChainstateManager (real reorg logic, bypasses PoW for speed)
 * - Real NetworkManager (authentic P2P behavior)
 * - SimulatedNetwork (in-memory networking)
 *
 * Can run thousands of these in a single process.
 */
class SimulatedNode {
public:
    SimulatedNode(int node_id,
                  SimulatedNetwork* network,
                  const chain::ChainParams* params = nullptr);
    ~SimulatedNode();

    // Delete copy constructor/assignment (has unique_ptr members)
    SimulatedNode(const SimulatedNode&) = delete;
    SimulatedNode& operator=(const SimulatedNode&) = delete;

    // Allow move constructor/assignment
    SimulatedNode(SimulatedNode&&) noexcept = default;
    SimulatedNode& operator=(SimulatedNode&&) noexcept = default;

    // Node ID
    int GetId() const { return node_id_; }

    // Connect to another node (initiates handshake)
    bool ConnectTo(int peer_node_id, const std::string& address = "127.0.0.1", uint16_t port = 8333);

    // Disconnect from a peer
    void DisconnectFrom(int peer_id);

    // Mine a block (instant, no PoW)
    uint256 MineBlock(const std::string& miner_address = "test_miner");

    // Get blockchain state
    int GetTipHeight() const;
    uint256 GetTipHash() const;
    const chain::CBlockIndex* GetTip() const;

    // Get network state
    size_t GetPeerCount() const;
    size_t GetOutboundPeerCount() const;
    size_t GetInboundPeerCount() const;

    // Ban management
    bool IsBanned(const std::string& address) const;
    void Ban(const std::string& address, int64_t ban_time_seconds = 86400);
    void Unban(const std::string& address);

    // Access components for advanced testing
    TestChainstateManager& GetChainstate() { return *chainstate_; }
    network::NetworkManager& GetNetworkManager() { return *network_manager_; }
    network::PeerManager& GetPeerManager();
    sync::BanMan& GetBanMan();

    // Process incoming messages (called by SimulatedNetwork)
    void OnMessage(const std::vector<uint8_t>& data);

    // Get node address for connections
    std::string GetAddress() const;
    uint16_t GetPort() const { return port_; }

    // Statistics
    struct NodeStats {
        size_t messages_sent = 0;
        size_t messages_received = 0;
        size_t bytes_sent = 0;
        size_t bytes_received = 0;
        size_t blocks_mined = 0;
        size_t connections_made = 0;
        size_t disconnections = 0;
    };
    NodeStats GetStats() const { return stats_; }

private:
    // Peer connection tracking
    struct PeerConnection {
        int node_id;
        std::string address;
        uint16_t port;
        bool is_outbound;
        uint64_t connected_time;
        bool version_received = false;
        bool verack_received = false;
    };

    // Node identity
    int node_id_;
    uint64_t nonce_;
    std::string address_;
    uint16_t port_;

    // Blockchain (real reorg logic, bypasses PoW)
    std::unique_ptr<TestChainstateManager> chainstate_;

    // Network components (real!)
    std::unique_ptr<network::NetworkManager> network_manager_;
    std::unique_ptr<sync::BanMan> ban_man_;

    // Simulated network
    SimulatedNetwork* sim_network_;

    // Chain parameters
    std::unique_ptr<chain::ChainParams> params_owned_;
    const chain::ChainParams* params_;

    // Peer connections
    std::vector<PeerConnection> peers_;

    // Block relay tracking (prevent relay storms)
    std::set<uint256> known_blocks_;  // Blocks we've seen
    std::map<uint256, std::set<int>> block_sources_;  // Which peers sent us each block

    // Statistics
    NodeStats stats_;

    // Helper: Setup message handlers
    void SetupMessageHandlers();

    // Message handlers
    void SendVersionMessage(int peer_node_id);
    void HandleVersionMessage(const std::vector<uint8_t>& data);
    void HandleVerackMessage(const std::vector<uint8_t>& data);
    void HandleBlockMessage(const std::vector<uint8_t>& data);
    void HandleDisconnectMessage(const std::vector<uint8_t>& data);
    void HandlePingMessage(const std::vector<uint8_t>& data);
    void HandlePongMessage(const std::vector<uint8_t>& data);

    // Block propagation
    void BroadcastBlock(const CBlockHeader& header);
};

} // namespace test
} // namespace coinbasechain

#endif // COINBASECHAIN_TEST_SIMULATED_NODE_HPP
