// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "validation/chainstate_manager.hpp"
#include "validation/validation.hpp"
#include "chain/block_manager.hpp"
#include "chain/chainparams.hpp"
#include "crypto/randomx_pow.hpp"
#include "consensus/pow.hpp"
#include "notifications.hpp"
#include "util/logging.hpp"
#include <cassert>
#include <ctime>
#include <iostream>

namespace coinbasechain {
namespace validation {

// Default suspicious reorg depth (matches Bitcoin Core's COINBASE_MATURITY)
static constexpr int SUSPICIOUS_REORG_DEPTH = 100;  // Halt if reorg exceeds this

ChainstateManager::ChainstateManager(const chain::ChainParams& params,
                                     int suspicious_reorg_depth)
    : block_manager_()
    , params_(params)
    , suspicious_reorg_depth_(suspicious_reorg_depth)
{
}

chain::CBlockIndex* ChainstateManager::AcceptBlockHeader(const CBlockHeader& header,
                                                         ValidationState& state,
                                                         int peer_id)
{
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);

    uint256 hash = header.GetHash();

    // Step 1: Check for duplicate
    chain::CBlockIndex* pindex = block_manager_.LookupBlockIndex(hash);
    if (pindex) {
        // Block header is already known
        if (pindex->nStatus & chain::BLOCK_FAILED_MASK) {
            LOG_DEBUG("Block header {} is marked invalid", hash.ToString().substr(0, 16));
            state.Invalid("duplicate", "block is marked invalid");
            return nullptr;
        }
        // Already have it and it's valid
        return pindex;
    }

    // Step 2: Cheap POW commitment check (anti-DoS)
    if (!CheckProofOfWork(header, crypto::POWVerifyMode::COMMITMENT_ONLY)) {
        state.Invalid("high-hash", "proof of work commitment failed");
        LOG_DEBUG("Block header {} failed POW commitment check", hash.ToString().substr(0, 16));
        return nullptr;
    }

    // Step 3: Check if this is a genesis block (validate hash matches expected)
    if (header.hashPrevBlock.IsNull()) {
        // This claims to be a genesis block
        if (hash != params_.GetConsensus().hashGenesisBlock) {
            state.Invalid("bad-genesis", "genesis block hash mismatch");
            LOG_ERROR("Rejected fake genesis block: {} (expected: {})",
                     hash.ToString(),
                     params_.GetConsensus().hashGenesisBlock.ToString());
            return nullptr;
        }
        // Valid genesis, but must be added via Initialize()
        state.Invalid("genesis-via-accept", "genesis block must be added via Initialize()");
        return nullptr;
    }

    // Step 4: Check if parent exists
    chain::CBlockIndex* pindexPrev = block_manager_.LookupBlockIndex(header.hashPrevBlock);
    if (!pindexPrev) {
        // Parent not found - this is an ORPHAN header
        LOG_DEBUG("Orphan header {}: parent {} not found",
                 hash.ToString().substr(0, 16),
                 header.hashPrevBlock.ToString().substr(0, 16));

        // Try to cache as orphan (Bitcoin Core-style)
        if (TryAddOrphanHeader(header, peer_id)) {
            LOG_INFO("Cached orphan header: hash={}, parent={}, peer={}",
                    hash.ToString().substr(0, 16),
                    header.hashPrevBlock.ToString().substr(0, 16),
                    peer_id);
            state.Invalid("orphaned", "header cached as orphan (parent not found)");
        } else {
            LOG_WARN("Failed to cache orphan header {} (DoS limit exceeded)",
                    hash.ToString().substr(0, 16));
            state.Invalid("orphan-limit", "orphan pool full or peer limit exceeded");
        }
        return nullptr;
    }

    // Step 5: Check if parent is marked invalid
    if (pindexPrev->nStatus & chain::BLOCK_FAILED_MASK) {
        LOG_DEBUG("Block header {} has prev block invalid: {}",
                 hash.ToString().substr(0, 16),
                 header.hashPrevBlock.ToString().substr(0, 16));
        state.Invalid("bad-prevblk", "previous block is invalid");
        return nullptr;
    }

    // Step 6: Check if descends from any known invalid block
    if (!pindexPrev->IsValid(chain::BLOCK_VALID_TREE)) {
        // Check against failed blocks list
        for (chain::CBlockIndex* failedit : m_failed_blocks) {
            if (pindexPrev->GetAncestor(failedit->nHeight) == failedit) {
                // This block descends from a known invalid block
                // Mark all blocks between pindexPrev and failedit as BLOCK_FAILED_CHILD
                chain::CBlockIndex* invalid_walk = pindexPrev;
                while (invalid_walk != failedit) {
                    invalid_walk->nStatus |= chain::BLOCK_FAILED_CHILD;
                    invalid_walk = invalid_walk->pprev;
                }
                LOG_DEBUG("Block header {} has prev block that descends from invalid block",
                         hash.ToString().substr(0, 16));
                state.Invalid("bad-prevblk", "previous block descends from invalid block");
                return nullptr;
            }
        }
    }

