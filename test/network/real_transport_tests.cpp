#include "catch_amalgamated.hpp"
#include "network/real_transport.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

using namespace coinbasechain::network;

namespace {
// Pick an available high-range port; try a small range to avoid flakiness.
static uint16_t pick_listen_port(RealTransport& t,
                                 std::function<void(TransportConnectionPtr)> accept_cb,
                                 uint16_t start = 42000,
                                 uint16_t end = 42100) {
    for (uint16_t p = start; p < end; ++p) {
        if (t.listen(p, accept_cb)) return p;
    }
    return 0;
}
}

TEST_CASE("RealTransport lifecycle is idempotent", "[network][transport][real]") {
    RealTransport t(1);

    // Not running before run()
    CHECK_FALSE(t.is_running());

    // stop() without run() should be safe
    t.stop();

    // run() starts, second run() is no-op
    t.run();
    CHECK(t.is_running());
    t.run();
    CHECK(t.is_running());

    // stop() is idempotent
    t.stop();
    t.stop();

    // Can be started again after stop
    t.run();
    CHECK(t.is_running());
    t.stop();
}

TEST_CASE("RealTransport listen/connect echo roundtrip", "[network][transport][real]") {
    RealTransport server(1);
    RealTransport client(1);

    server.run();
    client.run();

    std::shared_ptr<TransportConnection> inbound_conn;
    std::mutex m;
    std::condition_variable cv;
    bool accepted = false;
    bool connected = false;
    bool echoed = false;

    auto accept_cb = [&](TransportConnectionPtr c){
        {
            std::lock_guard<std::mutex> lk(m);
            inbound_conn = c;
            accepted = true;
        }
        // Echo server: read and write back
        inbound_conn->set_receive_callback([&](const std::vector<uint8_t>& data){
            inbound_conn->send(data);
        });
        inbound_conn->start();
        cv.notify_all();
    };

    // Try to bind
    uint16_t port = pick_listen_port(server, accept_cb);
    REQUIRE(port != 0);

    // Connect client
    std::shared_ptr<TransportConnection> client_conn;
    client_conn = client.connect("127.0.0.1", port, [&](bool ok){
        {
            std::lock_guard<std::mutex> lk(m);
            connected = ok;
        }
        if (ok && client_conn) {
            client_conn->start();
        }
        cv.notify_all();
    });
    REQUIRE(client_conn);

    // Prepare to receive echo
    std::vector<uint8_t> received;
    client_conn->set_receive_callback([&](const std::vector<uint8_t>& data){
        {
            std::lock_guard<std::mutex> lk(m);
            received = data;
            echoed = true;
        }
        cv.notify_all();
    });

    // Wait for accept+connect
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(3), [&]{ return accepted && connected; });
    }
    REQUIRE(accepted);
    REQUIRE(connected);

    // Verify canonical remote addresses are non-empty and look like IPs
    CHECK(!client_conn->remote_address().empty());
    CHECK(!inbound_conn->remote_address().empty());

    // Send payload and expect echo
    const std::string payload = "hello";
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    CHECK(client_conn->send(bytes));

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(3), [&]{ return echoed; });
    }
    REQUIRE(echoed);
    std::string echoed_str(received.begin(), received.end());
    CHECK(echoed_str == payload);

    // Close and ensure further sends fail
    client_conn->close();
    CHECK_FALSE(client_conn->send(bytes));

    client.stop();
    server.stop();
}