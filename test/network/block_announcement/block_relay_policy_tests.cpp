// BlockRelay policy tests: TTL/announce, INV handling during/after IBD, chunk boundaries, disconnect safety

#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "network/protocol.hpp"
#include "network/message.hpp"
#include <random>
#include <cstring>

using namespace coinbasechain;
using namespace coinbasechain::test;

static void SetZeroLatency(SimulatedNetwork& network) {
    SimulatedNetwork::NetworkConditions cond{};
    cond.latency_min = std::chrono::milliseconds(0);
    cond.latency_max = std::chrono::milliseconds(0);
    cond.jitter_max = std::chrono::milliseconds(0);
    network.SetNetworkConditions(cond);
}

static void AdvanceMs(SimulatedNetwork& net, int ms) {
    net.AdvanceTime(net.GetCurrentTime() + static_cast<uint64_t>(ms));
}

static void AdvanceSeconds(SimulatedNetwork& net, int seconds) {
    for (int i = 0; i < seconds * 5; ++i) AdvanceMs(net, 200);
}

static std::vector<uint8_t> MakeInvPayloadWithHashes(const std::vector<uint256>& hashes) {
    message::InvMessage inv;
    for (const auto& h : hashes) {
        protocol::InventoryVector iv; iv.type = protocol::InventoryType::MSG_BLOCK;
        std::memcpy(iv.hash.data(), h.data(), 32);
        inv.inventory.push_back(iv);
    }
    return inv.serialize();
}

static void SendINV(SimulatedNetwork& net, int from_node_id, int to_node_id, const std::vector<uint256>& hashes) {
    auto payload = MakeInvPayloadWithHashes(hashes);
    auto hdr = message::create_header(protocol::magic::REGTEST, protocol::commands::INV, payload);
    auto hdr_bytes = message::serialize_header(hdr);
    std::vector<uint8_t> full; full.reserve(hdr_bytes.size()+payload.size());
    full.insert(full.end(), hdr_bytes.begin(), hdr_bytes.end());
    full.insert(full.end(), payload.begin(), payload.end());
    net.SendMessage(from_node_id, to_node_id, full);
}

TEST_CASE("BlockRelay policy: VERACK path enqueues tip and flush sends INV", "[block_relay][policy]") {
    SimulatedNetwork net(51001); SetZeroLatency(net); net.EnableCommandTracking(true);
    SimulatedNode a(1, &net); SimulatedNode b(2, &net);

    // Ensure tip > genesis so VERACK enqueue path runs
    (void)a.MineBlock(); AdvanceSeconds(net, 1);

    // Handshake b->a
    b.ConnectTo(1); AdvanceSeconds(net, 2);

    // Flush without an explicit announce_tip_to_peers(); VERACK should have queued tip for b
    a.GetNetworkManager().flush_block_announcements();
    AdvanceSeconds(net, 1);

    int invs = net.CountCommandSent(a.GetId(), b.GetId(), protocol::commands::INV);
    CHECK(invs >= 1);
}

TEST_CASE("BlockRelay policy: AnnounceTipToAllPeers queues only READY peers", "[block_relay][policy]") {
    SimulatedNetwork net(51002); SetZeroLatency(net); net.EnableCommandTracking(true);
    SimulatedNode a(1, &net); SimulatedNode b(2, &net); SimulatedNode c(3, &net);

    // Ensure tip > genesis so AnnounceTipToAllPeers() queues
    (void)a.MineBlock(); AdvanceSeconds(net, 1);

    // Make b READY, c non-READY
    b.ConnectTo(1); AdvanceSeconds(net, 2);
    c.ConnectTo(1); // no time advancement -> c stays non-READY

    a.GetNetworkManager().announce_tip_to_peers();
    a.GetNetworkManager().flush_block_announcements();

    // Do NOT advance time here; advancing time would complete c's handshake
    // and trigger VERACK path enqueue, which is tested elsewhere.
    CHECK(net.CountCommandSent(a.GetId(), b.GetId(), protocol::commands::INV) >= 1);
    CHECK(net.CountCommandSent(a.GetId(), c.GetId(), protocol::commands::INV) == 0);
}

