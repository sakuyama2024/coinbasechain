#include "catch_amalgamated.hpp"
#include "network/protocol.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include <filesystem>
#include <chrono>

using namespace coinbasechain;
using namespace coinbasechain::test;

static void SetSlowHeaders(SimulatedNetwork& net, uint64_t bandwidth_bytes_per_sec,
                           std::chrono::milliseconds base_latency = std::chrono::milliseconds(50))
{
    SimulatedNetwork::NetworkConditions c;
    c.latency_min = c.latency_max = base_latency;
    c.jitter_max = std::chrono::milliseconds(0);
    c.packet_loss_rate = 0.0;
    c.bandwidth_bytes_per_sec = bandwidth_bytes_per_sec; // throttle big HEADERS payloads
    net.SetNetworkConditions(c);
}

TEST_CASE("IBD resume after restart", "[ibd][network][persistence]") {
    // Use simulated network and nodes with real P2P stack
    SimulatedNetwork net(/*seed=*/424242);
    // Make HEADERS delivery take noticeable time so we can capture mid-sync
    // ~2000 headers * 100 bytes ≈ 200kB per batch; at 10 kB/s => ~20s per batch
    SetSlowHeaders(net, /*bandwidth_bytes_per_sec=*/10 * 1024);

    // Miner with long chain (requires multiple HEADERS batches)
    SimulatedNode miner(1, &net);
    const int CHAIN_LEN = 2500; // > MAX_HEADERS_SIZE (2000) so it takes 2+ batches
    for (int i = 0; i < CHAIN_LEN; ++i) {
        (void)miner.MineBlock();
    }
    REQUIRE(miner.GetTipHeight() == CHAIN_LEN);

    // Fresh syncing node
    std::unique_ptr<SimulatedNode> sync = std::make_unique<SimulatedNode>(2, &net);

    // Connect and begin IBD
    REQUIRE(sync->ConnectTo(miner.GetId()));

    uint64_t t = 1000; // ms
    // Let handshake and initial GETHEADERS happen
    t += 2000; net.AdvanceTime(t);

    // Poll time forward until we observe mid-sync progress
    int mid_height = 0;
    for (int i = 0; i < 12; ++i) { // up to ~120s total
        t += 10'000; // +10s
        net.AdvanceTime(t);
        mid_height = sync->GetTipHeight();
        if (mid_height > 0 && mid_height < CHAIN_LEN) break;
    }

    REQUIRE(mid_height > 0);
    REQUIRE(mid_height < CHAIN_LEN);

    // Persist chainstate to a temp file (simulate shutdown save)
    const std::filesystem::path tmp_path = std::filesystem::temp_directory_path() / "cbc_ibd_resume_headers.json";
    REQUIRE(sync->GetChainstate().Save(tmp_path.string()));

    // Destroy node (simulate process stop)
    sync.reset();

    // Re-create node (simulate restart)
    sync = std::make_unique<SimulatedNode>(2, &net);

    // Load previously saved headers (simulate startup load)
    REQUIRE(sync->GetChainstate().Load(tmp_path.string()));

    // Ensure height after restart is not below the saved mid height
    int height_after_restart = sync->GetTipHeight();
    REQUIRE(height_after_restart >= mid_height);

    // Reconnect and finish sync
    REQUIRE(sync->ConnectTo(miner.GetId()));

    // Advance time in chunks to deliver remaining HEADERS batches
    const int MAX_STEPS = 6;
    for (int i = 0; i < MAX_STEPS && sync->GetTipHeight() < CHAIN_LEN; ++i) {
        t += 45'000; // enough for another batch
        net.AdvanceTime(t);
    }

    REQUIRE(sync->GetTipHeight() == CHAIN_LEN);
    REQUIRE(sync->GetTipHash() == miner.GetTipHash());
}
