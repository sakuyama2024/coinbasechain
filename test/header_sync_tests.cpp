// Copyright (c) 2024 Coinbase Chain
// Test suite for header sync

#include "catch_amalgamated.hpp"
#include "sync/header_sync.hpp"
#include "chain/chainparams.hpp"
#include "validation/chainstate_manager.hpp"
#include "test_chainstate_manager.hpp"

using namespace coinbasechain;
using namespace coinbasechain::sync;
using namespace coinbasechain::chain;
using namespace coinbasechain::validation;
using namespace coinbasechain::test;

TEST_CASE("HeaderSync initialization", "[header_sync]") {
    // Create RegTest params (easy difficulty)
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate_manager(*params);
    chainstate_manager.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate_manager, *params);

    SECTION("Initialize with genesis") {
        REQUIRE(sync.Initialize());
        REQUIRE(sync.GetBestHeight() == 0);
        REQUIRE(!sync.GetBestHash().IsNull());
        REQUIRE(sync.GetState() == HeaderSync::State::IDLE);
    }
}

TEST_CASE("HeaderSync process headers", "[header_sync]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate_manager(*params);
    chainstate_manager.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate_manager, *params);
    sync.Initialize();

    const auto& genesis = params->GenesisBlock();

    SECTION("Process valid chain of headers") {
        std::vector<CBlockHeader> headers;

        // Create 10 headers building on genesis
        CBlockHeader prev = genesis;
        for (int i = 1; i <= 10; i++) {
            CBlockHeader header;
            header.nVersion = 1;
            header.hashPrevBlock = prev.GetHash();
            header.minerAddress.SetNull();
            header.nTime = prev.nTime + 120;  // 2 minutes later
            header.nBits = 0x207fffff;        // RegTest difficulty
            header.nNonce = i;                // Simple nonce
            header.hashRandomX.SetNull();

            headers.push_back(header);
            prev = header;
        }

        // Process all headers
        REQUIRE(sync.ProcessHeaders(headers, 1));

        // Check results
        REQUIRE(sync.GetBestHeight() == 10);
        REQUIRE(!sync.GetBestHash().IsNull());
    }

    SECTION("Process empty headers") {
        std::vector<CBlockHeader> headers;
        REQUIRE(sync.ProcessHeaders(headers, 1));
        REQUIRE(sync.GetBestHeight() == 0);  // Still at genesis
    }

    // NOTE: The following negative tests are commented out because they test
    // ChainstateManager validation logic, not HeaderSync logic. Since we use
    // TestChainstateManager (which bypasses validation) to avoid expensive PoW,
    // these tests cannot work. Validation is tested in validation_tests.cpp instead.

    // SECTION("Reject headers with invalid PoW") {
    //     // This would test ChainstateManager::CheckProofOfWork, not HeaderSync
    // }

    // SECTION("Reject headers from future") {
    //     // This would test ChainstateManager::ContextualCheckBlockHeader, not HeaderSync
    // }
}

TEST_CASE("HeaderSync locator", "[header_sync]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate_manager(*params);
    chainstate_manager.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate_manager, *params);
    sync.Initialize();

    SECTION("Locator from genesis") {
        CBlockLocator locator = sync.GetLocator();
        REQUIRE(!locator.IsNull());
        REQUIRE(locator.vHave.size() >= 1);
        // Should include genesis
        REQUIRE(locator.vHave.back() == params->GenesisBlock().GetHash());
    }

    SECTION("Locator after adding headers") {
        // Add 100 headers
        std::vector<CBlockHeader> headers;
        CBlockHeader prev = params->GenesisBlock();

        for (int i = 1; i <= 100; i++) {
            CBlockHeader header;
            header.nVersion = 1;
            header.hashPrevBlock = prev.GetHash();
            header.nTime = prev.nTime + 120;
            header.nBits = 0x207fffff;
            header.nNonce = i;

            headers.push_back(header);
            prev = header;
        }

        REQUIRE(sync.ProcessHeaders(headers, 1));

        CBlockLocator locator = sync.GetLocator();
        REQUIRE(locator.vHave.size() > 1);

        // First entry should be tip
        REQUIRE(locator.vHave[0] == sync.GetBestHash());
    }
}