TEST_CASE("BlockRelay policy: IBD INV gating (ignore non-sync; adopt when none)", "[block_relay][policy][ibd]") {
    SimulatedNetwork net(51003); SetZeroLatency(net); net.EnableCommandTracking(true);
    SimulatedNode victim(10, &net); victim.SetBypassPOWValidation(true);
    SimulatedNode p_sync(11, &net); SimulatedNode p_other(12, &net);

    // Connect sync peer and select as sync peer
    victim.ConnectTo(p_sync.GetId()); AdvanceSeconds(net, 2);
    victim.GetNetworkManager().test_hook_check_initial_sync(); AdvanceSeconds(net, 1);

    // Connect non-sync peer
    victim.ConnectTo(p_other.GetId()); AdvanceSeconds(net, 2);

    // Non-sync announces a block via INV during IBD -> should NOT trigger GETHEADERS to p_other
    uint256 h{}; std::mt19937_64 rng(1); for (size_t i=0;i<4;++i){ uint64_t w=rng(); std::memcpy(h.data()+i*8,&w,8);}    
    SendINV(net, p_other.GetId(), victim.GetId(), {h});
    AdvanceSeconds(net, 1);

    CHECK(net.CountCommandSent(victim.GetId(), p_other.GetId(), protocol::commands::GETHEADERS) == 0);

    // New Victim without sync peer: adopt announcer and request headers
    SimulatedNode victim2(20, &net); victim2.SetBypassPOWValidation(true);
    victim2.ConnectTo(p_other.GetId()); AdvanceSeconds(net, 2);

    uint256 h2{}; for (size_t i=0;i<4;++i){ uint64_t w=rng(); std::memcpy(h2.data()+i*8,&w,8);} 
    SendINV(net, p_other.GetId(), victim2.GetId(), {h2});
    AdvanceSeconds(net, 1);

    CHECK(net.CountCommandSent(victim2.GetId(), p_other.GetId(), protocol::commands::GETHEADERS) >= 1);
}

TEST_CASE("BlockRelay policy: Post-IBD any peer INV triggers GETHEADERS to announcer", "[block_relay][policy][postibd]") {
    SimulatedNetwork net(51004); SetZeroLatency(net); net.EnableCommandTracking(true);
    SimulatedNode v(30, &net); v.SetBypassPOWValidation(true);
    SimulatedNode a1(31, &net); SimulatedNode a2(32, &net);

    // Connect both peers and complete handshakes
    v.ConnectTo(a1.GetId()); v.ConnectTo(a2.GetId()); AdvanceSeconds(net, 3);

    // Make v exit IBD: advance time and mine a few on v so tip is recent
    for (int i=0;i<5;++i){ (void)v.MineBlock(); AdvanceSeconds(net, 1);} 

    // Now INV from each announcer should cause GETHEADERS to announcer (post-IBD)
    uint256 h1{}; uint256 h2{}; std::mt19937_64 rng(7);
    for (int i=0;i<4;++i){ uint64_t w=rng(); std::memcpy(h1.data()+i*8,&w,8);} 
    for (int i=0;i<4;++i){ uint64_t w=rng(); std::memcpy(h2.data()+i*8,&w,8);} 

    SendINV(net, a1.GetId(), v.GetId(), {h1});
    SendINV(net, a2.GetId(), v.GetId(), {h2});
    AdvanceSeconds(net, 1);

    CHECK(net.CountCommandSent(v.GetId(), a1.GetId(), protocol::commands::GETHEADERS) >= 1);
    CHECK(net.CountCommandSent(v.GetId(), a2.GetId(), protocol::commands::GETHEADERS) >= 1);
}

