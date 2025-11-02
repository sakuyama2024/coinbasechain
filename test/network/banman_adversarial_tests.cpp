// BanMan adversarial tests (ported to test2)

#include "catch_amalgamated.hpp"

using namespace coinbasechain::network;

TEST_CASE("BanMan Adversarial - Ban Evasion", "[adversarial][banman][critical]") {
    BanMan banman("");
    SECTION("Different ports same IP") { banman.Ban("192.168.1.100:8333", 3600); REQUIRE(banman.IsBanned("192.168.1.100:8333")); REQUIRE_FALSE(banman.IsBanned("192.168.1.100:8334")); }
    SECTION("IPv4 vs IPv6 localhost") { banman.Ban("127.0.0.1", 3600); REQUIRE(banman.IsBanned("127.0.0.1")); REQUIRE_FALSE(banman.IsBanned("::1")); }
}

TEST_CASE("BanMan Adversarial - Ban List Limits", "[adversarial][banman][dos]") {
    BanMan banman("");
    SECTION("Ban 100 different IPs (scaled down)") { for (int i=0;i<100;i++){ banman.Ban("10.0.0."+std::to_string(i), 3600);} REQUIRE(banman.IsBanned("10.0.0.0")); REQUIRE(banman.IsBanned("10.0.0.99")); REQUIRE(banman.GetBanned().size()==100); }
    SECTION("Discourage 100 different IPs") { for(int i=0;i<100;i++){ banman.Discourage("10.0.0."+std::to_string(i)); } REQUIRE(banman.IsDiscouraged("10.0.0.0")); REQUIRE(banman.IsDiscouraged("10.0.0.99")); }
}

TEST_CASE("BanMan Adversarial - Time Manipulation", "[adversarial][banman][timing]") {
    BanMan banman("");
    SECTION("Permanent ban (offset = 0)") { banman.Ban("192.168.1.1", 0); REQUIRE(banman.IsBanned("192.168.1.1")); }
    SECTION("Negative offset (ban in past)") { banman.Ban("192.168.1.2", -100); (void)banman.IsBanned("192.168.1.2"); }
}

TEST_CASE("BanMan Adversarial - Edge Cases", "[adversarial][banman][edge]") {
    BanMan banman("");
    SECTION("Empty address string") { banman.Ban("", 3600); REQUIRE(banman.IsBanned("")); banman.Unban(""); REQUIRE_FALSE(banman.IsBanned("")); }
    SECTION("Very long address") { std::string long_addr(1000,'A'); banman.Ban(long_addr, 3600); REQUIRE(banman.IsBanned(long_addr)); }
    SECTION("Special characters") { std::string s = "192.168.1.1\n\t\r\"'\\"; banman.Ban(s,3600); REQUIRE(banman.IsBanned(s)); }
}

TEST_CASE("BanMan Adversarial - Duplicate Operations", "[adversarial][banman][idempotent]") {
    BanMan banman("");
    SECTION("Ban same address twice") { banman.Ban("192.168.1.1", 3600); banman.Ban("192.168.1.1", 7200); REQUIRE(banman.IsBanned("192.168.1.1")); REQUIRE(banman.GetBanned().size()==1); }
    SECTION("Unban non-existent") { banman.Unban("192.168.1.1"); REQUIRE_FALSE(banman.IsBanned("192.168.1.1")); }
    SECTION("Discourage twice") { banman.Discourage("192.168.1.1"); banman.Discourage("192.168.1.1"); REQUIRE(banman.IsDiscouraged("192.168.1.1")); }
}

TEST_CASE("BanMan Adversarial - Ban vs Discourage", "[adversarial][banman][interaction]") {
    BanMan banman("");
    SECTION("Ban AND discourage same address") { banman.Ban("192.168.1.1", 3600); banman.Discourage("192.168.1.1"); REQUIRE(banman.IsBanned("192.168.1.1")); REQUIRE(banman.IsDiscouraged("192.168.1.1")); }
    SECTION("Unban discouraged address") { banman.Ban("192.168.1.1", 3600); banman.Discourage("192.168.1.1"); banman.Unban("192.168.1.1"); REQUIRE_FALSE(banman.IsBanned("192.168.1.1")); REQUIRE(banman.IsDiscouraged("192.168.1.1")); }
    SECTION("Clear bans vs discouraged") { banman.Ban("192.168.1.1", 3600); banman.Discourage("192.168.1.1"); banman.ClearBanned(); REQUIRE_FALSE(banman.IsBanned("192.168.1.1")); REQUIRE(banman.IsDiscouraged("192.168.1.1")); }
    SECTION("Clear discouraged vs bans") { banman.Ban("192.168.1.1", 3600); banman.Discourage("192.168.1.1"); banman.ClearDiscouraged(); REQUIRE(banman.IsBanned("192.168.1.1")); REQUIRE_FALSE(banman.IsDiscouraged("192.168.1.1")); }
}

TEST_CASE("BanMan Adversarial - Sweep Operation", "[adversarial][banman][sweep]") {
    BanMan banman("");
    SECTION("Sweep removes only expired (no-crash)") { banman.Ban("192.168.1.1",3600); banman.Ban("192.168.1.2",3600); banman.SweepBanned(); REQUIRE(banman.IsBanned("192.168.1.1")); REQUIRE(banman.IsBanned("192.168.1.2")); }
}