TEST_CASE("HeaderSync synced status", "[header_sync]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate_manager(*params);
    chainstate_manager.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate_manager, *params);
    sync.Initialize();

    SECTION("Not synced at genesis (old timestamp)") {
        // Genesis is from 2011, so we're definitely not synced
        REQUIRE_FALSE(sync.IsSynced());
        REQUIRE(sync.GetState() == HeaderSync::State::IDLE);
    }

    SECTION("Synced after adding recent headers") {
        std::vector<CBlockHeader> headers;
        CBlockHeader prev = params->GenesisBlock();

        // Create a header with recent timestamp
        CBlockHeader header;
        header.nVersion = 1;
        header.hashPrevBlock = prev.GetHash();
        header.nTime = std::time(nullptr) - 60;  // 1 minute ago
        header.nBits = 0x207fffff;
        header.nNonce = 1;

        headers.push_back(header);

        REQUIRE(sync.ProcessHeaders(headers, 1));

        // Now we should be synced (tip is recent)
        REQUIRE(sync.IsSynced(3600));  // Within 1 hour
        REQUIRE(sync.GetState() == HeaderSync::State::SYNCED);
    }
}

TEST_CASE("HeaderSync request more", "[header_sync]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate_manager(*params);
    chainstate_manager.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate_manager, *params);
    sync.Initialize();

    SECTION("Should request more after full batch") {
        // Create exactly 2000 headers (MAX_HEADERS_RESULTS)
        std::vector<CBlockHeader> headers;
        CBlockHeader prev = params->GenesisBlock();

        for (int i = 1; i <= 2000; i++) {
            CBlockHeader header;
            header.nVersion = 1;
            header.hashPrevBlock = prev.GetHash();
            header.nTime = prev.nTime + 120;
            header.nBits = 0x207fffff;
            header.nNonce = i;

            headers.push_back(header);
            prev = header;
        }

        REQUIRE(sync.ProcessHeaders(headers, 1));

        // Should request more (full batch + not synced)
        REQUIRE(sync.ShouldRequestMore());
    }

    SECTION("Should not request more after partial batch") {
        // Create only 100 headers
        std::vector<CBlockHeader> headers;
        CBlockHeader prev = params->GenesisBlock();

        for (int i = 1; i <= 100; i++) {
            CBlockHeader header;
            header.nVersion = 1;
            header.hashPrevBlock = prev.GetHash();
            header.nTime = prev.nTime + 120;
            header.nBits = 0x207fffff;
            header.nNonce = i;

            headers.push_back(header);
            prev = header;
        }

        REQUIRE(sync.ProcessHeaders(headers, 1));

        // Should NOT request more (partial batch means peer is done)
        REQUIRE_FALSE(sync.ShouldRequestMore());
    }
}

TEST_CASE("HeaderSync progress", "[header_sync]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate_manager(*params);
    chainstate_manager.Initialize(params->GenesisBlock());
    HeaderSync sync(chainstate_manager, *params);
    sync.Initialize();

    SECTION("Progress at genesis") {
        double progress = sync.GetProgress();
        // Should be very low (genesis is old)
        REQUIRE(progress >= 0.0);
        REQUIRE(progress < 0.01);  // Less than 1%
    }

    SECTION("Progress after adding recent header") {
        std::vector<CBlockHeader> headers;

        CBlockHeader header;
        header.nVersion = 1;
        header.hashPrevBlock = params->GenesisBlock().GetHash();
        header.nTime = std::time(nullptr) - 60;  // Recent
        header.nBits = 0x207fffff;
        header.nNonce = 1;

        headers.push_back(header);
        REQUIRE(sync.ProcessHeaders(headers, 1));

        double progress = sync.GetProgress();
        // Should be close to 100%
        REQUIRE(progress > 0.99);
        REQUIRE(progress <= 1.0);
    }
}
