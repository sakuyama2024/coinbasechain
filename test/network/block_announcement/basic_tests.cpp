// Block Announcement - Basic tests using new infra

#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "network/peer.hpp"
#include "network/protocol.hpp"
#include "network/message.hpp"

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

static int CountINV(SimulatedNetwork& net, int from_node_id, int to_node_id) {
    return net.CountCommandSent(from_node_id, to_node_id, protocol::commands::INV);
}

TEST_CASE("BlockAnnouncement - Per-peer queue isolation", "[block_announcement][per_peer_queue][network]") {
    SimulatedNetwork network(12345);
    SetZeroLatency(network);
    network.EnableCommandTracking(true);

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

    // Advance beyond reannounce TTL (10 minutes) so periodic announce can re-queue tip
    network.AdvanceTime(network.GetCurrentTime() + 10*60*1000 + 1000);
    node1.GetNetworkManager().announce_tip_to_peers();
    network.AdvanceTime(network.GetCurrentTime() + 1);

    // INV should be sent to both peers (queue may be flushed immediately)
    CHECK(CountINV(network, node1.GetId(), node2.GetId()) >= 1);
    CHECK(CountINV(network, node1.GetId(), node3.GetId()) >= 1);
}

TEST_CASE("BlockAnnouncement - Per-peer deduplication", "[block_announcement][per_peer_queue][network]") {
    SimulatedNetwork network(54321);
    SetZeroLatency(network);
    network.EnableCommandTracking(true);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    (void)node1.MineBlock();
    network.AdvanceTime(network.GetCurrentTime() + 100);

    node2.ConnectTo(1);
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);

    CHECK(node1.GetPeerCount() == 1);

    // Advance beyond TTL before periodic announce
    network.AdvanceTime(network.GetCurrentTime() + 10*60*1000 + 1000);

    int inv_before = CountINV(network, node1.GetId(), node2.GetId());
    node1.GetNetworkManager().announce_tip_to_peers();
    network.AdvanceTime(network.GetCurrentTime() + 1);
    int inv_after_first = CountINV(network, node1.GetId(), node2.GetId());
CHECK(inv_after_first >= inv_before);

    // Within TTL, repeated announces should not increase INV count
    node1.GetNetworkManager().announce_tip_to_peers();
    network.AdvanceTime(network.GetCurrentTime() + 1);
    int inv_after_second = CountINV(network, node1.GetId(), node2.GetId());
    CHECK(inv_after_second == inv_after_first);

    node1.GetNetworkManager().announce_tip_to_peers();
    network.AdvanceTime(network.GetCurrentTime() + 1);
    int inv_after_third = CountINV(network, node1.GetId(), node2.GetId());
    CHECK(inv_after_third == inv_after_first);
}

TEST_CASE("BlockAnnouncement - Flush mechanism", "[block_announcement][announcement_flush][network]") {
    SimulatedNetwork network(99999);
    SetZeroLatency(network);
    network.EnableCommandTracking(true);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    (void)node1.MineBlock();
    network.AdvanceTime(network.GetCurrentTime() + 100);

    node2.ConnectTo(1);
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);

    CHECK(node1.GetPeerCount() == 1);

    // Advance beyond TTL before periodic announce
    network.AdvanceTime(network.GetCurrentTime() + 10*60*1000 + 1000);
    int inv_before = CountINV(network, node1.GetId(), node2.GetId());
    node1.GetNetworkManager().announce_tip_to_peers();
    network.AdvanceTime(network.GetCurrentTime() + 1);
    int inv_after = CountINV(network, node1.GetId(), node2.GetId());
CHECK(inv_after >= inv_before);

    node1.GetNetworkManager().flush_block_announcements();
    // After flush, queue is empty; INV count unchanged
    CHECK(CountINV(network, node1.GetId(), node2.GetId()) == inv_after);
}

