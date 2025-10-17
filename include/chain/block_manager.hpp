// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_CHAIN_BLOCK_MANAGER_HPP
#define COINBASECHAIN_CHAIN_BLOCK_MANAGER_HPP

#include "chain/block_index.hpp"
#include "chain/chain.hpp"
#include "primitives/block.h"
#include <map>
#include <string>
#include <memory>

namespace coinbasechain {
namespace chain {

/**
 * BlockManager - Manages all known block headers and the active chain
 *
 * Simplified from Bitcoin Core's BlockManager for headers-only chain.
 *
 * Key responsibilities:
 * - Store ALL known block headers (including forks, orphans)
 * - Maintain the active chain (best known chain)
 * - Provide lookups by hash and height
 * - Persist headers to disk
 *
 * Simplifications:
 * - No LevelDB (use simple JSON/binary file)
 * - No block files (blk*.dat, rev*.dat)
 * - No UTXO tracking
 * - No transaction index
 * - No block pruning
 * - All headers kept in memory
 *
 * Memory usage: ~120 bytes × num_headers
 * - 1M headers = ~120 MB
 * - 10M headers = ~1.2 GB (easily fits in RAM)
 *
 * THREAD SAFETY:
 * ---------------
 * BlockManager does NOT have its own mutex. Thread safety is provided by
 * ChainstateManager::validation_mutex_, which MUST be held when calling
 * any BlockManager method.
 *
 * This design is similar to Bitcoin Core, where a global mutex (cs_main)
 * protects the BlockMap. In our case, we use a member mutex (validation_mutex_).
 *
 * BlockManager is a PRIVATE member of ChainstateManager, so all access
 * goes through ChainstateManager methods which acquire validation_mutex_.
 *
 */

class BlockManager {
public:
    BlockManager();
    ~BlockManager();

    /**
     * Initialize with genesis block
     */
    bool Initialize(const CBlockHeader& genesis);

    /**
     * Look up a block by hash
     * Returns nullptr if not found
     */
    CBlockIndex* LookupBlockIndex(const uint256& hash);
    const CBlockIndex* LookupBlockIndex(const uint256& hash) const;

    /**
     * Add a new block header to the index
     *
     * Steps:
     * 1. Check if already exists
     * 2. Create new CBlockIndex
     * 3. Set parent pointer (pprev)
     * 4. Calculate height and chain work
     * 5. Insert into map
     *
     * Returns pointer to CBlockIndex (existing or new)
     */
    CBlockIndex* AddToBlockIndex(const CBlockHeader& header);

    /**
     * Get the active chain
     */
    CChain& ActiveChain() { return m_active_chain; }
    const CChain& ActiveChain() const { return m_active_chain; }

    /**
     * Get the current tip of the active chain
     */
    CBlockIndex* GetTip() { return m_active_chain.Tip(); }
    const CBlockIndex* GetTip() const { return m_active_chain.Tip(); }

    /**
     * Set a new tip for the active chain
     * This populates the entire vChain vector by walking backwards
     */
    void SetActiveTip(CBlockIndex& block)
    {
        m_active_chain.SetTip(block);
    }

    /**
     * Get total number of known blocks
     */
    size_t GetBlockCount() const { return m_block_index.size(); }

    /**
     * Get read-only access to block index for iteration
     *
     * Used by ChainstateManager to verify leaf nodes when adding candidates.
     * This allows checking if a block has children before adding it to the
     * candidate set.
     *
     * @return const reference to the block index map
     */
    const std::map<uint256, CBlockIndex>& GetBlockIndex() const { return m_block_index; }

    /**
     * Save all headers to disk
     * Format: Simple JSON or binary file
     */
    bool Save(const std::string& filepath) const;

    /**
     * Load headers from disk
     * Reconstructs entire block index and active chain
     *
     * @param filepath Path to headers file
     * @param expected_genesis_hash Expected genesis block hash (for validation)
     * @return true if loaded successfully and genesis matches
     */
    bool Load(const std::string& filepath, const uint256& expected_genesis_hash);

private:
    /**
     * Map of all known blocks: hash -> CBlockIndex
     * The map OWNS the CBlockIndex objects.
     * The keys (uint256 hashes) are what phashBlock points to.
     */
    std::map<uint256, CBlockIndex> m_block_index;

    /**
     * The active (best) chain
     * Points to CBlockIndex objects owned by m_block_index
     */
    CChain m_active_chain;

    /**
     * Genesis block hash (for validation)
     */
    uint256 m_genesis_hash;

    /**
     * Has Initialize() been called?
     */
    bool m_initialized{false};
};

} // namespace chain
} // namespace coinbasechain

#endif // COINBASECHAIN_CHAIN_BLOCK_MANAGER_HPP
