// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "sync/header_sync.hpp"
#include "sync/peer_manager.hpp"
#include "validation/chainstate_manager.hpp"
#include "validation/validation.hpp"
#include "chain/block_manager.hpp"
#include "util/threadpool.hpp"
#include "util/logging.hpp"
#include "crypto/randomx_pow.hpp"
#include "arith_uint256.h"
#include <ctime>
#include <randomx.h>

namespace coinbasechain {
namespace sync {

HeaderSync::HeaderSync(validation::ChainstateManager& chainstate_manager, const chain::ChainParams& params)
    : chainstate_manager_(chainstate_manager)
    , params_(params)
    , state_(State::IDLE)
    , last_batch_size_(0)
    , peer_manager_(std::make_unique<PeerManager>())
{
    LOG_INFO("HeaderSync initialized (sequential mode) with peer misbehavior tracking");
}

HeaderSync::~HeaderSync() = default;

bool HeaderSync::Initialize()
{
    // ChainstateManager should already be initialized by Application
    // Just update our state
    UpdateState();
    return true;
}

bool HeaderSync::ProcessHeaders(const std::vector<CBlockHeader>& headers, int peer_id)
{
    if (headers.empty()) {
        LOG_INFO("HeaderSync: Received empty headers from peer {}", peer_id);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_batch_size_ = 0;
        }
        UpdateState();
        return true;
    }

    // DoS Protection: Check headers message size limit
    if (headers.size() > validation::MAX_HEADERS_RESULTS) {
        LOG_ERROR("HeaderSync: Rejecting oversized headers message from peer {} (size={}, max={})",
                  peer_id, headers.size(), validation::MAX_HEADERS_RESULTS);
        peer_manager_->Misbehaving(peer_id, MisbehaviorPenalty::OVERSIZED_MESSAGE,
                                   "oversized headers message");
        return false;
    }

    LOG_INFO("HeaderSync: Processing {} headers from peer {}", headers.size(), peer_id);

    // DoS Protection: Check if first header connects to known chain
    const uint256& first_prev = headers[0].hashPrevBlock;
    bool prev_exists = chainstate_manager_.LookupBlockIndex(first_prev) != nullptr;

    if (!prev_exists) {
        LOG_WARN("HeaderSync: Headers don't connect to known chain from peer {} (first header prevhash: {})",
                 peer_id, first_prev.ToString());

        // Increment unconnecting counter
        if (peer_manager_->IncrementUnconnectingHeaders(peer_id)) {
            // Threshold exceeded - penalize
            peer_manager_->Misbehaving(peer_id, MisbehaviorPenalty::TOO_MANY_UNCONNECTING,
                                       "too many unconnecting headers messages");
        }
        return false;
    }

    // Headers connect - reset unconnecting counter
    peer_manager_->ResetUnconnectingHeaders(peer_id);

    // DoS Protection: Cheap PoW commitment check (before expensive full validation)
    // This is CRITICAL for DoS protection:
    // - Rejects entire batch BEFORE adding to block index
    // - Uses ChainstateManager's virtual CheckHeadersPoW
    // - Tests can override via TestChainstateManager
    bool pow_ok = chainstate_manager_.CheckHeadersPoW(headers);
    if (!pow_ok) {
        LOG_ERROR("HeaderSync: Headers failed PoW commitment check from peer {}", peer_id);
        peer_manager_->Misbehaving(peer_id, MisbehaviorPenalty::INVALID_POW,
                                   "header with invalid proof of work");
        return false;
    }

    // DoS Protection: Check headers are continuous
    bool continuous_ok = validation::CheckHeadersAreContinuous(headers);
    if (!continuous_ok) {
        LOG_ERROR("HeaderSync: Non-continuous headers from peer {}", peer_id);
        peer_manager_->Misbehaving(peer_id, MisbehaviorPenalty::NON_CONTINUOUS_HEADERS,
                                   "non-continuous headers sequence");
        return false;
    }

    // DoS Protection: Anti-DoS work threshold (only enforced after IBD)
    if (!chainstate_manager_.IsInitialBlockDownload()) {
        const chain::CBlockIndex* tip = chainstate_manager_.GetTip();
        arith_uint256 threshold = validation::GetAntiDoSWorkThreshold(tip, params_, false);
        arith_uint256 headers_work = validation::CalculateHeadersWork(headers);

        if (headers_work < threshold) {
            LOG_WARN("HeaderSync: Rejecting low-work headers from peer {} (work={}, threshold={})",
                     peer_id, headers_work.ToString().substr(0, 16), threshold.ToString().substr(0, 16));
            peer_manager_->Misbehaving(peer_id, MisbehaviorPenalty::LOW_WORK_HEADERS,
                                       "low-work header spam");
            return false;
        }
    }

