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

// HeaderSync - Simplified headers-only blockchain synchronization
// Single-pass sync (no presync/redownload), headers = blocks, fast sync
// See IBD_ANALYSIS.md for design rationale
//
// THREAD SAFETY: Called from multiple io_context threads concurrently
// Internal state protected by mutex_ (held only for brief reads/writes, not during validation)
// LOCKING ORDER: ChainstateManager::validation_mutex_ FIRST, HeaderSync::mutex_ AFTER
class HeaderSync {
public:
    enum class State {
        IDLE,      // Not syncing
        SYNCING,   // Actively downloading headers
        SYNCED     // Caught up to network tip
    };

    HeaderSync(validation::ChainstateManager& chainstate_manager, const chain::ChainParams& params);
    ~HeaderSync();

    bool Initialize();

    // Process HEADERS message from peer (returns true if successfully processed)
    bool ProcessHeaders(const std::vector<CBlockHeader>& headers, int peer_id);

    // Get block locator for GETHEADERS request (tells peer where our chain is)
    CBlockLocator GetLocator() const;

    // Get block locator from pprev of tip (ensures non-empty response even if peer at same tip)
    CBlockLocator GetLocatorFromPrev() const;

    // Check if synced (tip is recent, default max_age_seconds = 1 hour)
    bool IsSynced(int64_t max_age_seconds = 3600) const;

    State GetState() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    // Get sync progress (0.0 to 1.0, estimated from tip timestamp vs current time)
    double GetProgress() const;

    int GetBestHeight() const;
    uint256 GetBestHash() const;

    // Should we request more headers? (true if last batch was full and we're not synced)
    bool ShouldRequestMore() const;

    void SetSyncStateCallback(std::function<void(State, int)> callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        sync_state_callback_ = callback;
    }

    PeerManager& GetPeerManager() { return *peer_manager_; }
    const PeerManager& GetPeerManager() const { return *peer_manager_; }

private:
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