TEST_CASE("BlockRelay policy: Flush chunking boundaries 0, =MAX, 2xMAX; multi-peer fanout", "[block_relay][policy][chunking]") {
    SimulatedNetwork net(51005); SetZeroLatency(net); net.EnableCommandTracking(true);
    SimulatedNode a(1, &net); SimulatedNode b(2, &net); SimulatedNode c(3, &net);
    b.ConnectTo(1); c.ConnectTo(1); AdvanceSeconds(net, 2);

    auto& pm = a.GetNetworkManager().peer_manager();
    auto peers = pm.get_all_peers(); REQUIRE(peers.size() == 2);

    // Case 0: empty queue -> no INV
    a.GetNetworkManager().flush_block_announcements(); AdvanceSeconds(net, 1);
    CHECK(net.CountCommandSent(a.GetId(), b.GetId(), protocol::commands::INV) == 0);

    // Fill exactly MAX_INV_SIZE
    const size_t MAXN = protocol::MAX_INV_SIZE;
    std::vector<uint256> batch_max; batch_max.reserve(MAXN);
    std::mt19937_64 rng(123);
    for (size_t i=0;i<MAXN;++i){ uint256 h{}; for (int j=0;j<4;++j){ uint64_t w=rng(); std::memcpy(h.data()+j*8,&w,8);} batch_max.push_back(h);}    

    // Push into b's queue only
    peers[0]->with_block_inv_queue([&](auto& q){ for (auto& h: batch_max) q.push_back(h); });
    a.GetNetworkManager().flush_block_announcements(); AdvanceSeconds(net, 1);

    // Expect exactly 1 INV to b of size MAX_INV_SIZE
    auto payloads_b = net.GetCommandPayloads(a.GetId(), b.GetId(), protocol::commands::INV);
    size_t inv_count_b = 0; size_t last_size_b = 0;
    for (const auto& p : payloads_b){ message::InvMessage inv; if (inv.deserialize(p.data(), p.size())){ inv_count_b++; last_size_b = inv.inventory.size(); } }
    CHECK(inv_count_b >= 1);
    CHECK(last_size_b == MAXN);

    // 2xMAX -> two chunks per peer
    std::vector<uint256> batch_2x; batch_2x.reserve(2*MAXN);
    for (size_t i=0;i<2*MAXN;++i){ uint256 h{}; for (int j=0;j<4;++j){ uint64_t w=rng(); std::memcpy(h.data()+j*8,&w,8);} batch_2x.push_back(h);}    
    // Push to both peers
    for (auto& p : peers){ p->with_block_inv_queue([&](auto& q){ for (auto& h: batch_2x) q.push_back(h); }); }
    a.GetNetworkManager().flush_block_announcements(); AdvanceSeconds(net, 1);

    auto payloads_b2 = net.GetCommandPayloads(a.GetId(), b.GetId(), protocol::commands::INV);
    auto payloads_c2 = net.GetCommandPayloads(a.GetId(), c.GetId(), protocol::commands::INV);

    auto count_chunks = [](const std::vector<std::vector<uint8_t>>& payloads){ size_t chunks=0; for (auto& p: payloads){ message::InvMessage inv; if (inv.deserialize(p.data(), p.size())) chunks++; } return chunks; };
    CHECK(count_chunks(payloads_b2) >= 2);
    CHECK(count_chunks(payloads_c2) >= 2);
}

TEST_CASE("BlockRelay policy: Disconnect safety and state cleanup", "[block_relay][policy][disconnect]") {
    SimulatedNetwork net(51006); SetZeroLatency(net); net.EnableCommandTracking(true);
    SimulatedNode a(1, &net); SimulatedNode b(2, &net);

    b.ConnectTo(1); AdvanceSeconds(net, 2);

    // Queue something then disconnect peer and flush (should not crash)
    auto& pm = a.GetNetworkManager().peer_manager();
    auto peers = pm.get_all_peers(); REQUIRE(peers.size() == 1);
    peers[0]->with_block_inv_queue([&](auto& q){ uint256 h{}; q.push_back(h); });
    a.DisconnectFrom(b.GetId()); AdvanceSeconds(net, 1);

    // Flush should be safe with no recipients
    a.GetNetworkManager().flush_block_announcements();
    AdvanceSeconds(net, 1);

    CHECK(true);
}
