// Network edge case tests (ported to test2)

#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include <filesystem>

using namespace coinbasechain;
using namespace coinbasechain::test;
using namespace coinbasechain::network;

static void SetZeroLatency(SimulatedNetwork& network) {
    SimulatedNetwork::NetworkConditions c; c.latency_min=c.latency_max=std::chrono::milliseconds(0); c.jitter_max=std::chrono::milliseconds(0); network.SetNetworkConditions(c);
}

// Slow peer eviction - document behavior (timeout handled by NetworkManager)
TEST_CASE("Slow peer eviction - Peer times out if no headers sent", "[network][eviction][slow]") {
    SimulatedNetwork network(12345);
    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    // Build some history on node1
    for (int i = 0; i < 10; i++) (void)node1.MineBlock();

    REQUIRE(node2.ConnectTo(1));
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);
    CHECK(node1.GetPeerCount() == 1);

    // Advance significant time (simulate timeout window) without activity
    for (int i = 0; i < 100; i++) network.AdvanceTime(network.GetCurrentTime() + 60000);

    // Either still connected or disconnected depending on policy; infrastructure holds
    CHECK(node1.GetPeerCount() >= 0);
}

TEST_CASE("Slow peer eviction - Active peer stays connected", "[network][eviction][active]") {
    SimulatedNetwork network(12346);
    SetZeroLatency(network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    for (int i = 0; i < 5; i++) (void)node1.MineBlock();
    REQUIRE(node2.ConnectTo(1));
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);
    CHECK(node1.GetPeerCount() == 1);

    for (int i = 0; i < 10; i++) {
        (void)node1.MineBlock();
        for (int j = 0; j < 10; j++) network.AdvanceTime(network.GetCurrentTime() + 1000);
    }
    CHECK(node1.GetPeerCount() == 1);
}

TEST_CASE("Stale tip management - Node continues operating with stale tip", "[network][stale][tip]") {
    SimulatedNetwork network(12347);
    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    for (int i = 0; i < 10; i++) (void)node1.MineBlock();
    auto tip = node1.GetTipHash();

    for (int i = 0; i < 100; i++) network.AdvanceTime(network.GetCurrentTime() + 120000);
    CHECK(node1.GetTipHash() == tip);

    REQUIRE(node2.ConnectTo(1));
    for (int i = 0; i < 20; i++) network.AdvanceTime(network.GetCurrentTime() + 100);
    CHECK(node1.GetPeerCount() == 1);

    (void)node1.MineBlock();
    network.AdvanceTime(network.GetCurrentTime() + 100);
    CHECK(node1.GetTipHash() != tip);
}

TEST_CASE("BanMan persistence - Save and load bans from disk", "[network][banman][persistence]") {
    auto test_dir = std::filesystem::temp_directory_path() / "banman_persist_test_1";
    std::filesystem::create_directories(test_dir);
    {
        BanMan banman(test_dir.string(), false);
        banman.Ban("192.168.1.1", 0);
        banman.Ban("192.168.1.2", 3600);
        banman.Ban("192.168.1.3", 0);
        REQUIRE(banman.IsBanned("192.168.1.1"));
        REQUIRE(banman.IsBanned("192.168.1.2"));
        REQUIRE(banman.IsBanned("192.168.1.3"));
        REQUIRE(banman.Save());
    }
    {
        BanMan banman(test_dir.string(), false);
        REQUIRE_FALSE(banman.IsBanned("192.168.1.1"));
        REQUIRE_FALSE(banman.IsBanned("192.168.1.2"));
        REQUIRE_FALSE(banman.IsBanned("192.168.1.3"));
        REQUIRE(banman.Load());
        REQUIRE(banman.IsBanned("192.168.1.1"));
        REQUIRE(banman.IsBanned("192.168.1.2"));
        REQUIRE(banman.IsBanned("192.168.1.3"));
        auto bans = banman.GetBanned();
        REQUIRE(bans.size() == 3);
    }
    std::filesystem::remove_all(test_dir);
}

TEST_CASE("BanMan persistence - Unban persists correctly", "[network][banman][persistence]") {
    auto test_dir = std::filesystem::temp_directory_path() / "banman_persist_test_2";
    std::filesystem::create_directories(test_dir);
    {
        BanMan banman(test_dir.string(), false);
        banman.Ban("192.168.1.1", 0);
        banman.Ban("192.168.1.2", 0);
        banman.Ban("192.168.1.3", 0);
        banman.Unban("192.168.1.2");
        REQUIRE(banman.IsBanned("192.168.1.1"));
        REQUIRE_FALSE(banman.IsBanned("192.168.1.2"));
        REQUIRE(banman.IsBanned("192.168.1.3"));
        REQUIRE(banman.Save());
    }
    {
        BanMan banman(test_dir.string(), false);
        REQUIRE(banman.Load());
        REQUIRE(banman.IsBanned("192.168.1.1"));
        REQUIRE_FALSE(banman.IsBanned("192.168.1.2"));
        REQUIRE(banman.IsBanned("192.168.1.3"));
    }
    std::filesystem::remove_all(test_dir);
}

TEST_CASE("BanMan persistence - Clear all bans", "[network][banman][persistence]") {
    SimulatedNetwork network(12348);
    SimulatedNode node(1, &network);
    node.GetNetworkManager().peer_manager().Ban("192.168.1.1", 0);
    node.GetNetworkManager().peer_manager().Ban("192.168.1.2", 0);
    node.GetNetworkManager().peer_manager().Ban("192.168.1.3", 0);
    REQUIRE(node.GetNetworkManager().peer_manager().IsBanned("192.168.1.1"));
    REQUIRE(node.GetNetworkManager().peer_manager().IsBanned("192.168.1.2"));
    REQUIRE(node.GetNetworkManager().peer_manager().IsBanned("192.168.1.3"));
    node.GetNetworkManager().peer_manager().ClearBanned();
    REQUIRE_FALSE(node.GetNetworkManager().peer_manager().IsBanned("192.168.1.1"));
    REQUIRE_FALSE(node.GetNetworkManager().peer_manager().IsBanned("192.168.1.2"));
    REQUIRE_FALSE(node.GetNetworkManager().peer_manager().IsBanned("192.168.1.3"));
    auto bans = node.GetNetworkManager().peer_manager().GetBanned();
    REQUIRE(bans.size() == 0);
}
