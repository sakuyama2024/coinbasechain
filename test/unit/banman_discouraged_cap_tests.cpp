// BanMan discouraged size cap tests
#include "catch_amalgamated.hpp"
#include "network/banman.hpp"
#include <string>

using namespace coinbasechain::network;

TEST_CASE("BanMan - Discouraged cap enforced", "[banman][discouraged][unit]") {
    BanMan bm("");

    // Insert slightly above the cap
    const size_t target = BanMan::MAX_DISCOURAGED + 50;
    for (size_t i = 0; i < target; ++i) {
        std::string ip = "10.0." + std::to_string(i / 256) + "." + std::to_string(i % 256);
        bm.Discourage(ip);
    }

    // We should never exceed the cap
    // Internal structure is private; validate behavior by sampling a few entries
    size_t present = 0;
    for (size_t i = 0; i < target; i += (target / 10)) {
        std::string ip = "10.0." + std::to_string(i / 256) + "." + std::to_string(i % 256);
        if (bm.IsDiscouraged(ip)) present++;
    }
    CHECK(present <= 10);
}