    // Step 7: Add to block index BEFORE expensive validation
    // This ensures we cache the result of expensive operations (PoW verification)
    // If block fails validation below, we mark it invalid and never re-validate
    pindex = block_manager_.AddToBlockIndex(header);
    if (!pindex) {
        state.Error("failed to add block to index");
        return nullptr;
    }

    // Step 8: Contextual check
    int64_t adjusted_time = GetAdjustedTime();
    if (!ContextualCheckBlockHeaderWrapper(header, pindexPrev, adjusted_time, state)) {
        LOG_ERROR("Contextual check failed for block {}: {} - {}",
                 hash.ToString().substr(0, 16),
                 state.GetRejectReason(),
                 state.GetDebugMessage());
        // Mark as invalid and track it
        pindex->nStatus |= chain::BLOCK_FAILED_VALID;
        m_failed_blocks.insert(pindex);
        return nullptr;
    }

    // Step 9: Full POW verification (EXPENSIVE - this is why we cache the result)
    if (!CheckBlockHeaderWrapper(header, state)) {
        LOG_ERROR("Block header check failed for block {}: {} - {}",
                 hash.ToString().substr(0, 16),
                 state.GetRejectReason(),
                 state.GetDebugMessage());
        // Mark as invalid and track it
        pindex->nStatus |= chain::BLOCK_FAILED_VALID;
        m_failed_blocks.insert(pindex);
        return nullptr;
    }

    // Step 10: Mark as validated to TREE level
    [[maybe_unused]] bool raised = pindex->RaiseValidity(chain::BLOCK_VALID_TREE);

    // Step 11: Update best header
    chain_selector_.UpdateBestHeader(pindex);

    LOG_INFO("Accepted new block header: hash={}, height={}, work={}",
             hash.ToString().substr(0, 16),
             pindex->nHeight,
             pindex->nChainWork.ToString().substr(0, 16));

    // Step 12: Process any orphan children waiting for this parent
    ProcessOrphanHeaders(hash);

    return pindex;
}

bool ChainstateManager::ProcessNewBlockHeader(const CBlockHeader& header,
                                              ValidationState& state)
{
    // THREAD SAFETY: Uses std::recursive_mutex to make the entire operation atomic
    // All three steps (accept, add candidate, activate) hold validation_mutex_
    // without releasing it, preventing race conditions on m_candidates.
    //
    // Without recursive mutex, there would be a race between:
    // 1. AcceptBlockHeader() releasing the lock
    // 2. TryAddBlockIndexCandidate() acquiring it
    // 3. ActivateBestChain() acquiring it
    //
    // During these windows, another thread could modify m_candidates, causing
    // undefined behavior (iterator invalidation, broken std::set ordering, etc.)

    // Accept header (validates + adds to index)
    // Acquires validation_mutex_ recursively
    chain::CBlockIndex* pindex = AcceptBlockHeader(header, state);

    if (!pindex) {
        // Validation failed
        return false;
    }

    // Add to candidate set (if it's a viable tip)
    // Acquires validation_mutex_ recursively
    TryAddBlockIndexCandidate(pindex);

    // Block accepted - now try to activate best chain
    // Acquires validation_mutex_ recursively
    return ActivateBestChain(nullptr);
}

bool ChainstateManager::ActivateBestChain(chain::CBlockIndex* pindexMostWork)
{
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);

    // Find block with most work if not provided
    if (!pindexMostWork) {
        pindexMostWork = chain_selector_.FindMostWorkChain();
    }

    if (!pindexMostWork) {
        // No candidates - this is normal when there are no competing forks
        // Current tip is still the best chain
        LOG_DEBUG("ChainstateManager: No candidates found (no competing forks)");
        return true;  // Success - current chain is best
    }

    // Get current tip
    chain::CBlockIndex* pindexOldTip = block_manager_.GetTip();

    LOG_DEBUG("ActivateBestChain: pindexOldTip={} (height={}), pindexMostWork={} (height={})",
             pindexOldTip ? pindexOldTip->GetBlockHash().ToString().substr(0, 16) : "null",
             pindexOldTip ? pindexOldTip->nHeight : -1,
             pindexMostWork->GetBlockHash().ToString().substr(0, 16),
             pindexMostWork->nHeight);

    // Check if this is actually a new tip
    if (pindexOldTip == pindexMostWork) {
        // Already at best tip
        return true;
    }