    // Store batch size under lock
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_batch_size_ = headers.size();
    }

    // Capture the original tip BEFORE processing any headers (Bitcoin-style)
    // This ensures we calculate reorg depth against the starting point, not incrementally
    const chain::CBlockIndex* pindexOriginalTip = chainstate_manager_.GetTip();

    // Accept all headers into block index WITHOUT activating
    // (Similar to Bitcoin's approach where headers are validated but activation is deferred)
    for (const auto& header : headers) {
        validation::ValidationState state;
        chain::CBlockIndex* pindex = chainstate_manager_.AcceptBlockHeader(header, state, peer_id);

        if (!pindex) {
            const std::string& reason = state.GetRejectReason();

            // Check if header was orphaned (not an error - just missing parent)
            // Bitcoin: BLOCK_MISSING_PREV - not punished, we don't have the parent yet
            if (reason == "orphaned") {
                LOG_INFO("HeaderSync: Header from peer {} cached as orphan: {}",
                        peer_id, header.GetHash().ToString().substr(0, 16));
                // Continue processing rest of batch - orphan may be resolved later
                continue;
            }

            // DoS Protection: Orphan limit exceeded
            // Bitcoin: Would be tracked separately, but we treat excessive orphans as DoS
            if (reason == "orphan-limit") {
                LOG_WARN("HeaderSync: Peer {} exceeded orphan limit", peer_id);
                peer_manager_->Misbehaving(peer_id, MisbehaviorPenalty::TOO_MANY_ORPHANS,
                                           "exceeded orphan header limit");
                return false;
            }

            // Invalid header - instant disconnect (Bitcoin: BLOCK_INVALID_HEADER = 100 points)
            // Covers: "high-hash" (PoW failed), "bad-diffbits", "time-too-old", "time-too-new", "bad-version"
            if (reason == "high-hash" || reason == "bad-diffbits" ||
                reason == "time-too-old" || reason == "time-too-new" || reason == "bad-version") {
                LOG_ERROR("HeaderSync: Peer {} sent invalid header: {}", peer_id, reason);
                peer_manager_->Misbehaving(peer_id, MisbehaviorPenalty::INVALID_HEADER,
                                           "invalid header: " + reason);
                return false;
            }

            // Duplicate/cached invalid - instant disconnect (Bitcoin: BLOCK_CACHED_INVALID = 100 points)
            if (reason == "duplicate") {
                LOG_WARN("HeaderSync: Peer {} sent duplicate header marked invalid", peer_id);
                peer_manager_->Misbehaving(peer_id, MisbehaviorPenalty::INVALID_HEADER,
                                           "duplicate header marked as invalid");
                return false;
            }

            // Invalid parent - instant disconnect (Bitcoin: BLOCK_INVALID_PREV = 100 points)
            if (reason == "bad-prevblk") {
                LOG_ERROR("HeaderSync: Peer {} sent header with invalid parent", peer_id);
                peer_manager_->Misbehaving(peer_id, MisbehaviorPenalty::INVALID_HEADER,
                                           "header references invalid parent");
                return false;
            }

            // Genesis block errors - likely configuration issue or malicious
            if (reason == "bad-genesis" || reason == "genesis-via-accept") {
                LOG_ERROR("HeaderSync: Peer {} sent invalid genesis: {}", peer_id, reason);
                peer_manager_->Misbehaving(peer_id, MisbehaviorPenalty::INVALID_HEADER,
                                           "invalid genesis block");
                return false;
            }

            // Unknown rejection reason - log and fail (don't punish in case it's our bug)
            LOG_ERROR("HeaderSync: Failed to accept header from peer {} - Hash: {}, Reason: {}, Debug: {}",
                      peer_id, header.GetHash().ToString(), reason, state.GetDebugMessage());
            return false;
        }

        // Add to candidate set for batch activation later
        chainstate_manager_.TryAddBlockIndexCandidate(pindex);
    }

