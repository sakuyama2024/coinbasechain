// BanMan whitelist (NoBan) tests
#include "catch_amalgamated.hpp"
#include "network/banman.hpp"
#include <filesystem>
#include <thread>

using namespace coinbasechain::network;

TEST_CASE("BanMan - Localhost not whitelisted by default", "[banman][whitelist][unit]") {
    BanMan bm("");

    // By default, localhost is NOT whitelisted; banning should work
    bm.Ban("127.0.0.1", 3600);
    CHECK(bm.IsBanned("127.0.0.1"));

    // ::1 behaves the same
    bm.Ban("::1", 3600);
    CHECK(bm.IsBanned("::1"));
}

TEST_CASE("BanMan - AddToWhitelist removes existing ban and discouragement", "[banman][whitelist][unit]") {
    BanMan bm("");

    // Create ban and discouragement
    bm.Ban("10.0.0.1", 3600);
    bm.Discourage("10.0.0.1");

    REQUIRE(bm.IsBanned("10.0.0.1"));
    REQUIRE(bm.IsDiscouraged("10.0.0.1"));

    // Whitelist the address; should clear both
    bm.AddToWhitelist("10.0.0.1");
    CHECK_FALSE(bm.IsBanned("10.0.0.1"));
    CHECK_FALSE(bm.IsDiscouraged("10.0.0.1"));

    // Further attempts to ban/discourage should be ignored
    bm.Ban("10.0.0.1", 3600);
    bm.Discourage("10.0.0.1");
    CHECK_FALSE(bm.IsBanned("10.0.0.1"));
    CHECK_FALSE(bm.IsDiscouraged("10.0.0.1"));

    // Remove from whitelist; banning should work again
    bm.RemoveFromWhitelist("10.0.0.1");
    bm.Ban("10.0.0.1", 1);
    CHECK(bm.IsBanned("10.0.0.1"));
}

TEST_CASE("BanMan - Whitelist ban removal persists when autosave enabled", "[banman][whitelist][persistence][unit]") {
    // Use temp dir
    const std::string dir = "/tmp/banman_whitelist_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        BanMan bm(dir, true); // auto-save on
        bm.Ban("10.0.0.2", 3600);
        REQUIRE(bm.IsBanned("10.0.0.2"));
        bm.AddToWhitelist("10.0.0.2");
        CHECK_FALSE(bm.IsBanned("10.0.0.2"));
        // Destructor will Save()
    }

    // New instance should not see the ban
    {
        BanMan bm2(dir, false);
        REQUIRE(bm2.Load());
        CHECK_FALSE(bm2.IsBanned("10.0.0.2"));
    }

    std::filesystem::remove_all(dir);
}