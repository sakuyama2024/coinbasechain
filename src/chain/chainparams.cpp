// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "chain/chainparams.hpp"
#include "chain/arith_uint256.hpp"
#include "network/protocol.hpp"
#include <cassert>
#include <stdexcept>

namespace coinbasechain {
namespace chain {

// Static instance
std::unique_ptr<ChainParams> GlobalChainParams::instance = nullptr;

CBlockHeader CreateGenesisBlock(uint32_t nTime, uint32_t nNonce, uint32_t nBits,
                                int32_t nVersion) {
  CBlockHeader genesis;
  genesis.nVersion = nVersion;
  genesis.hashPrevBlock.SetNull();
  genesis.minerAddress.SetNull();
  genesis.nTime = nTime;
  genesis.nBits = nBits;
  genesis.nNonce = nNonce;
  genesis.hashRandomX.SetNull();
  return genesis;
}

std::string ChainParams::GetChainTypeString() const {
  switch (chainType) {
  case ChainType::MAIN:
    return "main";
  case ChainType::TESTNET:
    return "test";
  case ChainType::REGTEST:
    return "regtest";
  }
  return "unknown";
}

uint32_t ChainParams::GetNetworkMagic() const {
  switch (chainType) {
  case ChainType::MAIN:
    return protocol::magic::MAINNET;
  case ChainType::TESTNET:
    return protocol::magic::TESTNET;
  case ChainType::REGTEST:
    return protocol::magic::REGTEST;
  }
  return 0;
}

std::unique_ptr<ChainParams> ChainParams::CreateMainNet() {
  return std::make_unique<CMainParams>();
}

std::unique_ptr<ChainParams> ChainParams::CreateTestNet() {
  return std::make_unique<CTestNetParams>();
}

std::unique_ptr<ChainParams> ChainParams::CreateRegTest() {
  return std::make_unique<CRegTestParams>();
}

// ============================================================================
// MainNet Parameters
// ============================================================================

CMainParams::CMainParams() {
  chainType = ChainType::MAIN;

  // Consensus rules
  consensus.powLimit = uint256S(
      "000fffff00000000000000000000000000000000000000000000000000000000");
  consensus.nPowTargetSpacing = 2 * 60;               // 2 minutes
  consensus.nRandomXEpochDuration = 7 * 24 * 60 * 60; // 1 week
  consensus.nASERTHalfLife = 2 * 24 * 60 * 60;        // 2 days

  // ASERT anchor: Use block 1 as the anchor
  // This means block 0 (genesis) and block 1 both use powLimit (easy to mine)
  // Block 2 onwards uses ASERT relative to block 1's actual timestamp
  // This eliminates timing issues - block 1 can be mined at any time
  consensus.nASERTAnchorHeight = 1;

  // Minimum chain work 
  // Update this value periodically as the chain grows
  // Set to 0 for now since this is a fresh chain with no accumulated work
  // Once mainnet has significant work, update this to ~90% of current chain
  // work
  consensus.nMinimumChainWork = uint256S(
      "0x0000000000000000000000000000000000000000000000000000000000000000");

  // Network configuration
  nDefaultPort = protocol::ports::MAINNET;

  // Genesis block:
  // Mined on: 2025-10-12
  // Block hash:
  // 36de9b76dcd7899a52bab783f185c2563884afb4c6ee9f3b20a51e13a284cfa7
  genesis =
      CreateGenesisBlock(1760292878, // nTime - Oct 12, 2025
                         633285,     // nNonce - found by genesis miner
                         0x1e270fd8, // nBits - initial difficulty
                         1           // nVersion
      );

  consensus.hashGenesisBlock = genesis.GetHash();
  assert(consensus.hashGenesisBlock ==
         uint256S("0x36de9b76dcd7899a52bab783f185c2563884afb4c6ee9f3b20a51e13a2"
                  "84cfa7"));

  // Network expiration disabled for mainnet (no forced updates)
  consensus.nNetworkExpirationInterval = 0;
  consensus.nNetworkExpirationGracePeriod = 0;

  // Hardcoded seed node addresses (ct20-ct26)
  // These are reliable seed nodes for initial peer discovery
  vFixedSeeds.push_back("178.18.251.16:9590");
  vFixedSeeds.push_back("185.225.233.49:9590");
  vFixedSeeds.push_back("207.244.248.15:9590");
  vFixedSeeds.push_back("194.140.197.98:9590");
  vFixedSeeds.push_back("173.212.251.205:9590");
  vFixedSeeds.push_back("144.126.138.46:9590");
  vFixedSeeds.push_back("194.163.184.29:9590");
}

// ============================================================================
// TestNet Parameters
// ============================================================================

CTestNetParams::CTestNetParams() {
  chainType = ChainType::TESTNET;

  // Very easy difficulty for fast testing (2 minute blocks)
  // Target: ~250 hashes per block on average
  consensus.powLimit = uint256S(
      "007fffff00000000000000000000000000000000000000000000000000000000");
  consensus.nPowTargetSpacing = 2 * 60; // 2 minutes (same as mainnet)
  consensus.nRandomXEpochDuration = 7 * 24 * 60 * 60; // 1 week
  consensus.nASERTHalfLife = 60 * 60; // 1 hour half-life for testing (30 blocks)

  // ASERT anchor: Use block 1 (same as mainnet)
  consensus.nASERTAnchorHeight = 1;

  // Minimum chain work (eclipse attack protection)
  // Set to 0 for fresh testnet - update as the network grows
  consensus.nMinimumChainWork = uint256S(
      "0x0000000000000000000000000000000000000000000000000000000000000000");

  // Network expiration enabled for testnet - forces updates every 3 months
  // 1 hour blocks × 24 hours × 90 days = 2,160 blocks
  consensus.nNetworkExpirationInterval = 2160;
  consensus.nNetworkExpirationGracePeriod = 24;  // 1 day warning period

  // Network configuration
  nDefaultPort = protocol::ports::TESTNET;

  // Testnet genesis - mined at Oct 15, 2025
  // Block hash:
  // cb608755c4b2bee0b929fe5760dec6cc578b48976ee164bb06eb9597c17575f8
  genesis = CreateGenesisBlock(1760549555, // Oct 15, 2025
                               253,        // Nonce found by genesis miner
                               0x1f7fffff, // Easy difficulty for fast testing
                               1);

  consensus.hashGenesisBlock = genesis.GetHash();
  assert(consensus.hashGenesisBlock ==
         uint256S("0xcb608755c4b2bee0b929fe5760dec6cc578b48976ee164bb06eb9597c1"
                  "7575f8"));

  // Hardcoded seed node addresses (ct20-ct26)
  // These are reliable seed nodes for initial peer discovery
  vFixedSeeds.push_back("178.18.251.16:19590");
  vFixedSeeds.push_back("185.225.233.49:19590");
  vFixedSeeds.push_back("207.244.248.15:19590");
  vFixedSeeds.push_back("194.140.197.98:19590");
  vFixedSeeds.push_back("173.212.251.205:19590");
  vFixedSeeds.push_back("144.126.138.46:19590");
  vFixedSeeds.push_back("194.163.184.29:19590");
}

// ============================================================================
// RegTest Parameters (Local testing)
// ============================================================================

CRegTestParams::CRegTestParams() {
  chainType = ChainType::REGTEST;

  // Very easy difficulty - instant block generation
  consensus.powLimit = uint256S(
      "7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
  consensus.nPowTargetSpacing = 2 * 60;
  consensus.nRandomXEpochDuration =
      365ULL * 24 * 60 * 60 * 100; // 100 year (so all regtest blocks stay in same epoch)

  // Minimum chain work
  // Disabled for regtest - we want to generate chains from scratch
  consensus.nMinimumChainWork = uint256S(
      "0x0000000000000000000000000000000000000000000000000000000000000000");

  // Network expiration disabled for regtest (testing environment)
  consensus.nNetworkExpirationInterval = 0;  // Disabled
  consensus.nNetworkExpirationGracePeriod = 0;  // Disabled

  // Network configuration
  nDefaultPort = protocol::ports::REGTEST;

  // Regtest genesis - instant mine
  genesis = CreateGenesisBlock(1296688602, // Just use a fixed time
                               2,          // Easy nonce
                               0x207fffff, // Very easy difficulty
                               1);

  consensus.hashGenesisBlock = genesis.GetHash();
  assert(consensus.hashGenesisBlock ==
         uint256S("0x0233b37bb6942bfb471cfd7fb95caab0e0f7b19cc8767da65fbef59eb4"
                  "9e45bd"));

  vFixedSeeds.clear();  // No hardcoded seeds for local testing
}

// ============================================================================
// Global Params Singleton
// ============================================================================

void GlobalChainParams::Select(ChainType chain) {
  switch (chain) {
  case ChainType::MAIN:
    instance = ChainParams::CreateMainNet();
    break;
  case ChainType::TESTNET:
    instance = ChainParams::CreateTestNet();
    break;
  case ChainType::REGTEST:
    instance = ChainParams::CreateRegTest();
    break;
  }
}

const ChainParams &GlobalChainParams::Get() {
  if (!instance) {
    throw std::runtime_error(
        "GlobalChainParams not initialized - call Select() first");
  }
  return *instance;
}

bool GlobalChainParams::IsInitialized() { return instance != nullptr; }

} // namespace chain
} // namespace coinbasechain