    // Check if new chain has more work
    if (pindexOldTip && pindexMostWork->nChainWork <= pindexOldTip->nChainWork) {
        // Not enough work to switch
        LOG_DEBUG("Block accepted but not activated (insufficient work). Height: {}, Hash: {}",
                 pindexMostWork->nHeight,
                 pindexMostWork->GetBlockHash().ToString().substr(0, 16));
        return true;
    }

    // Find the fork point between old and new chains
    const chain::CBlockIndex* pindexFork = chain::LastCommonAncestor(pindexOldTip, pindexMostWork);

    LOG_DEBUG("ActivateBestChain: pindexFork={} (height={})",
             pindexFork ? pindexFork->GetBlockHash().ToString().substr(0, 16) : "null",
             pindexFork ? pindexFork->nHeight : -1);

    // Handle the case where no fork point exists (chains have no common ancestor)
    if (!pindexFork) {
        LOG_ERROR("ActivateBestChain: No common ancestor found between old tip and new chain");
        LOG_ERROR("  Old tip: height={}, hash={}",
                 pindexOldTip ? pindexOldTip->nHeight : -1,
                 pindexOldTip ? pindexOldTip->GetBlockHash().ToString().substr(0, 16) : "null");
        LOG_ERROR("  New tip: height={}, hash={}",
                 pindexMostWork->nHeight,
                 pindexMostWork->GetBlockHash().ToString().substr(0, 16));
        return false;
    }

    // INVARIANT: If we reach here, pindexOldTip is non-null.
    // Reason: LastCommonAncestor(a, b) returns nullptr if either a or b is nullptr.
    // Since pindexFork is non-null (we just checked), both inputs must have been non-null.
    // pindexMostWork is guaranteed non-null (checked at line 189).
    // Therefore, pindexOldTip must also be non-null.
    assert(pindexOldTip && "pindexOldTip must be non-null if pindexFork is non-null");

    // Calculate reorg depth (how many blocks will be disconnected)
    // Use height difference
    int reorg_depth = pindexOldTip->nHeight - pindexFork->nHeight;

    LOG_DEBUG("ActivateBestChain: reorg_depth={}, suspicious_reorg_depth_={}",
             reorg_depth, suspicious_reorg_depth_);

    // Deep reorg protection 
    // If suspiciousreorgdepth=N, reject reorgs >= N (allow up to N-1)
    if (suspicious_reorg_depth_ > 0 && reorg_depth >= suspicious_reorg_depth_) {
        LOG_ERROR("CRITICAL: Detected suspicious reorg of {} blocks, local policy allows {} blocks. "
                 "This may indicate a severe network issue or attack. Refusing to reorganize.",
                 reorg_depth, suspicious_reorg_depth_ - 1);
        LOG_ERROR("* current tip @ height {} ({})", pindexOldTip->nHeight,
                 pindexOldTip->GetBlockHash().ToString());
        LOG_ERROR("*   reorg tip @ height {} ({})", pindexMostWork->nHeight,
                 pindexMostWork->GetBlockHash().ToString());
        LOG_ERROR("*  fork point @ height {} ({})", pindexFork->nHeight,
                 pindexFork->GetBlockHash().ToString());
        return false;
    }

    // Disconnect blocks from old tip back to fork point
    std::vector<chain::CBlockIndex*> disconnected_blocks;  // Store as mutable
    chain::CBlockIndex* pindexWalk = pindexOldTip;
    while (pindexWalk && pindexWalk != pindexFork) {
        disconnected_blocks.push_back(pindexWalk);
        if (!DisconnectTip()) {
            LOG_ERROR("Failed to disconnect block during reorg");
            return false;
        }
        pindexWalk = block_manager_.GetTip();
    }

    // Connect blocks from fork point to new tip
    std::vector<chain::CBlockIndex*> connect_blocks;
    pindexWalk = pindexMostWork;
    while (pindexWalk && pindexWalk != pindexFork) {
        connect_blocks.push_back(pindexWalk);
        pindexWalk = pindexWalk->pprev;
    }

