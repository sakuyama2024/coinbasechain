// Block Announcement - Basic tests using new infra

#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "network/peer.hpp"
#include "network/protocol.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;
using namespace coinbasechain::network;

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

static struct TestSetup {
    TestSetup() { chain::GlobalChainParams::Select(chain::ChainType::REGTEST); }
} block_announcement_setup;

TEST_CASE("BlockAnnouncement - Per-peer queue isolation", "[block_announcement][per_peer_queue][network]") {
    SimulatedNetwork network(12345);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);
    SimulatedNode node3(3, &network);

    uint256 blockA = node1.MineBlock();
    network.AdvanceTime(network.GetCurrentTime() + 100);
    (void)blockA;

    node2.ConnectTo(1);
    node3.ConnectTo(1);
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);

    CHECK(node1.GetPeerCount() == 2);

    node1.GetNetworkManager().announce_tip_to_peers();

    CHECK(GetPeerAnnouncementQueueSize(node1, 2) == 1);
    CHECK(GetPeerAnnouncementQueueSize(node1, 3) == 1);

    auto q2 = GetPeerAnnouncementQueue(node1, 2);
    auto q3 = GetPeerAnnouncementQueue(node1, 3);
    REQUIRE(q2.size() == 1);
    REQUIRE(q3.size() == 1);
}

TEST_CASE("BlockAnnouncement - Per-peer deduplication", "[block_announcement][per_peer_queue][network]") {
    SimulatedNetwork network(54321);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    (void)node1.MineBlock();
    network.AdvanceTime(network.GetCurrentTime() + 100);

    node2.ConnectTo(1);
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);

    CHECK(node1.GetPeerCount() == 1);

    node1.GetNetworkManager().announce_tip_to_peers();
    CHECK(GetPeerAnnouncementQueueSize(node1, 2) == 1);

    node1.GetNetworkManager().announce_tip_to_peers();
    CHECK(GetPeerAnnouncementQueueSize(node1, 2) == 1);

    node1.GetNetworkManager().announce_tip_to_peers();
    CHECK(GetPeerAnnouncementQueueSize(node1, 2) == 1);
}

TEST_CASE("BlockAnnouncement - Flush mechanism", "[block_announcement][announcement_flush][network]") {
    SimulatedNetwork network(99999);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    (void)node1.MineBlock();
    network.AdvanceTime(network.GetCurrentTime() + 100);

    node2.ConnectTo(1);
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);

    CHECK(node1.GetPeerCount() == 1);

    node1.GetNetworkManager().announce_tip_to_peers();
    CHECK(GetPeerAnnouncementQueueSize(node1, 2) == 1);

    node1.GetNetworkManager().flush_block_announcements();
    CHECK(GetPeerAnnouncementQueueSize(node1, 2) == 0);
}

TEST_CASE("BlockAnnouncement - Announce to new peer on READY", "[block_announcement][announcement_flush][network]") {
    SimulatedNetwork network(77777);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    (void)node1.MineBlock();
    (void)node1.MineBlock();
    uint256 blockC = node1.MineBlock();
    network.AdvanceTime(network.GetCurrentTime() + 100);
    (void)blockC;

    node2.ConnectTo(1);
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);

    CHECK(node1.GetPeerCount() == 1);

    node1.GetNetworkManager().announce_tip_to_peers();
    auto q2 = GetPeerAnnouncementQueue(node1, 2);
    REQUIRE(q2.size() >= 1);
}

TEST_CASE("BlockAnnouncement - Disconnect before flush", "[block_announcement][announcement_flush][network]") {
    SimulatedNetwork network(11111);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    (void)node1.MineBlock();
    network.AdvanceTime(network.GetCurrentTime() + 100);

    node2.ConnectTo(1);
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);
    CHECK(node1.GetPeerCount() == 1);

    node1.GetNetworkManager().announce_tip_to_peers();
    CHECK(GetPeerAnnouncementQueueSize(node1, 2) == 1);

    node2.DisconnectFrom(1);
    network.AdvanceTime(network.GetCurrentTime() + 100);
    CHECK(node1.GetPeerCount() == 0);

    node1.GetNetworkManager().flush_block_announcements();
    // No crash expected

    (void)node1.MineBlock();
    network.AdvanceTime(network.GetCurrentTime() + 100);
    CHECK(node1.GetTipHeight() == 2);
}

TEST_CASE("BlockAnnouncement - Multiple blocks batched in single INV", "[block_announcement][announcement_flush][network]") {
    SimulatedNetwork network(22222);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    node2.ConnectTo(1);
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);
    CHECK(node1.GetPeerCount() == 1);

    for (int i = 0; i < 5; i++) {
        (void)node1.MineBlock();
        network.AdvanceTime(network.GetCurrentTime() + 50);
    }
    CHECK(node1.GetTipHeight() == 5);

    node1.GetNetworkManager().announce_tip_to_peers();
    size_t queue_size = GetPeerAnnouncementQueueSize(node1, 2);
    (void)queue_size;

    node1.GetNetworkManager().flush_block_announcements();
    CHECK(GetPeerAnnouncementQueueSize(node1, 2) == 0);
}

TEST_CASE("BlockAnnouncement - Multi-peer propagation (3-5 nodes)", "[block_announcement][network]") {
    SimulatedNetwork network(33333);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);
    SimulatedNode node3(3, &network);
    SimulatedNode node4(4, &network);

    (void)node1.MineBlock();
    network.AdvanceTime(network.GetCurrentTime() + 100);

    node2.ConnectTo(1);
    node3.ConnectTo(1);
    node4.ConnectTo(1);
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);

    CHECK(node1.GetPeerCount() == 3);

    node1.GetNetworkManager().announce_tip_to_peers();

    CHECK(GetPeerAnnouncementQueueSize(node1, 2) == 1);
    CHECK(GetPeerAnnouncementQueueSize(node1, 3) == 1);
    CHECK(GetPeerAnnouncementQueueSize(node1, 4) == 1);

    node1.GetNetworkManager().flush_block_announcements();
    CHECK(GetPeerAnnouncementQueueSize(node1, 2) == 0);
    CHECK(GetPeerAnnouncementQueueSize(node1, 3) == 0);
    CHECK(GetPeerAnnouncementQueueSize(node1, 4) == 0);
}

TEST_CASE("BlockAnnouncement - Periodic re-announcement", "[block_announcement][network]") {
    SimulatedNetwork network(44444);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    uint256 blockA = node1.MineBlock();
    network.AdvanceTime(network.GetCurrentTime() + 100);
    (void)blockA;

    node2.ConnectTo(1);
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);

    CHECK(node1.GetPeerCount() == 1);

    node1.GetNetworkManager().announce_tip_to_peers();
    CHECK(GetPeerAnnouncementQueueSize(node1, 2) == 1);

    node1.GetNetworkManager().flush_block_announcements();
    CHECK(GetPeerAnnouncementQueueSize(node1, 2) == 0);

    for (int i = 0; i < 5; i++) network.AdvanceTime(network.GetCurrentTime() + 1000);

    node1.GetNetworkManager().announce_tip_to_peers();
    CHECK(GetPeerAnnouncementQueueSize(node1, 2) == 1);

    auto queue = GetPeerAnnouncementQueue(node1, 2);
    REQUIRE(queue.size() == 1);
}
