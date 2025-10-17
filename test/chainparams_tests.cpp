// Copyright (c) 2024 Coinbase Chain
// Test suite for chain parameters

#include "catch_amalgamated.hpp"
#include "chain/chainparams.hpp"

using namespace coinbasechain::chain;

TEST_CASE("ChainParams creation", "[chainparams]") {
    SECTION("Create MainNet") {
        auto params = ChainParams::CreateMainNet();
        REQUIRE(params != nullptr);
        REQUIRE(params->GetChainType() == ChainType::MAIN);
        REQUIRE(params->GetChainTypeString() == "main");
        REQUIRE(params->GetDefaultPort() == 9333);

        const auto& consensus = params->GetConsensus();
        REQUIRE(consensus.nPowTargetSpacing == 120);  // 2 minutes
        REQUIRE(consensus.nRandomXEpochDuration == 7 * 24 * 60 * 60);  // 1 week
    }

    SECTION("Create TestNet") {
        auto params = ChainParams::CreateTestNet();
        REQUIRE(params != nullptr);
        REQUIRE(params->GetChainType() == ChainType::TESTNET);
        REQUIRE(params->GetChainTypeString() == "test");
        REQUIRE(params->GetDefaultPort() == 19333);
    }

    SECTION("Create RegTest") {
        auto params = ChainParams::CreateRegTest();
        REQUIRE(params != nullptr);
        REQUIRE(params->GetChainType() == ChainType::REGTEST);
        REQUIRE(params->GetChainTypeString() == "regtest");
        REQUIRE(params->GetDefaultPort() == 29333);

        const auto& consensus = params->GetConsensus();
        // RegTest has easy difficulty for instant mining
    }
}

TEST_CASE("GlobalChainParams singleton", "[chainparams]") {
    SECTION("Select and get params") {
        // Select mainnet
        GlobalChainParams::Select(ChainType::MAIN);
        REQUIRE(GlobalChainParams::IsInitialized());

        const auto& params = GlobalChainParams::Get();
        REQUIRE(params.GetChainType() == ChainType::MAIN);

        // Switch to regtest
        GlobalChainParams::Select(ChainType::REGTEST);
        const auto& params2 = GlobalChainParams::Get();
        REQUIRE(params2.GetChainType() == ChainType::REGTEST);
    }
}

TEST_CASE("Genesis block creation", "[chainparams]") {
    auto params = ChainParams::CreateRegTest();
    const auto& genesis = params->GenesisBlock();

    SECTION("Genesis block properties") {
        REQUIRE(genesis.nVersion == 1);
        REQUIRE(genesis.hashPrevBlock.IsNull());
        REQUIRE(genesis.minerAddress.IsNull());
        REQUIRE(genesis.nTime > 0);
        REQUIRE(genesis.nBits > 0);
    }

    SECTION("Genesis hash") {
        uint256 hash = genesis.GetHash();
        REQUIRE(!hash.IsNull());

        const auto& consensus = params->GetConsensus();
        REQUIRE(consensus.hashGenesisBlock == hash);
    }
}

TEST_CASE("Network magic bytes", "[chainparams]") {
    SECTION("Different networks have different magic") {
        auto main = ChainParams::CreateMainNet();
        auto test = ChainParams::CreateTestNet();
        auto reg = ChainParams::CreateRegTest();

        const auto& mainMagic = main->MessageStart();
        const auto& testMagic = test->MessageStart();
        const auto& regMagic = reg->MessageStart();

        // All should be 4 bytes
        REQUIRE(mainMagic.size() == 4);
        REQUIRE(testMagic.size() == 4);
        REQUIRE(regMagic.size() == 4);

        // All should be different
        REQUIRE(mainMagic != testMagic);
        REQUIRE(mainMagic != regMagic);
        REQUIRE(testMagic != regMagic);
    }
}
