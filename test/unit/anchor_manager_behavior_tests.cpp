#include "catch_amalgamated.hpp"
#include "network/anchor_manager.hpp"
#include "network/peer_manager.hpp"
#include "network/protocol.hpp"
#include <boost/asio.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace coinbasechain;
using namespace coinbasechain::network;
using json = nlohmann::json;

static std::string tmpfile(const char* name) { return std::string("/tmp/") + name; }

TEST_CASE("AnchorManager::SaveAnchors - no peers -> early return, no file", "[unit][anchor]") {
    boost::asio::io_context io;
    AddressManager addrman;
    PeerManager peermgr(io, addrman);

    // Callbacks (unused in Save)
    AnchorManager::AddressToStringCallback tostr = [](const protocol::NetworkAddress&){ return std::optional<std::string>{}; };
AnchorManager::ConnectCallback connect = [](const protocol::NetworkAddress&, bool /*noban*/ ){};

    AnchorManager am(peermgr, tostr, connect);

    const std::string path = tmpfile("am_save_none.json");
    std::filesystem::remove(path);

    CHECK(am.SaveAnchors(path));
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("AnchorManager::LoadAnchors - connects capped at 2 and deletes file", "[unit][anchor]") {
    boost::asio::io_context io;
    AddressManager addrman;
    PeerManager peermgr(io, addrman);

    // Helper to stringify NetworkAddress (IPv4-mapped IPv6)
    auto to_ip_str = [](const protocol::NetworkAddress& addr) -> std::optional<std::string> {
        try {
            boost::asio::ip::address_v6::bytes_type bytes;
            std::copy(addr.ip.begin(), addr.ip.end(), bytes.begin());
            auto v6 = boost::asio::ip::make_address_v6(bytes);
            if (v6.is_v4_mapped()) {
                return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6).to_string();
            }
            return v6.to_string();
        } catch(...) { return std::nullopt; }
    };

    std::vector<std::pair<std::string,uint16_t>> attempts;
AnchorManager::ConnectCallback connect = [&](const protocol::NetworkAddress& a, bool /*noban*/){
        auto s = to_ip_str(a);
        attempts.emplace_back(s.value_or("<bad>"), a.port);
    };

    AnchorManager am(peermgr, to_ip_str, connect);

    const std::string path = tmpfile("am_load_caps.json");
    std::filesystem::remove(path);

    json root; root["version"] = 1; root["count"] = 3; root["anchors"] = json::array();
    auto add = [&](int node_id){
        json j; j["services"] = 1; j["port"] = protocol::ports::REGTEST + node_id; j["ip"] = json::array();
        for (int i=0;i<10;++i) j["ip"].push_back(0);
        j["ip"].push_back(0xFF); j["ip"].push_back(0xFF);
        j["ip"].push_back(127); j["ip"].push_back(0); j["ip"].push_back(0); j["ip"].push_back(node_id % 255);
        root["anchors"].push_back(j);
    };
    add(2); add(3); add(4);
    {
        std::ofstream f(path);
        REQUIRE(f.is_open());
        f << root.dump(2);
    }

    CHECK(am.LoadAnchors(path));
    CHECK(attempts.size() == 2);
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("AnchorManager::LoadAnchors - invalid IP array -> reject and delete", "[unit][anchor]") {
    boost::asio::io_context io;
    AddressManager addrman;
    PeerManager peermgr(io, addrman);

    AnchorManager::AddressToStringCallback tostr = [](const protocol::NetworkAddress&){ return std::string("0.0.0.0"); };
    size_t calls = 0;
AnchorManager::ConnectCallback connect = [&](const protocol::NetworkAddress&, bool /*noban*/){ ++calls; };
    AnchorManager am(peermgr, tostr, connect);

    const std::string path = tmpfile("am_load_invalid.json");
    std::filesystem::remove(path);

    json root; root["version"] = 1; root["count"] = 1; root["anchors"] = json::array();
    json a; a["services"] = 1; a["port"] = protocol::ports::REGTEST + 2; a["ip"] = json::array();
    for (int i=0;i<15;++i) a["ip"].push_back(0); // invalid size
    root["anchors"].push_back(a);
    {
        std::ofstream f(path); REQUIRE(f.is_open()); f << root.dump(2);
    }

    CHECK_FALSE(am.LoadAnchors(path));
    CHECK(calls == 0);
    CHECK_FALSE(std::filesystem::exists(path));
}