// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_VALIDATION_CHAINSTATE_MANAGER_HPP
#define COINBASECHAIN_VALIDATION_CHAINSTATE_MANAGER_HPP

#include "primitives/block.h"
#include "validation/validation.hpp"
#include "validation/chain_selector.hpp"
#include "chain/block_manager.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <set>

namespace coinbasechain {

// Forward declarations
namespace chain {
    class ChainParams;
    class CBlockIndex;
}

namespace crypto {
    enum class POWVerifyMode;
}

namespace validation {

/**
 * ChainstateManager - High-level coordinator for blockchain state
 *
 * Responsibilities:
 * - Process new block headers (validate + accept)
 * - Activate best chain (tip selection)
 * - Emit notifications to subsystems
 * - Coordinate validation and block manager
 *
 * Simplified from Bitcoin Core's ChainstateManager:
 * - No multiple chainstates (no assumevalid, no snapshot)
 * - No UTXO set management (headers-only chain)
 * - No mempool coordination (no transactions)
 * - No background validation (single chainstate)
 *
 * This is the main entry point for adding blocks to the chain,
 * whether from mining or from the network.
 */
class ChainstateManager {
public:
    /**
     * Constructor
     *
     * @param params Chain parameters (must outlive this ChainstateManager instance)
     * @param suspicious_reorg_depth Maximum reorg depth before halting (0 = unlimited)
     *
     * LIFETIME REQUIREMENT: The ChainParams reference must remain valid for the
     * entire lifetime of this ChainstateManager. Typically, ChainParams is a global
     * or long-lived singleton, so this is safe. If ChainParams is destroyed before
     * ChainstateManager, undefined behavior will occur (dangling reference).
     */
    ChainstateManager(const chain::ChainParams& params,
                     int suspicious_reorg_depth = 100);

    /**
     * Accept a block header into the block index
     *
     * CRITICAL ANTI-DOS DESIGN:
     * - Cheap checks (commitment-only PoW) done BEFORE adding to index
     * - Block added to index BEFORE expensive validation (full PoW)
     * - If expensive validation fails, block marked BLOCK_FAILED_VALID
     * - Failed blocks added to m_failed_blocks for fast rejection
     * - This prevents attackers from forcing repeated expensive validation
     *
     * NEW: ORPHAN HANDLING (Bitcoin Core-style):
     * - If parent not found → header cached as orphan (instead of rejected)
     * - When parent arrives → orphan automatically processed
     * - DoS protection: limited orphan pool size, per-peer limits
     *
     * Steps:
     * 1. Check for duplicates (return existing if already known and valid)
     * 2. If already known and marked invalid, reject immediately
     * 3. Cheap PoW commitment check (anti-DoS, ALWAYS performed, rejects spam without caching)
     * 4. Check if genesis block (validate hash matches expected)
     * 5. Check parent exists and is valid
     *    → If parent NOT found: cache as orphan (if DoS limits allow)
     * 6. Check if descends from known invalid block
     * 7. Add to block index (BEFORE expensive validation!)
     * 8. Contextual checks (difficulty, timestamp, etc.)
     * 9. Full PoW verification (EXPENSIVE - result cached in index)
     * 10. If validation fails: mark BLOCK_FAILED_VALID + add to m_failed_blocks
     * 11. If validation succeeds: mark BLOCK_VALID_TREE + update best header
     * 12. Process any orphan children waiting for this parent
     *
     * @param header Block header to accept
     * @param state Validation state output
     * @param peer_id ID of peer that sent this header (for orphan DoS tracking, -1 = local)
     * @return Pointer to block index (or nullptr on failure/orphaned)
     *
     * @note The cheap PoW commitment check is UNCONDITIONAL - callers cannot bypass it.
     *       This prevents footguns where future code accidentally skips DoS protection.
     * @note If header is orphaned, returns nullptr but sets state to "orphaned" (not invalid)
     */
    chain::CBlockIndex* AcceptBlockHeader(const CBlockHeader& header,
                                          ValidationState& state,
                                          int peer_id = -1);

    /**
     * Process a new block header
     *
     * Steps:
     * 1. Accept header (via AcceptBlockHeader)
     * 2. Attempt to activate as new best chain
     * 3. Notify subsystems if chain tip changed
     *
     * @param header Block header to process
     * @param state Validation state output
     * @return true if accepted (even if not activated)
     */
    bool ProcessNewBlockHeader(const CBlockHeader& header,
                              ValidationState& state);