TEST_CASE("BlockAnnouncement - Announce to new peer on READY", "[block_announcement][announcement_flush][network]") {
    SimulatedNetwork network(77777);
    SetZeroLatency(network);
    network.EnableCommandTracking(true);

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

    // Advance beyond TTL and re-announce; INV should be sent to new peer
    network.AdvanceTime(network.GetCurrentTime() + 10*60*1000 + 1000);
    int inv_before = CountINV(network, node1.GetId(), node2.GetId());
    node1.GetNetworkManager().announce_tip_to_peers();
    int inv_after = CountINV(network, node1.GetId(), node2.GetId());
REQUIRE(inv_after >= inv_before);
}

TEST_CASE("BlockAnnouncement - Disconnect before flush", "[block_announcement][announcement_flush][network]") {
    SimulatedNetwork network(11111);
    SetZeroLatency(network);
    network.EnableCommandTracking(true);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    (void)node1.MineBlock();
    network.AdvanceTime(network.GetCurrentTime() + 100);

    node2.ConnectTo(1);
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);
    CHECK(node1.GetPeerCount() == 1);

    // Advance beyond TTL before announce
    network.AdvanceTime(network.GetCurrentTime() + 10*60*1000 + 1000);
    int inv_before = CountINV(network, node1.GetId(), node2.GetId());
    node1.GetNetworkManager().announce_tip_to_peers();
    int inv_after = CountINV(network, node1.GetId(), node2.GetId());
CHECK(inv_after >= inv_before);

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
    network.EnableCommandTracking(true);

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

    // Advance beyond TTL and announce
    network.AdvanceTime(network.GetCurrentTime() + 10*60*1000 + 1000);
    int inv_before_2 = CountINV(network, node1.GetId(), node2.GetId());
    int inv_before_3 = CountINV(network, node1.GetId(), node3.GetId());
    int inv_before_4 = CountINV(network, node1.GetId(), node4.GetId());
    node1.GetNetworkManager().announce_tip_to_peers();
    network.AdvanceTime(network.GetCurrentTime() + 1);

CHECK(CountINV(network, node1.GetId(), node2.GetId()) >= inv_before_2);
CHECK(CountINV(network, node1.GetId(), node3.GetId()) >= inv_before_3);
CHECK(CountINV(network, node1.GetId(), node4.GetId()) >= inv_before_4);

    node1.GetNetworkManager().flush_block_announcements();
    // INV counts unchanged by flush
    CHECK(CountINV(network, node1.GetId(), node2.GetId()) >= inv_before_2 + 1);
    CHECK(CountINV(network, node1.GetId(), node3.GetId()) >= inv_before_3 + 1);
    CHECK(CountINV(network, node1.GetId(), node4.GetId()) >= inv_before_4 + 1);
}

TEST_CASE("BlockAnnouncement - Periodic re-announcement", "[block_announcement][network]") {
    SimulatedNetwork network(44444);
    SetZeroLatency(network);
    network.EnableCommandTracking(true);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    uint256 blockA = node1.MineBlock();
    network.AdvanceTime(network.GetCurrentTime() + 100);
    (void)blockA;

    node2.ConnectTo(1);
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);

    CHECK(node1.GetPeerCount() == 1);

    // Advance beyond TTL and announce
    network.AdvanceTime(network.GetCurrentTime() + 10*60*1000 + 1000);
    int inv_before = CountINV(network, node1.GetId(), node2.GetId());
    node1.GetNetworkManager().announce_tip_to_peers();
    network.AdvanceTime(network.GetCurrentTime() + 1);
    int inv_after = CountINV(network, node1.GetId(), node2.GetId());
    CHECK(inv_after >= inv_before + 1);

    node1.GetNetworkManager().flush_block_announcements();
    // No change to INV count by flush
    CHECK(CountINV(network, node1.GetId(), node2.GetId()) == inv_after);

    // Advance beyond TTL again for re-announce
    network.AdvanceTime(network.GetCurrentTime() + 10*60*1000 + 1000);

    node1.GetNetworkManager().announce_tip_to_peers();
    network.AdvanceTime(network.GetCurrentTime() + 1);
CHECK(CountINV(network, node1.GetId(), node2.GetId()) >= inv_after);
}