    // Connect in reverse order (from fork to tip)
    for (auto it = connect_blocks.rbegin(); it != connect_blocks.rend(); ++it) {
        if (!ConnectTip(*it)) {
            LOG_ERROR("Failed to connect block during reorg at height {}",
                     (*it)->nHeight);

            // ERROR RECOVERY: Roll back to old tip
            LOG_WARN("Attempting to roll back to old tip...");

            // First, disconnect any blocks we successfully connected
            while (block_manager_.GetTip() != pindexFork) {
                if (!DisconnectTip()) {
                    LOG_ERROR("CRITICAL: Rollback failed! Chain state may be inconsistent!");
                    return false;
                }
            }

            // Now reconnect the old chain
            for (auto rit = disconnected_blocks.rbegin(); rit != disconnected_blocks.rend(); ++rit) {
                if (!ConnectTip(*rit)) {  // No const_cast needed now
                    LOG_ERROR("CRITICAL: Failed to restore old chain! Chain state may be inconsistent!");
                    return false;
                }
            }

            // Defensive: GetTip() should be non-null after successful reconnection,
            // but guard against unexpected state corruption
            const chain::CBlockIndex* restored_tip = block_manager_.GetTip();
            if (restored_tip) {
                LOG_INFO("Rollback successful - restored old tip at height {}",
                        restored_tip->nHeight);
            } else {
                LOG_ERROR("CRITICAL: Rollback completed but tip is null! Chain state is inconsistent!");
            }
            return false;
        }
    }

    // Log reorg information
    if (!disconnected_blocks.empty()) {
        // INVARIANT: If we disconnected blocks, there must have been an old tip
        // This was verified earlier (pindexOldTip non-null after line 242 assert)
        // and the tip was only used to fill disconnected_blocks
        assert(pindexOldTip && "pindexOldTip must be non-null if blocks were disconnected");

        LOG_WARN("REORGANIZE: Disconnect {} blocks; Connect {} blocks",
                 disconnected_blocks.size(), connect_blocks.size());
        LOG_INFO("REORGANIZE: Old tip: height={}, hash={}",
                 pindexOldTip->nHeight,
                 pindexOldTip->GetBlockHash().ToString().substr(0, 16));
        LOG_INFO("REORGANIZE: New tip: height={}, hash={}",
                 pindexMostWork->nHeight,
                 pindexMostWork->GetBlockHash().ToString().substr(0, 16));
        LOG_INFO("REORGANIZE: Fork point: height={}, hash={}",
                 pindexFork ? pindexFork->nHeight : -1,
                 pindexFork ? pindexFork->GetBlockHash().ToString().substr(0, 16) : "null");
    } else {
        LOG_INFO("New best chain activated! Height: {}, Hash: {}, Work: {}",
                 pindexMostWork->nHeight,
                 pindexMostWork->GetBlockHash().ToString().substr(0, 16),
                 pindexMostWork->nChainWork.ToString().substr(0, 16));
    }

    // Emit final tip notification
    Notifications().NotifyChainTip(pindexMostWork, pindexMostWork->nHeight);

    // Prune candidates that now have less work than active tip
    chain_selector_.PruneBlockIndexCandidates(block_manager_);

    return true;
}

const chain::CBlockIndex* ChainstateManager::GetTip() const
{
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
    return block_manager_.GetTip();
}

chain::CBlockIndex* ChainstateManager::LookupBlockIndex(const uint256& hash)
{
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
    return block_manager_.LookupBlockIndex(hash);
}

const chain::CBlockIndex* ChainstateManager::LookupBlockIndex(const uint256& hash) const
{
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
    return block_manager_.LookupBlockIndex(hash);
}

CBlockLocator ChainstateManager::GetLocator(const chain::CBlockIndex* pindex) const
{
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
    if (pindex) {
        return chain::GetLocator(pindex);
    }
    return block_manager_.ActiveChain().GetLocator();
}

bool ChainstateManager::IsOnActiveChain(const chain::CBlockIndex* pindex) const
{
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
    return pindex && block_manager_.ActiveChain().Contains(pindex);
}

const chain::CBlockIndex* ChainstateManager::GetBlockAtHeight(int height) const
{
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
    if (height < 0 || height > block_manager_.ActiveChain().Height()) {
        return nullptr;
    }
    return block_manager_.ActiveChain()[height];
}


bool ChainstateManager::ConnectTip(chain::CBlockIndex* pindexNew)
{
    if (!pindexNew) {
        LOG_ERROR("ConnectTip: null block index");
        return false;
    }

    // NOTIFICATION SEMANTICS (matching Bitcoin Core):
    // ConnectTip: Update state BEFORE notifying
    //   - SetActiveTip() is called BEFORE NotifyBlockConnected()
    //   - Subscribers see the NEW tip when they call GetTip()
    //   - This allows subscribers to query the updated chain state
    //
    // DisconnectTip: Notify BEFORE updating state
    //   - NotifyBlockDisconnected() is called BEFORE SetActiveTip()
    //   - Subscribers see the block BEING disconnected when they call GetTip()
    //   - This allows subscribers to query the old state before it's removed
    //
    // CRITICAL: This asymmetry is intentional and matches Bitcoin Core's semantics.
    // Subscribers must be aware:
    //   - On BlockConnected: GetTip() returns the newly connected block
    //   - On BlockDisconnected: GetTip() returns the block being disconnected

    // For headers-only chain, "connecting" just means:
    // 1. Setting this as the active tip
    // 2. Emitting notifications

    block_manager_.SetActiveTip(*pindexNew);

    LOG_DEBUG("ConnectTip: height={}, hash={}",
             pindexNew->nHeight,
             pindexNew->GetBlockHash().ToString().substr(0, 16));

    // Emit block connected notification AFTER updating tip
    CBlockHeader header = pindexNew->GetBlockHeader();
    Notifications().NotifyBlockConnected(header, pindexNew);

    return true;
}