    /**
     * Activate best chain
     *
     * Finds the chain with most work and activates it as the active tip.
     * Emits notifications if the tip changed.
     *
     * @param pindexMostWork Block with most work (if nullptr, finds it)
     * @return true if successful
     */
    bool ActivateBestChain(chain::CBlockIndex* pindexMostWork = nullptr);

    /**
     * Get current active tip
     */
    const chain::CBlockIndex* GetTip() const;

    /**
     * Lookup a block index by hash (thread-safe)
     * @param hash Block hash to lookup
     * @return Block index pointer or nullptr if not found
     */
    chain::CBlockIndex* LookupBlockIndex(const uint256& hash);
    const chain::CBlockIndex* LookupBlockIndex(const uint256& hash) const;

    /**
     * Get block locator for a given block index (thread-safe)
     * @param pindex Block index to get locator from (nullptr = tip)
     * @return Block locator
     */
    CBlockLocator GetLocator(const chain::CBlockIndex* pindex = nullptr) const;

    /**
     * Check if a block is on the active chain (thread-safe)
     * @param pindex Block to check
     * @return true if on active chain
     */
    bool IsOnActiveChain(const chain::CBlockIndex* pindex) const;

    /**
     * Get block at specific height on active chain (thread-safe)
     * @param height Height to get block at
     * @return Block index or nullptr if height invalid
     */
    const chain::CBlockIndex* GetBlockAtHeight(int height) const;

    /**
     * Check if we're still in Initial Block Download
     *
     * Returns true if:
     * - No tip yet (just started)
     * - Tip is too old (more than 1 hour behind for 2-min blocks)
     * - Chain work is below minimum (optional, for eclipse attack protection)
     *
     * Once all conditions pass, this latches to false permanently.
     * This prevents flapping and improves performance after IBD completes.
     *
     * @return true if in IBD, false if fully synced
     */
    bool IsInitialBlockDownload() const;

    /**
     * Try to add a block to the candidate set (thread-safe)
     *
     * This method is used for batch header processing where headers are
     * accepted via AcceptBlockHeader() but chain activation is deferred.
     * The block is added as a candidate if it could be a valid chain tip.
     *
     * @param pindex Block to consider adding
     * @note This is called internally by ProcessNewBlockHeader, but can
     *       also be called explicitly for batch processing workflows
     */
    void TryAddBlockIndexCandidate(chain::CBlockIndex* pindex);

    /**
     * Initialize the blockchain with genesis block
     * @param genesis_header Genesis block header
     * @return true if successful
     */
    bool Initialize(const CBlockHeader& genesis_header);

    /**
     * Load blockchain state from disk
     * @param filepath Path to headers file
     * @return true if successful
     */
    bool Load(const std::string& filepath);

    /**
     * Save blockchain state to disk
     * @param filepath Path to headers file
     * @return true if successful
     */
    bool Save(const std::string& filepath) const;

    /**
     * Get total number of blocks in the index
     * @return Block count
     */
    size_t GetBlockCount() const;

    /**
     * Get current height of active chain
     * @return Chain height (-1 if no chain)
     */
    int GetChainHeight() const;

    /**
     * Evict old orphan headers to free memory
     * Called periodically to prevent unbounded growth
     * @return Number of orphans evicted
     */
    size_t EvictOrphanHeaders();

    /**
     * Get orphan header count for monitoring/debugging
     * @return Number of orphan headers currently cached
     */
    size_t GetOrphanHeaderCount() const;

    /**
     * Mark a block and all its descendants as invalid
     *
     * This is the implementation for the `invalidateblock` RPC command.
     * It marks the specified block as BLOCK_FAILED_VALID and all its
     * descendants as BLOCK_FAILED_CHILD, then reactivates the best valid chain.
     *
     * Use cases:
     * - Testing reorg scenarios
     * - Manually excluding bad blocks from the chain
     * - Debugging consensus issues
     *
     * @param hash Hash of block to invalidate
     * @return true if block was found and invalidated
     */
    bool InvalidateBlock(const uint256& hash);

