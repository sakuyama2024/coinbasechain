// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_VALIDATION_CHAINSTATE_MANAGER_HPP
#define COINBASECHAIN_VALIDATION_CHAINSTATE_MANAGER_HPP

#include "chain/block_manager.hpp"
#include "primitives/block.h"
#include "validation/chain_selector.hpp"
#include "validation/validation.hpp"
#include <atomic>
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
// Processes headers, activates best chain, emits notifications (simplified from
// Bitcoin Core) Main entry point for adding blocks to the chain (mining or
// network)
class ChainstateManager {
public:
  // LIFETIME: ChainParams reference must outlive this ChainstateManager
  ChainstateManager(const chain::ChainParams &params,
                    int suspicious_reorg_depth = 100);

  // CRITICAL ANTI-DOS: Cheap commitment PoW check BEFORE index, full PoW AFTER
  // (cached if fails) ORPHAN HANDLING: Missing parent → cached as orphan (DoS
  // limits), auto-processed when parent arrives Returns nullptr if orphaned
  // (state="orphaned") or failed (state="invalid")
  chain::CBlockIndex *AcceptBlockHeader(const CBlockHeader &header,
                                        ValidationState &state,
                                        int peer_id = -1);

  // Process header: accept → activate best chain → notify if tip changed
  bool ProcessNewBlockHeader(const CBlockHeader &header,
                             ValidationState &state);

  // Activate chain with most work, emit notifications if tip changed
  bool ActivateBestChain(chain::CBlockIndex *pindexMostWork = nullptr);

  const chain::CBlockIndex *GetTip() const;

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
  // Connect/disconnect blocks (headers-only: just updates chain state and
  // notifications) Assumes validation_mutex_ held by caller
  bool ConnectTip(chain::CBlockIndex *pindexNew);
  bool DisconnectTip();

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

  // DoS protection limits
  static constexpr size_t MAX_ORPHAN_HEADERS =
      1000; // Total orphans across all peers
  static constexpr size_t MAX_ORPHAN_HEADERS_PER_PEER =
      50; // Max orphans per peer
  static constexpr int64_t ORPHAN_HEADER_EXPIRE_TIME =
      600; // 10 minutes in seconds

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