    // NOW activate best chain ONCE for the entire batch
    // The reorg check will see the full depth from pindexOriginalTip
    LOG_INFO("HeaderSync: Calling ActivateBestChain for batch of {} headers", headers.size());
    validation::ValidationState state;
    bool activate_result = chainstate_manager_.ActivateBestChain(nullptr);
    LOG_INFO("HeaderSync: ActivateBestChain returned {}", activate_result ? "true" : "FALSE");
    if (!activate_result) {
        return false;
    }

    // Show different messages during IBD vs normal operation
    if (chainstate_manager_.IsInitialBlockDownload()) {
        // During IBD: show progress percentage like Bitcoin
        double progress = GetProgress();
        LOG_INFO("Synchronizing block headers, height: {} (~{:.2f}%)", GetBestHeight(), progress * 100.0);
    } else {
        // After IBD: show simple new block notification
        LOG_INFO("New block header: height={}, hash={}...", GetBestHeight(), GetBestHash().ToString().substr(0, 16));
    }

    UpdateState();
    return true;
}

CBlockLocator HeaderSync::GetLocator() const
{
    return chainstate_manager_.GetLocator();
}

CBlockLocator HeaderSync::GetLocatorFromPrev() const
{
    // Matches Bitcoin's initial sync logic 
    // Start from pprev of tip to ensure non-empty response
    const chain::CBlockIndex* tip = chainstate_manager_.GetTip();
    if (!tip) {
        // At genesis, just use tip
        return chainstate_manager_.GetLocator();
    }

    if (tip->pprev) {
        // Use pprev - ensures peer sends back at least 1 header (our current tip)
        return chainstate_manager_.GetLocator(tip->pprev);
    } else {
        // Tip is genesis (no pprev), use tip itself
        return chainstate_manager_.GetLocator();
    }
}

bool HeaderSync::IsSynced(int64_t max_age_seconds) const
{
    const chain::CBlockIndex* tip = chainstate_manager_.GetTip();
    if (!tip) {
        return false;
    }

    // Check if tip is recent
    int64_t now = std::time(nullptr);
    int64_t tip_age = now - tip->nTime;

    return tip_age < max_age_seconds;
}

double HeaderSync::GetProgress() const
{
    const chain::CBlockIndex* tip = chainstate_manager_.GetTip();
    if (!tip) {
        return 0.0;
    }

    int64_t now = std::time(nullptr);
    int64_t tip_time = tip->nTime;

    // Estimate: if blocks are every 2 minutes, how far behind are we?
    int64_t time_behind = now - tip_time;
    if (time_behind <= 0) {
        return 1.0;  // Fully synced
    }

    // Rough estimate: assume we started from genesis at some point
    // This is just for display purposes
    int64_t genesis_time = params_.GenesisBlock().nTime;
    int64_t total_time = now - genesis_time;
    int64_t synced_time = tip_time - genesis_time;

    if (total_time <= 0) {
        return 1.0;
    }

    double progress = (double)synced_time / (double)total_time;
    return std::min(1.0, std::max(0.0, progress));
}

int HeaderSync::GetBestHeight() const
{
    const chain::CBlockIndex* tip = chainstate_manager_.GetTip();
    return tip ? tip->nHeight : -1;
}

uint256 HeaderSync::GetBestHash() const
{
    const chain::CBlockIndex* tip = chainstate_manager_.GetTip();
    return tip ? tip->GetBlockHash() : uint256();
}

bool HeaderSync::ShouldRequestMore() const
{
    // Request more if:
    // 1. Last batch was full (peer might have more)
    // 2. We're not synced yet
    std::lock_guard<std::mutex> lock(mutex_);
    return last_batch_size_ == MAX_HEADERS_RESULTS && !IsSynced();
}

void HeaderSync::UpdateState()
{
    std::lock_guard<std::mutex> lock(mutex_);

    State old_state = state_;

    if (IsSynced()) {
        state_ = State::SYNCED;
    } else if (last_batch_size_ > 0) {
        // We're syncing if we've received headers from peers
        state_ = State::SYNCING;
    } else {
        // Not synced and no recent batch = idle (e.g., at genesis)
        state_ = State::IDLE;
    }

    // Notify callback if state changed (call with mutex held to avoid race on callback itself)
    if (state_ != old_state && sync_state_callback_) {
        sync_state_callback_(state_, GetBestHeight());
    }
}

} // namespace sync
} // namespace coinbasechain