bool ChainstateManager::DisconnectTip()
{
    chain::CBlockIndex* pindexDelete = block_manager_.GetTip();
    if (!pindexDelete) {
        LOG_ERROR("DisconnectTip: no tip to disconnect");
        return false;
    }

    if (!pindexDelete->pprev) {
        LOG_ERROR("DisconnectTip: cannot disconnect genesis block");
        return false;
    }

    LOG_DEBUG("DisconnectTip: height={}, hash={}",
             pindexDelete->nHeight,
             pindexDelete->GetBlockHash().ToString().substr(0, 16));

    // NOTIFICATION SEMANTICS (matching Bitcoin Core):
    // Notify BEFORE updating state
    //   - Subscribers receive NotifyBlockDisconnected() while GetTip() still returns pindexDelete
    //   - This allows subscribers to query the old chain state before the block is removed
    //   - After notification, SetActiveTip() moves tip back to pprev
    //
    // See ConnectTip() for the complementary (but asymmetric) behavior on block connection.
    CBlockHeader header = pindexDelete->GetBlockHeader();
    Notifications().NotifyBlockDisconnected(header, pindexDelete);

    // For headers-only chain, "disconnecting" just means:
    // Moving the active tip pointer back to parent
    block_manager_.SetActiveTip(*pindexDelete->pprev);

    return true;
}

void ChainstateManager::TryAddBlockIndexCandidate(chain::CBlockIndex* pindex)
{
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
    chain_selector_.TryAddBlockIndexCandidate(pindex, block_manager_);
}

bool ChainstateManager::IsInitialBlockDownload() const
{
    // Fast path: check latch first (lock-free)
    if (m_cached_finished_ibd.load(std::memory_order_relaxed)) {
        return false;
    }

    // No tip yet - definitely in IBD
    const chain::CBlockIndex* tip = GetTip();
    if (!tip) {
        return true;
    }

    // Tip too old - still syncing (1 hour for 2-minute blocks)
    int64_t now = std::time(nullptr);
    if (tip->nTime < now - 3600) {
        return true;
    }

    // MinimumChainWork check (eclipse attack protection)
    // Prevents accepting fake low-work chains during IBD
    if (tip->nChainWork < UintToArith256(params_.GetConsensus().nMinimumChainWork)) {
        return true;
    }

    // All checks passed - we're synced!
    // Latch to false permanently
    LOG_INFO("Initial Block Download complete at height {}!", tip->nHeight);
    m_cached_finished_ibd.store(true, std::memory_order_relaxed);

    return false;
}

bool ChainstateManager::Initialize(const CBlockHeader& genesis_header)
{
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);

    if (!block_manager_.Initialize(genesis_header)) {
        return false;
    }

    // Initialize the candidate set with genesis block
    chain::CBlockIndex* genesis = block_manager_.GetTip();
    if (genesis) {
        chain_selector_.AddCandidateUnchecked(genesis);
        chain_selector_.SetBestHeader(genesis);
        LOG_DEBUG("Initialized with genesis as candidate: height={}, hash={}",
                 genesis->nHeight,
                 genesis->GetBlockHash().ToString().substr(0, 16));
    }

    return true;
}

