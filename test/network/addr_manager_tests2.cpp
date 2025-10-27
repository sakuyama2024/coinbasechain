#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "test_orchestrator.hpp"
#include "network/protocol.hpp"
#include "network/message.hpp"
#include "network/addr_manager.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;
using namespace coinbasechain::network;
using namespace coinbasechain::protocol;

static std::vector<uint8_t> MakeWire(const std::string& cmd, const std::vector<uint8_t>& payload) {
    auto hdr = message::create_header(magic::REGTEST, cmd, payload);
    auto hdr_bytes = message::serialize_header(hdr);
    std::vector<uint8_t> full;
    full.reserve(hdr_bytes.size() + payload.size());
    full.insert(full.end(), hdr_bytes.begin(), hdr_bytes.end());
    full.insert(full.end(), payload.begin(), payload.end());
    return full;
}

TEST_CASE("GETADDR: answered only for inbound peers", "[network][addr]") {
    SimulatedNetwork net(2601);
    TestOrchestrator orch(&net);

    SimulatedNode victim(1, &net);
    SimulatedNode inbound_peer(2, &net);
    SimulatedNode outbound_peer(3, &net);

    // Case 1: inbound peer -> victim should respond with ADDR
    net.EnableCommandTracking(true);
    REQUIRE(inbound_peer.ConnectTo(1));
    REQUIRE(orch.WaitForConnection(victim, inbound_peer));

    std::vector<uint8_t> getaddr_payload; // empty
    auto wire1 = MakeWire(commands::GETADDR, getaddr_payload);
    net.SendMessage(inbound_peer.GetId(), victim.GetId(), wire1);
    orch.AdvanceTime(std::chrono::milliseconds(200));
    REQUIRE(net.CountCommandSent(victim.GetId(), inbound_peer.GetId(), commands::ADDR) >= 1);

    // Case 2: victim is outbound to peer -> ignore GETADDR
    REQUIRE(victim.ConnectTo(3));
    REQUIRE(orch.WaitForConnection(victim, outbound_peer));

    auto wire2 = MakeWire(commands::GETADDR, {});
    net.SendMessage(outbound_peer.GetId(), victim.GetId(), wire2);
    orch.AdvanceTime(std::chrono::milliseconds(200));
    REQUIRE(net.CountCommandSent(victim.GetId(), outbound_peer.GetId(), commands::ADDR) == 0);
}

TEST_CASE("ADDR response is capped at MAX_ADDR_SIZE", "[network][addr]") {
    SimulatedNetwork net(2602);
    TestOrchestrator orch(&net);

    SimulatedNode victim(1, &net);
    SimulatedNode requester(2, &net);

    // Pre-fill victim's AddressManager with many addresses
    auto& am = victim.GetNetworkManager().address_manager();
    for (int i = 0; i < 5000; ++i) {
        protocol::NetworkAddress addr;
        addr.services = NODE_NETWORK;
        addr.port = 9590;
        // 127.0.1.x IPv4-mapped
        for (int j = 0; j < 10; ++j) addr.ip[j] = 0; addr.ip[10] = 0xFF; addr.ip[11] = 0xFF;
        addr.ip[12] = 127; addr.ip[13] = 0; addr.ip[14] = 1; addr.ip[15] = static_cast<uint8_t>(i % 255);
        am.add(addr);
    }

    net.EnableCommandTracking(true);
    REQUIRE(requester.ConnectTo(1));
    REQUIRE(orch.WaitForConnection(victim, requester));

    auto getaddr = MakeWire(commands::GETADDR, {});
    net.SendMessage(requester.GetId(), victim.GetId(), getaddr);
    orch.AdvanceTime(std::chrono::milliseconds(300));

    auto payloads = net.GetCommandPayloads(victim.GetId(), requester.GetId(), commands::ADDR);
    REQUIRE_FALSE(payloads.empty());

    message::AddrMessage msg;
    REQUIRE(msg.deserialize(payloads.front().data(), payloads.front().size()));
    REQUIRE(msg.addresses.size() <= MAX_ADDR_SIZE);
}

TEST_CASE("good() is called on outbound after VERACK (moves to tried)", "[network][addr]") {
    SimulatedNetwork net(2603);
    TestOrchestrator orch(&net);

    SimulatedNode victim(1, &net);
    SimulatedNode peer(2, &net);

    auto& am = victim.GetNetworkManager().address_manager();
    size_t tried_before = am.tried_count();

    // Pre-seed address so good() can move it from new->tried deterministically
    auto addr_peer = protocol::NetworkAddress::from_string(peer.GetAddress(), peer.GetPort(), NODE_NETWORK);
    am.add(addr_peer);

    // Outbound from victim to peer
    REQUIRE(victim.ConnectTo(2));
    REQUIRE(orch.WaitForConnection(victim, peer));

    // Allow handshake to complete
    for (int i = 0; i < 30; ++i) orch.AdvanceTime(std::chrono::milliseconds(100));

    size_t tried_after = am.tried_count();
    REQUIRE(tried_after >= tried_before + 1);
}

TEST_CASE("cleanup_stale removes terrible entries (too many failures)", "[network][addr]") {
    AddressManager am;

    protocol::NetworkAddress a1; for (int i=0;i<16;++i) a1.ip[i]=0; a1.services=NODE_NETWORK; a1.port=9590; a1.ip[15]=10;
    protocol::NetworkAddress a2 = a1; a2.ip[15]=11;

    REQUIRE(am.add(a1));
    REQUIRE(am.add(a2));
    REQUIRE(am.size() == 2);

    // Make a1 terrible by repeated failures
    for (int i = 0; i < 20; ++i) am.failed(a1);

    am.cleanup_stale();

    REQUIRE(am.size() == 1);
}

TEST_CASE("GETADDR empty address manager sends zero addresses", "[network][addr]") {
    SimulatedNetwork net(2604);
    TestOrchestrator orch(&net);

    SimulatedNode victim(1, &net);
    SimulatedNode requester(2, &net);

    net.EnableCommandTracking(true);

    REQUIRE(requester.ConnectTo(1));
    REQUIRE(orch.WaitForConnection(victim, requester));

    auto getaddr = MakeWire(commands::GETADDR, {});
    net.SendMessage(requester.GetId(), victim.GetId(), getaddr);
    orch.AdvanceTime(std::chrono::milliseconds(300));

    auto payloads = net.GetCommandPayloads(victim.GetId(), requester.GetId(), commands::ADDR);

    // Deterministic: expect a single ADDR response with zero addresses when empty
    REQUIRE(payloads.size() >= 1);
    message::AddrMessage msg;
    REQUIRE(msg.deserialize(payloads.front().data(), payloads.front().size()));
    REQUIRE(msg.addresses.size() == 0);
}