    /**
     * Check Proof of Work for a batch of headers
     *
     * Uses the virtual CheckProofOfWork method, allowing test subclasses
     * to override PoW validation for the entire batch.
     *
     * @param headers Headers to validate
     * @return true if all headers have valid PoW
     */
    bool CheckHeadersPoW(const std::vector<CBlockHeader>& headers) const;

protected:
    /**
     * Check Proof of Work for a block header
     *
     * Virtual to allow test subclasses to override with mock validation.
     * Production code uses real RandomX PoW validation.
     *
     * @param header Block header to validate
     * @param mode Verification mode (COMMITMENT_ONLY or FULL)
     * @return true if PoW is valid
     */
    virtual bool CheckProofOfWork(const CBlockHeader& header,
                                   crypto::POWVerifyMode mode) const;

    /**
     * Check block header (non-contextual)
     *
     * Virtual to allow test subclasses to override validation.
     * Default implementation calls CheckBlockHeader from validation.cpp.
     *
     * @param header Block header to check
     * @param state Validation state output
     * @return true if valid
     */
    virtual bool CheckBlockHeaderWrapper(const CBlockHeader& header,
                                         ValidationState& state) const;

    /**
     * Check block header contextually
     *
     * Virtual to allow test subclasses to override validation.
     * Default implementation calls ContextualCheckBlockHeader from validation.cpp.
     *
     * @param header Block header to check
     * @param pindexPrev Previous block index
     * @param adjusted_time Current adjusted time
     * @param state Validation state output
     * @return true if valid
     */
    virtual bool ContextualCheckBlockHeaderWrapper(const CBlockHeader& header,
                                                    const chain::CBlockIndex* pindexPrev,
                                                    int64_t adjusted_time,
                                                    ValidationState& state) const;

private:
    /**
     * Connect a block to the active chain
     *
     * For headers-only, this just means updating the active chain state
     * and emitting notifications. No UTXO or transaction validation needed.
     *
     * @param pindexNew Block to connect
     * @return true if successful
     * @note Assumes validation_mutex_ is held by caller
     */
    bool ConnectTip(chain::CBlockIndex* pindexNew);

    /**
     * Disconnect the current tip from the active chain
     *
     * For headers-only, this just means moving the tip pointer back
     * to pprev and emitting notifications.
     *
     * @return true if successful
     * @note Assumes validation_mutex_ is held by caller
     */
    bool DisconnectTip();

    /**
     * Process orphan headers that were waiting for a specific parent
     *
     * When a new header is accepted, this function checks if any orphan headers
     * were waiting for it as their parent. If found, those orphans are recursively
     * processed (which may trigger more orphan processing).
     *
     * @param parentHash Hash of the newly accepted parent header
     * @note Assumes validation_mutex_ is held by caller
     */
    void ProcessOrphanHeaders(const uint256& parentHash);

    /**
     * Try to add an orphan header to the cache
     *
     * Checks DoS limits and evicts old orphans if needed before adding.
     *
     * @param header Orphan header to cache
     * @param peer_id ID of peer that sent this header (for DoS tracking)
     * @return true if added, false if rejected (DoS limit exceeded)
     * @note Assumes validation_mutex_ is held by caller
     */
    bool TryAddOrphanHeader(const CBlockHeader& header, int peer_id);

    chain::BlockManager block_manager_;
    ChainSelector chain_selector_;
    const chain::ChainParams& params_;
    int suspicious_reorg_depth_;

    /**
     * Orphan header storage
     *
     * Stores headers whose parent is not yet known. When the parent arrives,
     * the orphan is automatically processed via ProcessOrphanHeaders().
     *
     * Structure:
     * - header: The orphan block header
     * - nTimeReceived: When this orphan was first received (for eviction)
     * - peer_id: Which peer sent this (for DoS tracking and banning)
     */
    struct OrphanHeader {
        CBlockHeader header;
        int64_t nTimeReceived;
        int peer_id;
    };

    /**
     * Map of orphan headers: hash -> orphan data
     *
     * DoS Protection:
     * - Limited to MAX_ORPHAN_HEADERS total (prevents memory exhaustion)
     * - Time-based eviction (old orphans removed periodically)
     * - Per-peer limits enforced via m_peer_orphan_count
     *
     * @note Protected by validation_mutex_
     */
    std::map<uint256, OrphanHeader> m_orphan_headers;