bool ChainstateManager::Load(const std::string& filepath)
{
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);

    if (!block_manager_.Load(filepath, params_.GetConsensus().hashGenesisBlock)) {
        return false;
    }

    // Rebuild the candidate set after loading from disk
    // We need to find all leaf nodes (tips) in the block tree
    chain_selector_.ClearCandidates();
    chain_selector_.SetBestHeader(nullptr);

    // Walk through all blocks and find tips (blocks with no known children)
    // Algorithm:
    // 1. Build a set of all blocks that have children (by scanning pprev pointers)
    // 2. Any block NOT in that set is a leaf (potential candidate)
    // 3. Only add valid leaves (BLOCK_VALID_TREE) to candidates

    const auto& block_index = block_manager_.GetBlockIndex();

    // Step 1: Build set of blocks with children
    std::set<const chain::CBlockIndex*> blocks_with_children;
    for (const auto& [hash, block] : block_index) {
        if (block.pprev) {
            blocks_with_children.insert(block.pprev);
        }
    }

    // Step 2: Find all leaf nodes and add valid ones as candidates
    size_t leaf_count = 0;
    size_t candidate_count = 0;
    for (const auto& [hash, block] : block_index) {
        // Check if this block is a leaf (has no children)
        if (blocks_with_children.find(&block) == blocks_with_children.end()) {
            leaf_count++;

            // Only add as candidate if validated to TREE level
            if (block.IsValid(chain::BLOCK_VALID_TREE)) {
                // Need mutable pointer for candidate set
                chain::CBlockIndex* mutable_block =
                    const_cast<chain::CBlockIndex*>(&block);
                chain_selector_.AddCandidateUnchecked(mutable_block);
                candidate_count++;

                LOG_DEBUG("Added leaf as candidate: height={}, hash={}, work={}",
                         block.nHeight,
                         hash.ToString().substr(0, 16),
                         block.nChainWork.ToString().substr(0, 16));

                // Track best header (most chainwork)
                chain_selector_.UpdateBestHeader(mutable_block);
            } else {
                LOG_DEBUG("Found invalid leaf (not added to candidates): height={}, hash={}, status={}",
                         block.nHeight,
                         hash.ToString().substr(0, 16),
                         block.nStatus);
            }
        }
    }

    chain::CBlockIndex* tip = block_manager_.GetTip();
    LOG_INFO("Loaded chain state: {} total blocks, {} leaf nodes, {} valid candidates",
             block_index.size(),
             leaf_count,
             candidate_count);

    if (tip) {
        LOG_INFO("Active chain tip: height={}, hash={}",
                 tip->nHeight,
                 tip->GetBlockHash().ToString().substr(0, 16));
    }

    if (chain_selector_.GetBestHeader()) {
        LOG_INFO("Best header: height={}, hash={}, work={}",
                 chain_selector_.GetBestHeader()->nHeight,
                 chain_selector_.GetBestHeader()->GetBlockHash().ToString().substr(0, 16),
                 chain_selector_.GetBestHeader()->nChainWork.ToString().substr(0, 16));
    }

    return true;
}

bool ChainstateManager::Save(const std::string& filepath) const
{
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
    return block_manager_.Save(filepath);
}

size_t ChainstateManager::GetBlockCount() const
{
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
    return block_manager_.GetBlockCount();
}

int ChainstateManager::GetChainHeight() const
{
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
    return block_manager_.ActiveChain().Height();
}

void ChainstateManager::ProcessOrphanHeaders(const uint256& parentHash)
{
    // NOTE: Assumes validation_mutex_ is already held by caller

    std::vector<uint256> orphansToProcess;

    // Find all orphans that have this as parent
    for (const auto& [hash, orphan] : m_orphan_headers) {
        if (orphan.header.hashPrevBlock == parentHash) {
            orphansToProcess.push_back(hash);
        }
    }

    if (orphansToProcess.empty()) {
        return;
    }

    LOG_INFO("Processing {} orphan headers that were waiting for parent {}",
             orphansToProcess.size(),
             parentHash.ToString().substr(0, 16));

    // Process each orphan (this is recursive - orphan may have orphan children)
    for (const uint256& hash : orphansToProcess) {
        auto it = m_orphan_headers.find(hash);
        if (it == m_orphan_headers.end()) {
            continue;  // Already processed by earlier iteration
        }

        // IMPORTANT: Copy header BEFORE erasing from map to avoid dangling reference
        CBlockHeader orphan_header = it->second.header;  // Copy, not reference!
        int orphan_peer_id = it->second.peer_id;

        // Remove from orphan pool BEFORE processing
        // (prevents infinite recursion if orphan is invalid and re-added)
        m_orphan_headers.erase(it);

        // Decrement peer orphan count
        auto peer_it = m_peer_orphan_count.find(orphan_peer_id);
        if (peer_it != m_peer_orphan_count.end()) {
            peer_it->second--;
            if (peer_it->second == 0) {
                m_peer_orphan_count.erase(peer_it);
            }
        }

        // Recursively process the orphan
        LOG_DEBUG("Processing orphan header: hash={}, parent={}",
                 hash.ToString().substr(0, 16),
                 orphan_header.hashPrevBlock.ToString().substr(0, 16));

        ValidationState orphan_state;
        chain::CBlockIndex* pindex = AcceptBlockHeader(orphan_header, orphan_state, orphan_peer_id);

        if (!pindex) {
            LOG_DEBUG("Orphan header {} failed validation: {}",
                     hash.ToString().substr(0, 16),
                     orphan_state.GetRejectReason());
            // If it's orphaned again (missing grandparent), it will be re-added to pool
            // If it's invalid, it won't be re-added
        } else {
            LOG_INFO("Successfully processed orphan header: hash={}, height={}",
                    hash.ToString().substr(0, 16),
                    pindex->nHeight);
        }
    }
}

