// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_CHAIN_CHAINPARAMS_HPP
#define COINBASECHAIN_CHAIN_CHAINPARAMS_HPP

#include "chain/arith_uint256.hpp"
#include "chain/block.hpp"
#include "chain/uint.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace coinbasechain {
namespace chain {

/**
 * Chain type enumeration
 * Simple modern enum class (vs Bitcoins's ChainType)
 */
enum class ChainType {
  MAIN,    // Production mainnet
  TESTNET, // Public test network
  REGTEST  // Regression test (local testing)
};

/**
 * Consensus parameters
 * Simplified from Bitcoin's Consensus::Params
 */
struct ConsensusParams {
  // Proof of Work
  uint256 powLimit;               // Maximum difficulty (easiest target)
  int64_t nPowTargetSpacing{120}; // 2 minutes between blocks

  int64_t nRandomXEpochDuration{7 * 24 * 60 * 60}; // 1 week

  // ASERT difficulty adjustment
  int64_t nASERTHalfLife{2 * 24 * 60 * 60}; // 2 days (in seconds)

  // ASERT anchor block parameters
  struct ASERTAnchor {
    int32_t nHeight;        // Anchor block height
    uint32_t nBits;         // Anchor block difficulty
    int64_t nPrevBlockTime; // Parent block timestamp
  };

  // ASERT anchor block height
  // Set to 1 to use block 1 as anchor (block 0=genesis and block 1 both use
  // powLimit) This allows block 1 to be mined at any time without difficulty
  // adjustment issues
  int32_t nASERTAnchorHeight{1};

  // Hash of genesis block
  uint256 hashGenesisBlock;

  // Minimum cumulative chain work for IBD completion Set to 0 to disable check 
  // (regtest), or to actual chain work
  // (mainnet/testnet)
  uint256 nMinimumChainWork;
};

/**
 * ChainParams - Chain-specific parameters
 * Simplified  version of Bitcoin's CChainParams
 */
class ChainParams {
public:
  ChainParams() = default;
  virtual ~ChainParams() = default;

  // Accessors
  const ConsensusParams &GetConsensus() const { return consensus; }
  uint32_t GetNetworkMagic() const; // Returns protocol::magic::* constant for this chain
  uint16_t GetDefaultPort() const { return nDefaultPort; }
  const CBlockHeader &GenesisBlock() const { return genesis; }
  ChainType GetChainType() const { return chainType; }
  std::string GetChainTypeString() const;
  const std::vector<std::string> &FixedSeeds() const { return vFixedSeeds; }

  // Factory methods
  static std::unique_ptr<ChainParams> CreateMainNet();
  static std::unique_ptr<ChainParams> CreateTestNet();
  static std::unique_ptr<ChainParams> CreateRegTest();

protected:
  ConsensusParams consensus;
  uint16_t nDefaultPort{};
  ChainType chainType{ChainType::MAIN};
  CBlockHeader genesis;
  std::vector<std::string> vFixedSeeds;  // Hardcoded seed node addresses (IP:port)
};

/**
 * MainNet parameters
 */
class CMainParams : public ChainParams {
public:
  CMainParams();
};

/**
 * TestNet parameters
 */
class CTestNetParams : public ChainParams {
public:
  CTestNetParams();
};

/**
 * RegTest parameters
 */
class CRegTestParams : public ChainParams {
public:
  CRegTestParams();
};

/**
 * Global chain params singleton
 * Simple alternative to Bitcoin's global pointer
 */
class GlobalChainParams {
public:
  static void Select(ChainType chain);
  static const ChainParams &Get();
  static bool IsInitialized();

private:
  static std::unique_ptr<ChainParams> instance;
};

// Helper to create genesis block
CBlockHeader CreateGenesisBlock(uint32_t nTime, uint32_t nNonce, uint32_t nBits,
                                int32_t nVersion = 1);

} // namespace chain
} // namespace coinbasechain

#endif // COINBASECHAIN_CHAIN_CHAINPARAMS_HPP
