// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "chain/block_manager.hpp"
#include "chain/arith_uint256.hpp"
#include "chain/logging.hpp"
#include <fstream>
#include <nlohmann/json.hpp>

namespace coinbasechain {
namespace chain {

BlockManager::BlockManager() = default;
BlockManager::~BlockManager() = default;

bool BlockManager::Initialize(const CBlockHeader &genesis) {
  LOG_CHAIN_TRACE("Initialize: called with genesis hash={}",
                  genesis.GetHash().ToString().substr(0, 16));

  if (m_initialized) {
    LOG_CHAIN_ERROR("BlockManager already initialized");
    return false;
  }

  // Add genesis block
  CBlockIndex *pindex = AddToBlockIndex(genesis);
  if (!pindex) {
    LOG_CHAIN_ERROR("Failed to add genesis block");
    return false;
  }

  // Set as active tip
  m_active_chain.SetTip(*pindex);

  // Remember genesis hash
  m_genesis_hash = genesis.GetHash();
  m_initialized = true;

  LOG_CHAIN_TRACE("BlockManager initialized with genesis: {}",
                 m_genesis_hash.ToString());

  return true;
}

CBlockIndex *BlockManager::LookupBlockIndex(const uint256 &hash) {
  auto it = m_block_index.find(hash);
  if (it == m_block_index.end())
    return nullptr;
  return &it->second;
}

const CBlockIndex *BlockManager::LookupBlockIndex(const uint256 &hash) const {
  auto it = m_block_index.find(hash);
  if (it == m_block_index.end())
    return nullptr;
  return &it->second;
}

CBlockIndex *BlockManager::AddToBlockIndex(const CBlockHeader &header) {
  uint256 hash = header.GetHash();

  LOG_CHAIN_TRACE("AddToBlockIndex: hash={} prev={}",
                  hash.ToString().substr(0, 16),
                  header.hashPrevBlock.ToString().substr(0, 16));

  // Already have it?
  auto it = m_block_index.find(hash);
  if (it != m_block_index.end()) {
    LOG_CHAIN_TRACE("AddToBlockIndex: Block already exists, returning existing index");
    return &it->second;
  }

  // Create new entry (use try_emplace to construct CBlockIndex in-place)
  auto [iter, inserted] = m_block_index.try_emplace(hash, header);
  if (!inserted) {
    LOG_CHAIN_ERROR("Failed to insert block {}", hash.ToString());
    return nullptr;
  }

  CBlockIndex *pindex = &iter->second;

  // Set hash pointer (points to the map's key)
  pindex->phashBlock = &iter->first;

  // Connect to parent
  pindex->pprev = LookupBlockIndex(header.hashPrevBlock);

  // CRITICAL: Set nHeight and nChainWork ONCE during block creation
  // These fields are used by ChainSelector's CBlockIndexWorkComparator for
  // std::set ordering and MUST remain immutable after the block is added
  // to any sorted container (e.g., candidate set).
  //
  // NEVER modify these fields after this point! Doing so would violate
  // std::set invariants and cause undefined behavior (broken ordering,
  // failed lookups, crashes).
  //
  // If you need to modify these fields for any reason (e.g., revalidation),
  // you MUST:
  // 1. Remove the block from all ChainSelector candidate sets
  // 2. Modify the fields
  // 3. Re-add the block to candidate sets if applicable
  if (pindex->pprev) {
    // Not genesis - calculate height and chain work
    pindex->nHeight = pindex->pprev->nHeight + 1;
    pindex->nChainWork = pindex->pprev->nChainWork + GetBlockProof(*pindex);
    LOG_CHAIN_TRACE("AddToBlockIndex: Created new block index height={} log2_work={:.6f}",
                    pindex->nHeight, std::log(pindex->nChainWork.getdouble()) / std::log(2.0));
  } else {
    // Genesis block
    pindex->nHeight = 0;
    pindex->nChainWork = GetBlockProof(*pindex);
    LOG_CHAIN_TRACE("AddToBlockIndex: Created GENESIS block index log2_work={:.6f}",
                    std::log(pindex->nChainWork.getdouble()) / std::log(2.0));
  }

  return pindex;
}

bool BlockManager::Save(const std::string &filepath) const {
  using json = nlohmann::json;

  try {
    LOG_CHAIN_TRACE("Saving {} headers to {}", m_block_index.size(), filepath);

    json root;
    root["version"] = 1; // Format version for future compatibility
    root["block_count"] = m_block_index.size();

    // Save tip hash
    if (m_active_chain.Tip()) {
      root["tip_hash"] = m_active_chain.Tip()->GetBlockHash().ToString();
    } else {
      root["tip_hash"] = "";
    }

    // Save genesis hash
    root["genesis_hash"] = m_genesis_hash.ToString();

    // Save all blocks
    json blocks = json::array();
    for (const auto &[hash, block_index] : m_block_index) {
      json block_data;

      // Block hash
      block_data["hash"] = hash.ToString();

      // Header fields
      block_data["version"] = block_index.nVersion;
      block_data["miner_address"] = block_index.minerAddress.ToString();
      block_data["time"] = block_index.nTime;
      block_data["bits"] = block_index.nBits;
      block_data["nonce"] = block_index.nNonce;
      block_data["hash_randomx"] = block_index.hashRandomX.ToString();

      // Chain metadata
      block_data["height"] = block_index.nHeight;
      block_data["chainwork"] = block_index.nChainWork.GetHex();
      block_data["status"] = block_index.nStatus;

      // Previous block hash (for reconstruction)
      if (block_index.pprev) {
        block_data["prev_hash"] = block_index.pprev->GetBlockHash().ToString();
      } else {
        block_data["prev_hash"] = uint256().ToString(); // Genesis has null prev
      }

      blocks.push_back(block_data);
    }

    root["blocks"] = blocks;

    // Write to file
    std::ofstream file(filepath);
    if (!file.is_open()) {
      LOG_CHAIN_ERROR("Failed to open file for writing: {}", filepath);
      return false;
    }

    file << root.dump(2); // Pretty print with 2-space indent
    file.close();

    LOG_CHAIN_TRACE("Successfully saved {} headers", m_block_index.size());
    return true;

  } catch (const std::exception &e) {
    LOG_CHAIN_ERROR("Exception during Save: {}", e.what());
    return false;
  }
}

bool BlockManager::Load(const std::string &filepath,
                        const uint256 &expected_genesis_hash) {
  using json = nlohmann::json;

  try {
    LOG_CHAIN_TRACE("Loading headers from {}", filepath);

    // Open file
    std::ifstream file(filepath);
    if (!file.is_open()) {
      LOG_CHAIN_TRACE("Header file not found: {} (starting fresh)", filepath);
      return false;
    }

    // Parse JSON
    json root;
    file >> root;
    file.close();

    // Validate format version
    int version = root.value("version", 0);
    if (version != 1) {
      LOG_CHAIN_ERROR("Unsupported header file version: {}", version);
      return false;
    }

    size_t block_count = root.value("block_count", 0);
    std::string genesis_hash_str = root.value("genesis_hash", "");
    std::string tip_hash_str = root.value("tip_hash", "");

    LOG_CHAIN_TRACE("Loading {} headers, genesis: {}, tip: {}", block_count,
                   genesis_hash_str, tip_hash_str);

    // CRITICAL: Validate genesis block hash matches expected network
    uint256 loaded_genesis_hash;
    loaded_genesis_hash.SetHex(genesis_hash_str);
    if (loaded_genesis_hash != expected_genesis_hash) {
      LOG_CHAIN_ERROR("GENESIS MISMATCH: Loaded genesis {} does not match "
                      "expected genesis {}",
                      genesis_hash_str, expected_genesis_hash.ToString());
      LOG_CHAIN_ERROR(
          "This datadir contains headers from a different network!");
      LOG_CHAIN_ERROR(
          "Please delete the headers file or use a different datadir.");
      return false;
    }

    LOG_CHAIN_TRACE("Genesis block validation passed: {}", genesis_hash_str);

    // Clear existing state
    m_block_index.clear();
    m_active_chain.Clear();

    // Load all blocks
    const json &blocks = root["blocks"];

    // First pass: Create all CBlockIndex objects (without connecting pprev)
    std::map<uint256, std::pair<CBlockIndex *, uint256>>
        block_map; // hash -> (pindex, prev_hash)

    for (const auto &block_data : blocks) {
      // Parse block data
      uint256 hash;
      hash.SetHex(block_data["hash"].get<std::string>());

      uint256 prev_hash;
      prev_hash.SetHex(block_data["prev_hash"].get<std::string>());

      // Create header
      CBlockHeader header;
      header.nVersion = block_data["version"].get<int32_t>();
      header.minerAddress.SetHex(
          block_data["miner_address"].get<std::string>());
      header.nTime = block_data["time"].get<uint32_t>();
      header.nBits = block_data["bits"].get<uint32_t>();
      header.nNonce = block_data["nonce"].get<uint32_t>();
      header.hashRandomX.SetHex(block_data["hash_randomx"].get<std::string>());
      header.hashPrevBlock = prev_hash;

      // Add to block index (use try_emplace to construct CBlockIndex in-place)
      auto [iter, inserted] = m_block_index.try_emplace(hash, header);
      if (!inserted) {
        LOG_CHAIN_ERROR("Duplicate block in header file: {}", hash.ToString());
        return false;
      }

      CBlockIndex *pindex = &iter->second;
      pindex->phashBlock = &iter->first;

      // Restore metadata
      pindex->nHeight = block_data["height"].get<int>();
      pindex->nChainWork.SetHex(block_data["chainwork"].get<std::string>());
      pindex->nStatus = block_data["status"].get<uint32_t>();

      // Store for second pass
      block_map[hash] = {pindex, prev_hash};
    }

    // Second pass: Connect pprev pointers
    for (auto &[hash, data] : block_map) {
      CBlockIndex *pindex = data.first;
      const uint256 &prev_hash = data.second;

      if (!prev_hash.IsNull()) {
        pindex->pprev = LookupBlockIndex(prev_hash);
        if (!pindex->pprev) {
          LOG_CHAIN_ERROR("Parent block not found for {}: {}", hash.ToString(),
                          prev_hash.ToString());
          return false;
        }
      } else {
        pindex->pprev = nullptr; // Genesis
      }
    }

    // Restore genesis hash
    m_genesis_hash.SetHex(genesis_hash_str);

    // Restore active chain tip
    if (!tip_hash_str.empty()) {
      uint256 tip_hash;
      tip_hash.SetHex(tip_hash_str);
      CBlockIndex *tip = LookupBlockIndex(tip_hash);
      if (!tip) {
        LOG_CHAIN_ERROR("Tip block not found: {}", tip_hash_str);
        return false;
      }
      m_active_chain.SetTip(*tip);
      LOG_CHAIN_TRACE("Restored active chain to height {}", tip->nHeight);
    }

    m_initialized = true;
    LOG_CHAIN_TRACE("Successfully loaded {} headers", m_block_index.size());
    return true;

  } catch (const std::exception &e) {
    LOG_CHAIN_ERROR("Exception during Load: {}", e.what());
    m_block_index.clear();
    m_active_chain.Clear();
    m_initialized = false;
    return false;
  }
}

} // namespace chain
} // namespace coinbasechain