bool ChainstateManager::TryAddOrphanHeader(const CBlockHeader& header, int peer_id)
{
    // NOTE: Assumes validation_mutex_ is already held by caller

    uint256 hash = header.GetHash();

    // Check if already in orphan pool
    if (m_orphan_headers.find(hash) != m_orphan_headers.end()) {
        LOG_DEBUG("Orphan header {} already in pool", hash.ToString().substr(0, 16));
        return true;
    }

    // DoS Protection 1: Check per-peer limit
    int peer_orphan_count = m_peer_orphan_count[peer_id];
    if (peer_orphan_count >= static_cast<int>(MAX_ORPHAN_HEADERS_PER_PEER)) {
        LOG_WARN("Peer {} exceeded orphan limit ({}/{}), rejecting orphan {}",
                peer_id,
                peer_orphan_count,
                MAX_ORPHAN_HEADERS_PER_PEER,
                hash.ToString().substr(0, 16));
        return false;
    }

    // DoS Protection 2: Check total limit
    if (m_orphan_headers.size() >= MAX_ORPHAN_HEADERS) {
        // Evict oldest orphan to make room
        LOG_DEBUG("Orphan pool full ({}/{}), evicting oldest",
                 m_orphan_headers.size(),
                 MAX_ORPHAN_HEADERS);

        size_t evicted = EvictOrphanHeaders();
        if (evicted == 0) {
            LOG_ERROR("Failed to evict any orphans, pool stuck at max size");
            return false;
        }
    }

    // Add to orphan pool
    m_orphan_headers[hash] = OrphanHeader{
        header,
        std::time(nullptr),
        peer_id
    };

    // Update peer count
    m_peer_orphan_count[peer_id]++;

    LOG_DEBUG("Added orphan header to pool: hash={}, peer={}, pool_size={}, peer_orphans={}",
             hash.ToString().substr(0, 16),
             peer_id,
             m_orphan_headers.size(),
             m_peer_orphan_count[peer_id]);

    return true;
}

size_t ChainstateManager::EvictOrphanHeaders()
{
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);

    if (m_orphan_headers.empty()) {
        return 0;
    }

    int64_t now = std::time(nullptr);
    size_t evicted = 0;

    // Strategy 1: Evict expired orphans (older than 10 minutes)
    auto it = m_orphan_headers.begin();
    while (it != m_orphan_headers.end()) {
        if (now - it->second.nTimeReceived > ORPHAN_HEADER_EXPIRE_TIME) {
            LOG_DEBUG("Evicting expired orphan header: hash={}, age={}s",
                     it->first.ToString().substr(0, 16),
                     now - it->second.nTimeReceived);

            // Decrement peer count
            int peer_id = it->second.peer_id;
            auto peer_it = m_peer_orphan_count.find(peer_id);
            if (peer_it != m_peer_orphan_count.end()) {
                peer_it->second--;
                if (peer_it->second == 0) {
                    m_peer_orphan_count.erase(peer_it);
                }
            }

            it = m_orphan_headers.erase(it);
            evicted++;
        } else {
            ++it;
        }
    }

    // Strategy 2: If still at limit, evict oldest
    if (evicted == 0 && m_orphan_headers.size() >= MAX_ORPHAN_HEADERS) {
        // Find oldest orphan
        auto oldest = m_orphan_headers.begin();
        for (auto it = m_orphan_headers.begin(); it != m_orphan_headers.end(); ++it) {
            if (it->second.nTimeReceived < oldest->second.nTimeReceived) {
                oldest = it;
            }
        }

        LOG_DEBUG("Evicting oldest orphan header: hash={}, age={}s",
                 oldest->first.ToString().substr(0, 16),
                 now - oldest->second.nTimeReceived);

        // Decrement peer count
        int peer_id = oldest->second.peer_id;
        auto peer_it = m_peer_orphan_count.find(peer_id);
        if (peer_it != m_peer_orphan_count.end()) {
            peer_it->second--;
            if (peer_it->second == 0) {
                m_peer_orphan_count.erase(peer_it);
            }
        }

        m_orphan_headers.erase(oldest);
        evicted++;
    }

    if (evicted > 0) {
        LOG_INFO("Evicted {} orphan headers (pool size now: {})",
                evicted,
                m_orphan_headers.size());
    }

    return evicted;
}

size_t ChainstateManager::GetOrphanHeaderCount() const
{
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
    return m_orphan_headers.size();
}

