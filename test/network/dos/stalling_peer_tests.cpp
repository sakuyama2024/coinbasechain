// DoS: Stalling peer timeout test

#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "infra/attack_simulated_node.hpp"
#include "test_orchestrator.hpp"
#include "network_observer.hpp"
#include "chain/chainparams.hpp"

using namespace coinbasechain;
using namespace coinbasechain::chain;
using namespace coinbasechain::test;

static struct TestSetup {
    TestSetup() { chain::GlobalChainParams::Select(chain::ChainType::REGTEST); }
} test_setup_stall;

TEST_CASE("DoS: Stalling peer timeout", "[dos][network]") {
    // Test that victim doesn't hang when attacker stalls responses

    SimulatedNetwork network(999);
    TestOrchestrator orchestrator(&network);
    NetworkObserver observer;
    AutoDumpOnFailure auto_dump(observer);

    SimulatedNode victim(1, &network);
    AttackSimulatedNode attacker(2, &network);

    observer.OnCustomEvent("TEST_START", -1, "Stalling peer timeout test");

    // Setup chain
    victim.SetBypassPOWValidation(true);
    for (int i = 0; i < 10; i++) {
        victim.MineBlock();
    }

    attacker.ConnectTo(1);
    REQUIRE(orchestrator.WaitForConnection(victim, attacker));
    REQUIRE(orchestrator.WaitForSync(victim, attacker));

    // Enable stalling: Attacker won't respond to GETHEADERS
    observer.OnCustomEvent("PHASE", -1, "Enabling stall mode");
    attacker.EnableStalling(true);

    // Send orphan headers to trigger GETHEADERS request
    observer.OnCustomEvent("PHASE", -1, "Sending orphans to trigger GETHEADERS");
    attacker.SendOrphanHeaders(1, 50);

    // Victim will request parents, but attacker stalls
    observer.OnCustomEvent("PHASE", -1, "Waiting for timeout (victim should not hang)");
    orchestrator.AdvanceTime(std::chrono::seconds(5));

    // Verify: Victim should still be functional (didn't hang)
    orchestrator.AssertHeight(victim, 10);

    // Attacker may be disconnected for stalling (implementation specific)
    observer.OnCustomEvent("TEST_END", -1, "PASSED - Victim survived stall attack");
    auto_dump.MarkSuccess();
}
