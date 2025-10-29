// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_VALIDATION_CHAINSTATE_MANAGER_HPP
#define COINBASECHAIN_VALIDATION_CHAINSTATE_MANAGER_HPP

#include "chain/block_manager.hpp"
#include "chain/block.hpp"
#include "chain/chain_selector.hpp"
#include "chain/validation.hpp"
#include "network/protocol.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>

namespace coinbasechain {

// Forward declarations
namespace chain {
class ChainParams;
class CBlockIndex;
} // namespace chain

namespace crypto {
enum class POWVerifyMode;
}

namespace validation {

// ChainstateManager - High-level coordinator for blockchain state
// Processes headers, activates best chain, emits notifications 
// Main entry point for adding blocks to the chain (mining or
// network)
class ChainstateManager {
public:
  // LIFETIME: ChainParams reference must outlive this ChainstateManager
  explicit ChainstateManager(const chain::ChainParams &params,
                             int suspicious_reorg_depth = 100);

  // CRITICAL ANTI-DOS: Cheap commitment PoW check BEFORE index, full PoW AFTER
  // (cached if fails) ORPHAN HANDLING: Missing parent → cached as orphan (DoS
  // limits), auto-processed when parent arrives Returns nullptr if orphaned
  // (state="orphaned") or failed (state="invalid")
  // Accept a block header into the block index following Core's ordering.
  // min_pow_checked gates anti-DoS: caller must ensure header chain has sufficient work.
  chain::CBlockIndex *AcceptBlockHeader(const CBlockHeader &header,
                                        ValidationState &state,
                                        bool min_pow_checked);

  // Process header: accept → activate best chain → notify if tip changed
  bool ProcessNewBlockHeader(const CBlockHeader &header,
                             ValidationState &state);

  // Activate chain with most work, emit notifications if tip changed
  bool ActivateBestChain(chain::CBlockIndex *pindexMostWork = nullptr);

  const chain::CBlockIndex *GetTip() const;

  // Get chain parameters (thread-safe, params_ is const)
  const chain::ChainParams &GetParams() const { return params_; }

  // Thread-safe block index lookup
  chain::CBlockIndex *LookupBlockIndex(const uint256 &hash);
  const chain::CBlockIndex *LookupBlockIndex(const uint256 &hash) const;

  // Get block locator (nullptr = tip)
  CBlockLocator GetLocator(const chain::CBlockIndex *pindex = nullptr) const;

  bool IsOnActiveChain(const chain::CBlockIndex *pindex) const;
  const chain::CBlockIndex *GetBlockAtHeight(int height) const;

  // Check if in IBD (no tip, tip too old, or low work)
  // Latches to false once IBD completes (no flapping)
  bool IsInitialBlockDownload() const;

  // Add block to candidate set (for batch processing workflows)
  void TryAddBlockIndexCandidate(chain::CBlockIndex *pindex);

  bool Initialize(const CBlockHeader &genesis_header);
  bool Load(const std::string &filepath);
  bool Save(const std::string &filepath) const;

  size_t GetBlockCount() const;
  int GetChainHeight() const;

  // Add orphan header (network-layer helper) with per-peer limits/DoS checks
  bool AddOrphanHeader(const CBlockHeader &header, int peer_id);

  // Evict old orphan headers (DoS protection)
  size_t EvictOrphanHeaders();
  size_t GetOrphanHeaderCount() const;

  // Mark block and descendants invalid (for invalidateblock RPC)
  bool InvalidateBlock(const uint256 &hash);

  // Check PoW for batch of headers (virtual for testing)
  bool CheckHeadersPoW(const std::vector<CBlockHeader> &headers) const;

protected:
  // Virtual methods for test mocking
  virtual bool CheckProofOfWork(const CBlockHeader &header,
                                crypto::POWVerifyMode mode) const;
  virtual bool CheckBlockHeaderWrapper(const CBlockHeader &header,
                                       ValidationState &state) const;
  virtual bool ContextualCheckBlockHeaderWrapper(
      const CBlockHeader &header, const chain::CBlockIndex *pindexPrev,
      int64_t adjusted_time, ValidationState &state) const;

private:
  // Activation step result classification (Core-alike control flow)
  enum class ActivateResult {
    OK,                // activation complete or nothing to do
    CONSENSUS_INVALID, // candidate (or its chain) is consensus-invalid
    POLICY_REFUSED,    // refused by local policy (e.g., suspicious reorg)
    SYSTEM_ERROR       // unexpected failure (I/O/corruption)
  };

  // Deferred notification events (dispatched after releasing validation lock)
  enum class NotifyType { BlockConnected, BlockDisconnected, ChainTip };
  struct PendingNotification {
    NotifyType type;
    CBlockHeader header; // for block connect/disconnect
    chain::CBlockIndex *pindex{nullptr};
    int height{0}; // for ChainTip
  };

  // One activation attempt for a specific candidate; does not loop.
  // Assumes validation_mutex_ is held by caller. Appends events on success only.
  ActivateResult ActivateBestChainStep(chain::CBlockIndex *pindexMostWork,
                                       std::vector<PendingNotification> &events);

  // Connect/disconnect blocks (headers-only: just updates chain state and queues
  // notifications). Assumes validation_mutex_ held by caller
  bool ConnectTip(chain::CBlockIndex *pindexNew,
                  std::vector<PendingNotification> &events);
  bool DisconnectTip(std::vector<PendingNotification> &events);

  // Process orphan headers waiting for parent (recursive)
  // Assumes validation_mutex_ held by caller
  void ProcessOrphanHeaders(const uint256 &parentHash);

  // Try to add orphan header (checks DoS limits, evicts old orphans)
  // Assumes validation_mutex_ held by caller
  bool TryAddOrphanHeader(const CBlockHeader &header, int peer_id);

  chain::BlockManager block_manager_;
  ChainSelector chain_selector_;
  const chain::ChainParams &params_;
  int suspicious_reorg_depth_;

  // Orphan header storage (headers with missing parent, auto-processed when
  // parent arrives)
  struct OrphanHeader {
    CBlockHeader header;
    int64_t nTimeReceived;
    int peer_id;
  };

  // DoS Protection: limited size, time-based eviction, per-peer limits
  // Protected by validation_mutex_
  std::map<uint256, OrphanHeader> m_orphan_headers;
  std::map<int, int> m_peer_orphan_count; // peer_id -> orphan count

  // Failed blocks (prevents reprocessing, marks descendants as
  // BLOCK_FAILED_CHILD) Protected by validation_mutex_
  std::set<chain::CBlockIndex *> m_failed_blocks;

  // Cached IBD status (latches false once complete, atomic for lock-free reads)
  mutable std::atomic<bool> m_cached_finished_ibd{false};

  // THREAD SAFETY: Recursive mutex serializes all validation operations
  // Protected: block_manager_, chain_selector_, m_failed_blocks,
  // m_orphan_headers Not protected: m_cached_finished_ibd (atomic), params_
  // (const), suspicious_reorg_depth_ (const) All public methods acquire lock,
  // private methods assume lock held
  mutable std::recursive_mutex validation_mutex_;
};

} // namespace validation
} // namespace coinbasechain

#endif // COINBASECHAIN_VALIDATION_CHAINSTATE_MANAGER_HPP
