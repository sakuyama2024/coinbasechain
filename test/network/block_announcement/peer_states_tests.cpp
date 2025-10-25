// Block Announcement - Peer state tests using new infra

#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "network/peer.hpp"
#include "network/protocol.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;

static void SetZeroLatency(SimulatedNetwork& network) {
    SimulatedNetwork::NetworkConditions conditions;
    conditions.latency_min = std::chrono::milliseconds(0);
    conditions.latency_max = std::chrono::milliseconds(0);
    conditions.jitter_max = std::chrono::milliseconds(0);
    network.SetNetworkConditions(conditions);
}

static size_t GetPeerAnnouncementQueueSize(SimulatedNode& node, int peer_node_id) {
    auto& peer_mgr = node.GetNetworkManager().peer_manager();
    auto all_peers = peer_mgr.get_all_peers();
    for (const auto& peer : all_peers) {
        if (!peer) continue;
        if (peer->port() == protocol::ports::REGTEST + peer_node_id) {
            std::lock_guard<std::mutex> lock(peer->block_inv_mutex_);
            return peer->blocks_for_inv_relay_.size();
        }
    }
    return 0;
}

static std::vector<uint256> GetPeerAnnouncementQueue(SimulatedNode& node, int peer_node_id) {
    auto& peer_mgr = node.GetNetworkManager().peer_manager();
    auto all_peers = peer_mgr.get_all_peers();
    for (const auto& peer : all_peers) {
        if (!peer) continue;
        if (peer->port() == protocol::ports::REGTEST + peer_node_id) {
            std::lock_guard<std::mutex> lock(peer->block_inv_mutex_);
            return peer->blocks_for_inv_relay_;
        }
    }
    return {};
}

TEST_CASE("BlockAnnouncement - Mixed peer states (READY vs non-READY)", "[block_announcement][peer_states][network]") {
    SimulatedNetwork network(66666);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);
    SimulatedNode node3(3, &network);

    node2.ConnectTo(1);
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);
    CHECK(node1.GetPeerCount() == 1);

    node3.ConnectTo(1); // do not advance time yet (not READY)

    uint256 blockA = node1.MineBlock(); (void)blockA;

    node1.GetNetworkManager().announce_tip_to_peers();

    size_t q2 = GetPeerAnnouncementQueueSize(node1, 2);
    CHECK(q2 == 1);

    size_t q3 = GetPeerAnnouncementQueueSize(node1, 3);
    CHECK(q3 == 0);

    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);
    CHECK(node1.GetPeerCount() == 2);

    uint256 blockB = node1.MineBlock(); (void)blockB;
    node1.GetNetworkManager().announce_tip_to_peers();

    q3 = GetPeerAnnouncementQueueSize(node1, 3);
    CHECK(q3 == 1);

    auto q3_blocks = GetPeerAnnouncementQueue(node1, 3);
    REQUIRE(q3_blocks.size() == 1);
}
