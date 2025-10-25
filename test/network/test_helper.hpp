#ifndef COINBASECHAIN_TEST2_HELPER_HPP
#define COINBASECHAIN_TEST2_HELPER_HPP

#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "infra/attack_simulated_node.hpp"
#include "chain/chainparams.hpp"
#include <catch_amalgamated.hpp>

namespace coinbasechain {
namespace test {

static struct Test2Setup {
    Test2Setup() {
        chain::GlobalChainParams::Select(chain::ChainType::REGTEST);
    }
} test2_setup;

} // namespace test
} // namespace coinbasechain

#endif
