// DoS/Privacy: Address echo suppression tests

#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "network/message.hpp"
#include "network/protocol.hpp"
#include "test_orchestrator.hpp"
#include "util/time.hpp"
#include <algorithm>
#include <cstring>

using namespace coinbasechain;
using namespace coinbasechain::test;
using namespace coinbasechain::protocol;

static protocol::TimestampedAddress MakeTsAddrIPv4(const std::string& ip_v4, uint16_t port, uint32_t ts) {
    protocol::TimestampedAddress ta;
    ta.timestamp = ts;
    ta.address.services = protocol::ServiceFlags::NODE_NETWORK;
    ta.address.port = port;
    std::memset(ta.address.ip.data(), 0, 10);
    ta.address.ip[10] = 0xFF; ta.address.ip[11] = 0xFF;
    int a,b,c,d; if (sscanf(ip_v4.c_str(), "%d.%d.%d.%d", &a,&b,&c,&d)==4) {
        ta.address.ip[12] = (uint8_t)a; ta.address.ip[13]=(uint8_t)b; ta.address.ip[14]=(uint8_t)c; ta.address.ip[15]=(uint8_t)d;
    }
    return ta;
}

static std::vector<uint8_t> MakeWire(const std::string& cmd, const std::vector<uint8_t>& payload) {
    auto hdr = message::create_header(magic::REGTEST, cmd, payload);
    auto hdr_bytes = message::serialize_header(hdr);
    std::vector<uint8_t> full; full.reserve(hdr_bytes.size()+payload.size());
    full.insert(full.end(), hdr_bytes.begin(), hdr_bytes.end());
    full.insert(full.end(), payload.begin(), payload.end());
    return full;
}

TEST_CASE("Echo suppression: node does not echo addresses learned from same peer", "[network][addr][echo]") {
    SimulatedNetwork net(88001);
    TestOrchestrator orch(&net);
    net.EnableCommandTracking(true);

    SimulatedNode A(1, &net); // server (receiver of ADDR, responder to GETADDR)
    SimulatedNode B(2, &net); // client

    REQUIRE(B.ConnectTo(A.GetId()));
    REQUIRE(orch.WaitForConnection(A, B));

    // B announces address X to A via ADDR
    const uint32_t ts = static_cast<uint32_t>(coinbasechain::util::GetTime());
    auto X = MakeTsAddrIPv4("10.0.0.42", ports::REGTEST, ts);
    message::AddrMessage addr_msg; addr_msg.addresses.push_back(X);
    auto payload = addr_msg.serialize();
    net.SendMessage(B.GetId(), A.GetId(), MakeWire(commands::ADDR, payload));
    orch.AdvanceTime(std::chrono::milliseconds(200));

    // B requests GETADDR; A should NOT include X back to B
    net.SendMessage(B.GetId(), A.GetId(), MakeWire(commands::GETADDR, {}));
    orch.AdvanceTime(std::chrono::milliseconds(300));

    auto payloads = net.GetCommandPayloads(A.GetId(), B.GetId(), commands::ADDR);
    REQUIRE_FALSE(payloads.empty());

    message::AddrMessage resp; REQUIRE(resp.deserialize(payloads.back().data(), payloads.back().size()));

    auto to_key = [](const protocol::NetworkAddress& a){
        char buf[64];
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", a.ip[12], a.ip[13], a.ip[14], a.ip[15], a.port);
        return std::string(buf);
    };
    std::string x_key = "10.0.0.42:" + std::to_string(ports::REGTEST);
    bool found_x = false;
    for (const auto& ta : resp.addresses) {
        if (to_key(ta.address) == x_key) { found_x = true; break; }
    }
    REQUIRE_FALSE(found_x);
}