    /**
     * Per-peer orphan count tracking
     *
     * Tracks how many orphans each peer has sent to detect DoS attacks.
     * If a peer exceeds MAX_ORPHAN_HEADERS_PER_PEER, new orphans are rejected.
     *
     * Map: peer_id -> number of orphans from that peer currently cached
     *
     * @note Protected by validation_mutex_
     */
    std::map<int, int> m_peer_orphan_count;

    // DoS protection limits
    static constexpr size_t MAX_ORPHAN_HEADERS = 1000;        // Total orphans across all peers
    static constexpr size_t MAX_ORPHAN_HEADERS_PER_PEER = 50; // Max orphans per peer
    static constexpr int64_t ORPHAN_HEADER_EXPIRE_TIME = 600; // 10 minutes in seconds

    /**
     * Set of block indices that have failed validation
     *
     * Purpose:
     * 1. Avoid reprocessing known invalid blocks (performance + DoS protection)
     * 2. Mark descendants as BLOCK_FAILED_CHILD (propagate invalidity)
     *
     * When a block fails validation:
     * - It's added to m_failed_blocks
     * - Its nStatus gets BLOCK_FAILED_VALID flag set
     *
     * When accepting a new header:
     * - Check if pprev descends from any block in m_failed_blocks
     * - If so, reject and mark all intermediate blocks as BLOCK_FAILED_CHILD
     *
     * This ensures the entire invalid subtree is marked, preventing wasted
     * validation effort and protecting against attacks using invalid chains.
     *
     * @note Protected by validation_mutex_
     */
    std::set<chain::CBlockIndex*> m_failed_blocks;

    /**
     * Cached IBD status (latches to false once IBD completes)
     * Using atomic for lock-free fast path in IsInitialBlockDownload()
     */
    mutable std::atomic<bool> m_cached_finished_ibd{false};

    /**
     * Mutex to serialize all validation operations
     *
     * PROTECTED DATA:
     * - block_manager_ (and its internal m_block_index map, m_active_chain)
     * - chain_selector_ (and its internal m_candidates, m_best_header)
     * - m_failed_blocks
     *
     * NOT PROTECTED (by design):
     * - m_cached_finished_ibd (std::atomic, lock-free)
     * - params_ (const reference, read-only)
     * - suspicious_reorg_depth_ (const after construction, read-only)
     *
     * LOCKING DISCIPLINE:
     * - ALL public methods acquire validation_mutex_ (std::lock_guard)
     * - Private methods assume mutex is already held by caller
     * - Methods like GetTip(), LookupBlockIndex() use mutable mutex (const correctness)
     * - Uses std::recursive_mutex to allow public methods to call other public methods
     *
     * WHY RECURSIVE MUTEX:
     * Some public methods need to call other public methods atomically:
     * - ProcessNewBlockHeader() calls AcceptBlockHeader() then ActivateBestChain()
     * - These operations must be atomic to prevent race conditions on m_candidates
     * - Alternative would be internal helper functions, but recursive mutex is simpler
     *
     * THREAD SAFETY GUARANTEES:
     * - Multiple threads can call public methods concurrently (serialized by mutex)
     * - BlockManager has no internal mutex (relies on ChainstateManager's mutex)
     * - ChainSelector has no internal mutex (relies on ChainstateManager's mutex)
     * - Iterator invalidation prevented (no concurrent modification of containers)
     * - Readers and writers both acquire exclusive lock (no std::shared_mutex yet)
     *
     * DEADLOCK PREVENTION:
     * - Never call external code while holding lock (notifications released before lock)
     * - No lock ordering issues (only one mutex in the system)
     * - Lock is always acquired at function entry, released at function exit (RAII)
     *
     * PERFORMANCE CONSIDERATION:
     * - IsInitialBlockDownload() uses atomic for fast path (avoids lock after IBD)
     * - Could use std::shared_mutex in future for read-heavy workloads
     *   (multiple readers, single writer) but std::mutex simpler for now
     */
    mutable std::recursive_mutex validation_mutex_;
};

} // namespace validation
} // namespace coinbasechain

#endif // COINBASECHAIN_VALIDATION_CHAINSTATE_MANAGER_HPP
