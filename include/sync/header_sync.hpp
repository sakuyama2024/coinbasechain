// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_SYNC_HEADER_SYNC_HPP
#define COINBASECHAIN_SYNC_HEADER_SYNC_HPP

#include "chain/chainparams.hpp"
#include "primitives/block.h"
#include <memory>
#include <cstdint>
#include <functional>
#include <mutex>

namespace coinbasechain {

// Forward declarations
namespace validation {
    class ChainstateManager;
}

namespace util {
    class ThreadPool;
}

namespace sync {

// Forward declaration
class PeerManager;

/**
 * HeaderSync - Simplified headers-only blockchain synchronization
 *
 * Key simplification vs Bitcoin:
 * - Single-pass sync (no presync/redownload)
 * - Headers = blocks (no separate block download)
 * - Fast sync (< 1 minute for millions of headers)
 *
 * See IBD_ANALYSIS.md for design rationale.
 *
 * THREAD SAFETY:
 * ---------------
 * HeaderSync is called from multiple io_context threads concurrently.
 * Internal state (state_, last_batch_size_, callback) is protected by mutex_.
 *
 * LOCKING ORDER (see LOCKING_ORDER.md):
 * - ChainstateManager::validation_mutex_ acquired FIRST (inside ProcessHeaders)
 * - HeaderSync::mutex_ acquired AFTER (for state updates)
 * - This mutex is held only for brief state reads/writes, not during validation
 */
class HeaderSync {
public:
    enum class State {
        IDLE,      // Not syncing
        SYNCING,   // Actively downloading headers
        SYNCED     // Caught up to network tip
    };

    /**
     * Constructor
     * @param chainstate_manager Reference to chainstate manager (handles validation)
     * @param params Chain parameters (for genesis, consensus rules)
     */
    HeaderSync(validation::ChainstateManager& chainstate_manager, const chain::ChainParams& params);
    ~HeaderSync();

    /**
     * Initialize with genesis block
     */
    bool Initialize();

    /**
     * Process received HEADERS message from peer
     * @param headers List of headers (up to 2000)
     * @param peer_id ID of peer who sent headers (for logging/banning)
     * @return true if successfully processed, false on error
     */
    bool ProcessHeaders(const std::vector<CBlockHeader>& headers, int peer_id);

    /**
     * Get block locator for GETHEADERS request
     * Used to tell peer where our chain is, so they know what to send
     */
    CBlockLocator GetLocator() const;

    /**
     * Get block locator starting from pprev of tip (for initial sync)
     * This ensures we get a non-empty response even if peer is at same tip
     * Matches Bitcoin's initial sync behavior 
     */
    CBlockLocator GetLocatorFromPrev() const;

    /**
     * Check if we're synced (tip is recent)
     * @param max_age_seconds Maximum age of tip to be considered synced (default: 1 hour)
     */
    bool IsSynced(int64_t max_age_seconds = 3600) const;

    /**
     * Get current sync state (thread-safe)
     */
    State GetState() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    /**
     * Get sync progress (0.0 to 1.0)
     * Estimates based on tip timestamp vs current time
     */
    double GetProgress() const;

    /**
     * Get best known header height
     */
    int GetBestHeight() const;

    /**
     * Get best known header hash
     */
    uint256 GetBestHash() const;

    /**
     * Should we request more headers from peer?
     * True if last batch was full (2000 headers) and we're not synced
     */
    bool ShouldRequestMore() const;

    /**
     * Callback when sync state changes
     * Used to notify application of sync progress (thread-safe)
     */
    void SetSyncStateCallback(std::function<void(State, int)> callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        sync_state_callback_ = callback;
    }

    /**
     * Get peer manager (for external peer management)
     */
    PeerManager& GetPeerManager() { return *peer_manager_; }
    const PeerManager& GetPeerManager() const { return *peer_manager_; }

private:
    /**
     * Update sync state
     */
    void UpdateState();

private:
    // Bitcoin protocol constants
    static constexpr size_t MAX_HEADERS_RESULTS = 2000;
    static constexpr int64_t MAX_FUTURE_BLOCK_TIME = 2 * 60 * 60;  // 2 hours

    // Chainstate manager (handles validation and chain state)
    validation::ChainstateManager& chainstate_manager_;

    // Chain parameters (consensus rules, genesis)
    const chain::ChainParams& params_;

    // Thread safety: protects state_, last_batch_size_, sync_state_callback_
    // Multiple io_context threads can call HeaderSync methods concurrently
    // See LOCKING_ORDER.md: Acquire ChainstateManager::validation_mutex_ BEFORE this
    mutable std::mutex mutex_;

    // Current sync state (GUARDED_BY mutex_)
    State state_;

    // How many headers in last batch received? (GUARDED_BY mutex_)
    size_t last_batch_size_;

    // Sync state change callback (GUARDED_BY mutex_)
    std::function<void(State, int)> sync_state_callback_;

    // Thread pool for parallel RandomX verification
    std::unique_ptr<util::ThreadPool> verification_pool_;

    // Peer manager for DoS protection (misbehavior tracking)
    std::unique_ptr<PeerManager> peer_manager_;
};

} // namespace sync
} // namespace coinbasechain

#endif // COINBASECHAIN_SYNC_HEADER_SYNC_HPP
