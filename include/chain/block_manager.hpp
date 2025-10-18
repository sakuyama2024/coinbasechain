// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_CHAIN_BLOCK_MANAGER_HPP
#define COINBASECHAIN_CHAIN_BLOCK_MANAGER_HPP

#include "chain/block_index.hpp"
#include "chain/chain.hpp"
#include "primitives/block.h"
#include <map>
#include <memory>
#include <string>

namespace coinbasechain {
namespace chain {

// BlockManager - Manages all known block headers and the active chain
// Simplified from Bitcoin Core for headers-only chain (no LevelDB, block files,
// UTXO, tx index, or pruning) All headers kept in memory (~120 bytes each: 1M
// headers = ~120 MB, 10M headers = ~1.2 GB)
//
// THREAD SAFETY: NO internal mutex - caller MUST hold
// ChainstateManager::validation_mutex_ BlockManager is PRIVATE member of
// ChainstateManager, all access goes through ChainstateManager

class BlockManager {
public:
  BlockManager();
  ~BlockManager();

  bool Initialize(const CBlockHeader &genesis);

  // Look up block by hash (returns nullptr if not found)
  CBlockIndex *LookupBlockIndex(const uint256 &hash);
  const CBlockIndex *LookupBlockIndex(const uint256 &hash) const;

  // Add new block header to index (returns pointer to CBlockIndex, existing or
  // new) Creates CBlockIndex, sets parent pointer, calculates height and chain
  // work
  CBlockIndex *AddToBlockIndex(const CBlockHeader &header);

  CChain &ActiveChain() { return m_active_chain; }
  const CChain &ActiveChain() const { return m_active_chain; }

  CBlockIndex *GetTip() { return m_active_chain.Tip(); }
  const CBlockIndex *GetTip() const { return m_active_chain.Tip(); }

  // Set new tip for active chain (populates entire vChain vector by walking
  // backwards)
  void SetActiveTip(CBlockIndex &block) { m_active_chain.SetTip(block); }

  size_t GetBlockCount() const { return m_block_index.size(); }

  // Read-only access to block index (for checking if block has children)
  const std::map<uint256, CBlockIndex> &GetBlockIndex() const {
    return m_block_index;
  }

  bool Save(const std::string &filepath) const;

  // Load headers from disk (reconstructs block index and active chain)
  // Returns true if loaded successfully and genesis matches
  bool Load(const std::string &filepath, const uint256 &expected_genesis_hash);

private:
  // Map of all known blocks: hash -> CBlockIndex (map owns CBlockIndex objects,
  // keys are what phashBlock points to)
  std::map<uint256, CBlockIndex> m_block_index;

  // Active (best) chain (points to CBlockIndex objects owned by m_block_index)
  CChain m_active_chain;

  uint256 m_genesis_hash; // Genesis block hash (for validation)
  bool m_initialized{false};
};

} // namespace chain
} // namespace coinbasechain

#endif // COINBASECHAIN_CHAIN_BLOCK_MANAGER_HPP
