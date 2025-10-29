// Header sync reselection regression tests

#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "network/protocol.hpp"
#include "network/message.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;
using namespace coinbasechain::protocol;

static void SetZeroLatency(SimulatedNetwork& net) {
    SimulatedNetwork::NetworkConditions c; c.latency_min=c.latency_max=std::chrono::milliseconds(0); c.jitter_max=std::chrono::milliseconds(0); net.SetNetworkConditions(c);
}

TEST_CASE("HeaderSync - Reselection after stall and empty HEADERS can reuse peer", "[network_header_sync][regression]") {
    SimulatedNetwork net(52001);
    SetZeroLatency(net);
    net.EnableCommandTracking(true);

    // Miner builds a short chain
    SimulatedNode miner(10, &net);
    for (int i = 0; i < 30; ++i) (void)miner.MineBlock();

    // Two serving peers that sync to miner
    SimulatedNode p1(11, &net);
    SimulatedNode p2(12, &net);
    p1.ConnectTo(miner.GetId());
    p2.ConnectTo(miner.GetId());
    uint64_t t = 1000; net.AdvanceTime(t);

    REQUIRE(p1.GetTipHeight() == 30);
    REQUIRE(p2.GetTipHeight() == 30);

    // Victim connects to both
    SimulatedNode victim(13, &net);
    // Connect victim to p1 first to force initial sync selection to p1
    victim.ConnectTo(p1.GetId());
    t += 200; net.AdvanceTime(t);

    // Trigger initial sync selection to p1
    victim.GetNetworkManager().test_hook_check_initial_sync();
    t += 200; net.AdvanceTime(t);

    // Now connect to p2 (will be available for reselection)
    victim.ConnectTo(p2.GetId());
    t += 200; net.AdvanceTime(t);

    // Verify GETHEADERS went to p1 at least once
    int gh_p1_initial = net.CountCommandSent(victim.GetId(), p1.GetId(), commands::GETHEADERS);
    REQUIRE(gh_p1_initial >= 1);

    int sync_peer_id = p1.GetId();
    int other_peer_id = p2.GetId();

    // Round 1: Stall current sync peer, verify reselection to the other peer
    {
        // Record baseline GETHEADERS to other peer before stall
        int gh_other_baseline = net.CountCommandSent(victim.GetId(), other_peer_id, commands::GETHEADERS);

        SimulatedNetwork::NetworkConditions drop; drop.packet_loss_rate = 1.0; // stall: drop messages to victim
        net.SetLinkConditions(sync_peer_id, victim.GetId(), drop);

        // Advance beyond stall timeout and process timers (ProcessTimers itself calls CheckInitialSync on stall)
        for (int i = 0; i < 4; ++i) {
            t += 60 * 1000;
            net.AdvanceTime(t);
            victim.GetNetworkManager().test_hook_header_sync_process_timers();
        }

        // Poll for GETHEADERS to other peer increasing beyond baseline
        bool switched = false;
        for (int i = 0; i < 20; ++i) {
            t += 200; net.AdvanceTime(t);
            int gh_now = net.CountCommandSent(victim.GetId(), other_peer_id, commands::GETHEADERS);
            if (gh_now > gh_other_baseline) { switched = true; break; }
        }
        CHECK(switched);

        // Switch roles: track the new sync peer
        sync_peer_id = other_peer_id;
        other_peer_id = (sync_peer_id == p1.GetId()) ? p2.GetId() : p1.GetId();
    }

    // Round 2: From current sync peer, send empty HEADERS to trigger ClearSyncPeer()
    // Then verify that the SAME peer can be reselected (sync_started was cleared).
    {
        // Baseline counts on both peers before triggering reselection
        int gh_p1_base = net.CountCommandSent(victim.GetId(), p1.GetId(), commands::GETHEADERS);
        int gh_p2_base = net.CountCommandSent(victim.GetId(), p2.GetId(), commands::GETHEADERS);

        // Build empty HEADERS message
        message::HeadersMessage empty;
        auto payload = empty.serialize();
        auto hdr = message::create_header(protocol::magic::REGTEST, commands::HEADERS, payload);
        auto hdr_bytes = message::serialize_header(hdr);
        std::vector<uint8_t> full; full.reserve(hdr_bytes.size()+payload.size());
        full.insert(full.end(), hdr_bytes.begin(), hdr_bytes.end());
        full.insert(full.end(), payload.begin(), payload.end());

        // Inject from current sync peer
        net.SendMessage(sync_peer_id, victim.GetId(), full);
        for (int i = 0; i < 5; ++i) { t += 200; net.AdvanceTime(t); }

        // Poll for reselection to any peer (GETHEADERS should increase on either)
        bool reselected = false;
        for (int i = 0; i < 20; ++i) {
            t += 200; net.AdvanceTime(t);
            victim.GetNetworkManager().test_hook_check_initial_sync();
            int gh_p1_now = net.CountCommandSent(victim.GetId(), p1.GetId(), commands::GETHEADERS);
            int gh_p2_now = net.CountCommandSent(victim.GetId(), p2.GetId(), commands::GETHEADERS);
            if (gh_p1_now > gh_p1_base || gh_p2_now > gh_p2_base) { reselected = true; break; }
        }
        CHECK(reselected);
    }
}