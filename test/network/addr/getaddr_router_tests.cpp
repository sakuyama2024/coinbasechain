#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "network/network_manager.hpp"
#include "network/message.hpp"
#include "network/message_router.hpp"
#include "test_orchestrator.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;
using namespace coinbasechain::network;
using namespace coinbasechain::protocol;

static std::vector<uint8_t> MakeWire(const std::string& cmd, const std::vector<uint8_t>& payload) {
    auto hdr = message::create_header(magic::REGTEST, cmd, payload);
    auto hdr_bytes = message::serialize_header(hdr);
    std::vector<uint8_t> full; full.reserve(hdr_bytes.size() + payload.size());
    full.insert(full.end(), hdr_bytes.begin(), hdr_bytes.end());
    full.insert(full.end(), payload.begin(), payload.end());
    return full;
}

TEST_CASE("GETADDR ignored pre-VERACK (parity)", "[network][addr][parity][preverack]") {
    SimulatedNetwork net(48100);
    TestOrchestrator orch(&net);
    net.EnableCommandTracking(true);

    SimulatedNode server(1, &net);
    SimulatedNode client(2, &net);

    REQUIRE(client.ConnectTo(server.GetId()));

    // Send GETADDR immediately, before waiting/handshake settling
    auto hdr = message::create_header(magic::REGTEST, commands::GETADDR, {});
    auto hdr_bytes = message::serialize_header(hdr);
    net.SendMessage(client.GetId(), server.GetId(), hdr_bytes);

    // Advance a short time; do NOT settle handshake fully
    orch.AdvanceTime(std::chrono::milliseconds(150));

    // No ADDR should be sent by server to client
    auto payloads = net.GetCommandPayloads(server.GetId(), client.GetId(), commands::ADDR);
    REQUIRE(payloads.empty());

    // Stats should record a prehandshake ignore
    auto& nm = server.GetNetworkManager();
    auto stats = nm.router_for_test().GetGetAddrDebugStats();
    REQUIRE(stats.ignored_prehandshake >= 1);
}

TEST_CASE("GETADDR router counters: served, repeat, outbound ignored", "[network][addr][diagnostics]") {
    SimulatedNetwork net(48101);
    TestOrchestrator orch(&net);
    net.EnableCommandTracking(true);

    SimulatedNode server(1, &net);
    SimulatedNode client(2, &net);

    auto& srv_nm = server.GetNetworkManager();
    auto base0 = srv_nm.router_for_test().GetGetAddrDebugStats();

    REQUIRE(client.ConnectTo(server.GetId()));
    REQUIRE(orch.WaitForConnection(server, client));
    for (int i = 0; i < 12; ++i) orch.AdvanceTime(std::chrono::milliseconds(100));

    // Served once due to client's auto GETADDR after handshake
    auto after_conn = srv_nm.router_for_test().GetGetAddrDebugStats();
    REQUIRE(after_conn.served == base0.served + 1);

    // Repeat ignored (explicit GETADDR on same connection)
    net.SendMessage(client.GetId(), server.GetId(), MakeWire(commands::GETADDR, {}));
    orch.AdvanceTime(std::chrono::milliseconds(200));
    auto s2 = srv_nm.router_for_test().GetGetAddrDebugStats();
    REQUIRE(s2.ignored_repeat >= 1);

    // Outbound ignored (server sends to client)
    net.SendMessage(server.GetId(), client.GetId(), MakeWire(commands::GETADDR, {}));
    orch.AdvanceTime(std::chrono::milliseconds(200));
    auto& cli_nm = client.GetNetworkManager();
    auto cstats = cli_nm.router_for_test().GetGetAddrDebugStats();
    REQUIRE(cstats.ignored_outbound >= 1);
}

TEST_CASE("GETADDR reply shuffles order across seeds", "[network][addr][privacy][shuffle]") {
    SimulatedNetwork net(48102);
    TestOrchestrator orch(&net);
    net.EnableCommandTracking(true);

    SimulatedNode server(1, &net);
    SimulatedNode client(2, &net);

    // Prefill server's AddrMan with multiple addresses
    auto& am = server.GetNetworkManager().address_manager();
    for (int i = 0; i < 10; ++i) {
        NetworkAddress a; a.services = NODE_NETWORK; a.port = 9590;
        for (int j=0;j<10;++j) a.ip[j]=0; a.ip[10]=0xFF; a.ip[11]=0xFF;
        a.ip[12] = 127; a.ip[13] = 0; a.ip[14] = 2; a.ip[15] = static_cast<uint8_t>(i+1);
        am.add(a);
    }

    // Connect and settle
    REQUIRE(client.ConnectTo(server.GetId()));
    REQUIRE(orch.WaitForConnection(server, client));
    for (int i = 0; i < 12; ++i) orch.AdvanceTime(std::chrono::milliseconds(100));

    // Seed 1
    server.GetNetworkManager().router_for_test().TestSeedRng(42);
    net.SendMessage(client.GetId(), server.GetId(), MakeWire(commands::GETADDR, {}));
    orch.AdvanceTime(std::chrono::milliseconds(300));
    auto p1 = net.GetCommandPayloads(server.GetId(), client.GetId(), commands::ADDR);
    REQUIRE_FALSE(p1.empty());
    message::AddrMessage m1; REQUIRE(m1.deserialize(p1.back().data(), p1.back().size()));

    // Reconnect to reset once-per-connection gate
    client.DisconnectFrom(server.GetId());
    REQUIRE(orch.WaitForDisconnect(server, client));
    REQUIRE(client.ConnectTo(server.GetId()));
    REQUIRE(orch.WaitForConnection(server, client));
    for (int i = 0; i < 12; ++i) orch.AdvanceTime(std::chrono::milliseconds(100));

    // Seed 2
    server.GetNetworkManager().router_for_test().TestSeedRng(99);
    net.SendMessage(client.GetId(), server.GetId(), MakeWire(commands::GETADDR, {}));
    orch.AdvanceTime(std::chrono::milliseconds(300));
    auto p2 = net.GetCommandPayloads(server.GetId(), client.GetId(), commands::ADDR);
    REQUIRE_FALSE(p2.empty());
    message::AddrMessage m2; REQUIRE(m2.deserialize(p2.back().data(), p2.back().size()));

    // Compare orders (same set allowed, order should differ with high probability)
    bool same_order = m1.addresses.size() == m2.addresses.size();
    if (same_order) {
        for (size_t i = 0; i < m1.addresses.size(); ++i) {
            for (int j=0;j<16;++j) {
                if (m1.addresses[i].address.ip[j] != m2.addresses[i].address.ip[j]) { same_order = false; break; }
            }
            if (!same_order) break;
        }
    }
    REQUIRE_FALSE(same_order);
}
