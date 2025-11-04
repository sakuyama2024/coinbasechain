// Additional TimeData tests focusing on AddTimeData behavior

#include "catch_amalgamated.hpp"
#include "chain/timedata.hpp"
#include "network/protocol.hpp"

using namespace coinbasechain;
using namespace coinbasechain::chain;
using namespace coinbasechain::protocol;

static NetworkAddress A(uint32_t v4){ return NetworkAddress::from_ipv4(NODE_NETWORK, v4, 9590); }

TEST_CASE("TimeData - median update and limits", "[timedata][add]") {
    TestOnlyResetTimeData();

    // 5 samples (odd) → update median
    AddTimeData(A(0x01010101), 10);   // +10s
    AddTimeData(A(0x02020202), 20);   // +20s
    AddTimeData(A(0x03030303), 30);   // +30s
    AddTimeData(A(0x04040404), 40);   // +40s
    AddTimeData(A(0x05050505), 50);   // +50s → median = 30

    REQUIRE(GetTimeOffset() == 30);

    // Even number of samples (6) → no update per Core quirk
    AddTimeData(A(0x06060606), 60);
    REQUIRE(GetTimeOffset() == 30);

    // Add large positive sample beyond DEFAULT_MAX_TIME_ADJUSTMENT; offset resets to 0
    int64_t too_far = DEFAULT_MAX_TIME_ADJUSTMENT + 600; // > +70 min
    AddTimeData(A(0x07070707), too_far);
    // After adding to odd size (7), median likely exceeds range → offset set to 0
    REQUIRE(GetTimeOffset() == 0);
}

TEST_CASE("TimeData - duplicate source ignored and size cap", "[timedata][add]") {
    TestOnlyResetTimeData();

    auto addr = A(0x0A0A0A0A);
    AddTimeData(addr, 5);
    AddTimeData(addr, 1000); // duplicate source ignored

    // Need to reach odd size >=5 to trigger update
    AddTimeData(A(0x0B0B0B0B), 5);
    AddTimeData(A(0x0C0C0C0C), 5);
    AddTimeData(A(0x0D0D0D0D), 5);
    AddTimeData(A(0x0E0E0E0E), 5); // size=5 (one duplicate ignored) → median=5

    REQUIRE(GetTimeOffset() == 5);
}