bool ChainstateManager::CheckHeadersPoW(const std::vector<CBlockHeader>& headers) const
{
    // Check all headers have valid proof-of-work
    // Uses virtual CheckProofOfWork so tests can override
    for (const auto& header : headers) {
        if (!CheckProofOfWork(header, crypto::POWVerifyMode::COMMITMENT_ONLY)) {
            LOG_DEBUG("Header failed PoW commitment check: {}",
                     header.GetHash().ToString().substr(0, 16));
            return false;
        }
    }
    return true;
}

bool ChainstateManager::CheckProofOfWork(const CBlockHeader& header,
                                         crypto::POWVerifyMode mode) const
{
    // Default implementation: use real RandomX PoW validation
    return consensus::CheckProofOfWork(header, header.nBits, params_, mode);
}

bool ChainstateManager::CheckBlockHeaderWrapper(const CBlockHeader& header,
                                                 ValidationState& state) const
{
    // Default implementation: call real validation
    return CheckBlockHeader(header, params_, state);
}

bool ChainstateManager::ContextualCheckBlockHeaderWrapper(const CBlockHeader& header,
                                                          const chain::CBlockIndex* pindexPrev,
                                                          int64_t adjusted_time,
                                                          ValidationState& state) const
{
    // Default implementation: call real contextual validation
    return ContextualCheckBlockHeader(header, pindexPrev, params_, adjusted_time, state);
}

bool ChainstateManager::InvalidateBlock(const uint256& hash)
{
    std::lock_guard<std::recursive_mutex> lock(validation_mutex_);

    // Look up the block
    chain::CBlockIndex* pindex = block_manager_.LookupBlockIndex(hash);
    if (!pindex) {
        LOG_ERROR("InvalidateBlock: block {} not found", hash.ToString());
        return false;
    }

    LOG_INFO("Invalidating block {} at height {}", hash.ToString(), pindex->nHeight);

    // Mark this block as BLOCK_FAILED_VALID
    pindex->nStatus |= chain::BLOCK_FAILED_VALID;
    m_failed_blocks.insert(pindex);

    // Mark all descendants as BLOCK_FAILED_CHILD
    // Walk through all blocks and mark any that descend from this block
    const auto& block_index = block_manager_.GetBlockIndex();
    for (const auto& [block_hash, block] : block_index) {
        // Skip the block itself (already marked)
        if (&block == pindex) {
            continue;
        }

        // Check if this block descends from the invalidated block
        // by walking back through ancestors
        const chain::CBlockIndex* ancestor = block.GetAncestor(pindex->nHeight);
        if (ancestor == pindex) {
            // This block descends from the invalidated block
            const_cast<chain::CBlockIndex*>(&block)->nStatus |= chain::BLOCK_FAILED_CHILD;
            LOG_DEBUG("Marked descendant {} at height {} as BLOCK_FAILED_CHILD",
                     block_hash.ToString().substr(0, 16),
                     block.nHeight);
        }
    }

    // Remove invalidated blocks from the candidate set
    chain_selector_.ClearCandidates();

    // Rebuild candidates with only valid blocks
    for (const auto& [block_hash, block] : block_index) {
        if (block.IsValid(chain::BLOCK_VALID_TREE)) {
            // Check if this is a leaf node (no known children)
            bool is_leaf = true;
            for (const auto& [other_hash, other_block] : block_index) {
                if (other_block.pprev == &block) {
                    is_leaf = false;
                    break;
                }
            }

            if (is_leaf) {
                chain::CBlockIndex* mutable_block = const_cast<chain::CBlockIndex*>(&block);
                chain_selector_.AddCandidateUnchecked(mutable_block);
                LOG_DEBUG("Re-added valid leaf as candidate: height={}, hash={}",
                         block.nHeight,
                         block_hash.ToString().substr(0, 16));
            }
        }
    }

    // If the active tip descends from the invalidated block, reactivate best valid chain
    chain::CBlockIndex* current_tip = block_manager_.GetTip();
    if (current_tip) {
        const chain::CBlockIndex* ancestor = current_tip->GetAncestor(pindex->nHeight);
        if (ancestor == pindex) {
            // Current tip descends from invalidated block - need to reactivate
            LOG_WARN("Active tip descends from invalidated block, reactivating best valid chain");

            // Find the best valid chain
            chain::CBlockIndex* pindexMostWork = chain_selector_.FindMostWorkChain();
            if (pindexMostWork) {
                LOG_INFO("Reactivating to block {} at height {}",
                        pindexMostWork->GetBlockHash().ToString(),
                        pindexMostWork->nHeight);
                ActivateBestChain(pindexMostWork);
            } else {
                LOG_ERROR("No valid chain found after invalidation!");
                return false;
            }
        }
    }

    LOG_INFO("Successfully invalidated block {} and all descendants", hash.ToString());
    return true;
}

} // namespace validation
} // namespace coinbasechain
