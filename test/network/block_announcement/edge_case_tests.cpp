// Block Announcement - Edge case tests using new infra

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

TEST_CASE("BlockAnnouncement - Immediate relay vs queued announcement", "[block_announcement][immediate_relay][network]") {
    SimulatedNetwork network(77777);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);
    SimulatedNode node3(3, &network);

    node2.ConnectTo(1);
    node3.ConnectTo(1);
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);
    CHECK(node1.GetPeerCount() == 2);

    // relay_block via MineBlock() should bypass queues
    (void)node1.MineBlock();
    size_t q2 = GetPeerAnnouncementQueueSize(node1, 2);
    size_t q3 = GetPeerAnnouncementQueueSize(node1, 3);
    CHECK(q2 == 0);
    CHECK(q3 == 0);

    // queued announce
    (void)node1.MineBlock();
    node1.GetNetworkManager().announce_tip_to_peers();
    q2 = GetPeerAnnouncementQueueSize(node1, 2);
    q3 = GetPeerAnnouncementQueueSize(node1, 3);
    CHECK(q2 == 1);
    CHECK(q3 == 1);

    node1.GetNetworkManager().flush_block_announcements();
    CHECK(GetPeerAnnouncementQueueSize(node1, 2) == 0);
    CHECK(GetPeerAnnouncementQueueSize(node1, 3) == 0);
}

TEST_CASE("BlockAnnouncement - Thread safety with concurrent queue access", "[block_announcement][thread_safety][network]") {
    SimulatedNetwork network(88888);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    node2.ConnectTo(1);
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);
    CHECK(node1.GetPeerCount() == 1);

    for (int i = 0; i < 5; i++) (void)node1.MineBlock();

    std::atomic<int> announce_count{0};
    std::atomic<int> flush_count{0};
    std::atomic<bool> test_failed{false};

    auto announce_worker = [&]() {
        for (int i = 0; i < 10; i++) {
            try { node1.GetNetworkManager().announce_tip_to_peers(); announce_count++; }
            catch (...) { test_failed = true; }
        }
    };
    auto flush_worker = [&]() {
        for (int i = 0; i < 10; i++) {
            try { node1.GetNetworkManager().flush_block_announcements(); flush_count++; }
            catch (...) { test_failed = true; }
        }
    };

    std::vector<std::thread> threads;
    threads.emplace_back(announce_worker);
    threads.emplace_back(announce_worker);
    threads.emplace_back(flush_worker);
    threads.emplace_back(flush_worker);
    for (auto& t : threads) t.join();

    CHECK(test_failed == false);
    CHECK(announce_count == 20);
    CHECK(flush_count == 20);

    size_t final_q = GetPeerAnnouncementQueueSize(node1, 2);
    CHECK((final_q == 0 || final_q == 1));
}

TEST_CASE("BlockAnnouncement - Memory management with disconnect", "[block_announcement][memory][network]") {
    SimulatedNetwork network(99999);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    node2.ConnectTo(1);
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);
    CHECK(node1.GetPeerCount() == 1);

    (void)node1.MineBlock();
    node1.GetNetworkManager().announce_tip_to_peers();
    CHECK(GetPeerAnnouncementQueueSize(node1, 2) >= 1);

    node1.DisconnectFrom(2);
    for (int i = 0; i < 10; i++) network.AdvanceTime(network.GetCurrentTime() + 100);
    CHECK(node1.GetPeerCount() == 0);

    node1.GetNetworkManager().flush_block_announcements();
    CHECK(node1.GetPeerCount() == 0);

    node1.GetNetworkManager().announce_tip_to_peers();
    node1.GetNetworkManager().flush_block_announcements();
}