TEST_CASE("Echo suppression is per-peer: addresses from C are served to other peers", "[network][addr][echo][per-peer]") {
    SimulatedNetwork net(88002);
    TestOrchestrator orch(&net);
    net.EnableCommandTracking(true);

    SimulatedNode A(1, &net); // server
    SimulatedNode B(2, &net); // client 1
    SimulatedNode C(3, &net); // client 2

    REQUIRE(B.ConnectTo(A.GetId()));
    REQUIRE(C.ConnectTo(A.GetId()));
    REQUIRE(orch.WaitForConnection(A, B));
    REQUIRE(orch.WaitForConnection(A, C));

    // C announces X to A
    const uint32_t ts = static_cast<uint32_t>(coinbasechain::util::GetTime());
    auto X = MakeTsAddrIPv4("10.0.0.99", ports::REGTEST, ts);
    message::AddrMessage addr_msg; addr_msg.addresses.push_back(X);
    auto payload = addr_msg.serialize();
    net.SendMessage(C.GetId(), A.GetId(), MakeWire(commands::ADDR, payload));
    orch.AdvanceTime(std::chrono::milliseconds(200));

    // C requests GETADDR; A should NOT include X back to C (echo suppression)
    net.SendMessage(C.GetId(), A.GetId(), MakeWire(commands::GETADDR, {}));
    orch.AdvanceTime(std::chrono::milliseconds(500));

    auto payloads_AC = net.GetCommandPayloads(A.GetId(), C.GetId(), commands::ADDR);
    REQUIRE_FALSE(payloads_AC.empty());

    message::AddrMessage respC; REQUIRE(respC.deserialize(payloads_AC.back().data(), payloads_AC.back().size()));

    auto to_key = [](const protocol::NetworkAddress& a){
        char buf[64];
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", a.ip[12], a.ip[13], a.ip[14], a.ip[15], a.port);
        return std::string(buf);
    };
    std::string x_key = "10.0.0.99:" + std::to_string(ports::REGTEST);

    bool found_x_in_C = false;
    for (const auto& ta : respC.addresses) {
        if (to_key(ta.address) == x_key) { found_x_in_C = true; break; }
    }
    REQUIRE_FALSE(found_x_in_C);

    // B requests GETADDR; A SHOULD include X to B (no suppression for different peer)
    net.SendMessage(B.GetId(), A.GetId(), MakeWire(commands::GETADDR, {}));
    orch.AdvanceTime(std::chrono::milliseconds(400));

    auto payloads_AB = net.GetCommandPayloads(A.GetId(), B.GetId(), commands::ADDR);
    REQUIRE_FALSE(payloads_AB.empty());

    bool found_x_in_B = false;
    for (const auto& pl : payloads_AB) {
        message::AddrMessage respB;
        if (!respB.deserialize(pl.data(), pl.size())) continue;
        for (const auto& ta : respB.addresses) {
            if (to_key(ta.address) == x_key) { found_x_in_B = true; break; }
        }
        if (found_x_in_B) break;
    }
    REQUIRE(found_x_in_B);
}

TEST_CASE("Echo suppression TTL expiry allows address to be served back after 10m", "[network][addr][echo][ttl]") {
    SimulatedNetwork net(88003);
    TestOrchestrator orch(&net);
    net.EnableCommandTracking(true);

    SimulatedNode A(1, &net); // server
    SimulatedNode B(2, &net); // client

    REQUIRE(B.ConnectTo(A.GetId()));
    REQUIRE(orch.WaitForConnection(A, B));

    // B announces Y to A
    const uint32_t ts2 = static_cast<uint32_t>(coinbasechain::util::GetTime());
    auto Y = MakeTsAddrIPv4("10.0.0.77", ports::REGTEST, ts2);
    message::AddrMessage addr_msg2; addr_msg2.addresses.push_back(Y);
    auto payload2 = addr_msg2.serialize();
    net.SendMessage(B.GetId(), A.GetId(), MakeWire(commands::ADDR, payload2));
    orch.AdvanceTime(std::chrono::milliseconds(200));

    // Immediate GETADDR from B: Y must be suppressed
    net.SendMessage(B.GetId(), A.GetId(), MakeWire(commands::GETADDR, {}));
    orch.AdvanceTime(std::chrono::milliseconds(400));
    auto payloads_AB1 = net.GetCommandPayloads(A.GetId(), B.GetId(), commands::ADDR);
    REQUIRE_FALSE(payloads_AB1.empty());
    message::AddrMessage resp1; REQUIRE(resp1.deserialize(payloads_AB1.back().data(), payloads_AB1.back().size()));
    auto to_key2 = [](const protocol::NetworkAddress& a){
        char buf[64];
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", a.ip[12], a.ip[13], a.ip[14], a.ip[15], a.port);
        return std::string(buf);
    };
    std::string y_key = "10.0.0.77:" + std::to_string(ports::REGTEST);
    bool found_y_early = false;
    for (const auto& ta : resp1.addresses) {
        if (to_key2(ta.address) == y_key) { found_y_early = true; break; }
    }
    REQUIRE_FALSE(found_y_early);

    // Advance time beyond 10 minutes (TTL) and try again
    orch.AdvanceTime(std::chrono::seconds(601));

    net.SendMessage(B.GetId(), A.GetId(), MakeWire(commands::GETADDR, {}));
    orch.AdvanceTime(std::chrono::milliseconds(400));
    auto payloads_AB2 = net.GetCommandPayloads(A.GetId(), B.GetId(), commands::ADDR);
    REQUIRE_FALSE(payloads_AB2.empty());

    bool found_y_late = false;
    for (const auto& pl : payloads_AB2) {
        message::AddrMessage resp2;
        if (!resp2.deserialize(pl.data(), pl.size())) continue;
        for (const auto& ta : resp2.addresses) {
            if (to_key2(ta.address) == y_key) { found_y_late = true; break; }
        }
        if (found_y_late) break;
    }
    REQUIRE(found_y_late);
}
