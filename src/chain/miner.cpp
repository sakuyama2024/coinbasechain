// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "chain/miner.hpp"
#include "chain/arith_uint256.hpp"
#include "chain/pow.hpp"
#include "chain/randomx_pow.hpp"
#include "randomx.h"
#include "chain/logging.hpp"
#include "chain/chainstate_manager.hpp"
#include <ctime>
#include <thread>

namespace coinbasechain {
namespace mining {

CPUMiner::CPUMiner(const chain::ChainParams &params,
                   validation::ChainstateManager &chainstate)
    : params_(params), chainstate_(chainstate) {}

CPUMiner::~CPUMiner() { Stop(); }


  // Single-threaded CPU mining
  
bool CPUMiner::Start() {
  if (mining_.load()) {
    LOG_WARN("Miner: Already mining");
    return false;
  }


  LOG_INFO("Miner: Starting (chain: {})", params_.GetChainTypeString());

  mining_.store(true);
  total_hashes_.store(0);
  start_time_ = std::chrono::steady_clock::now();

  // Create initial block template
  current_template_ = CreateBlockTemplate();
  template_prev_hash_ = current_template_.hashPrevBlock;

  LOG_INFO("Miner: Mining block at height {}", current_template_.nHeight);
  LOG_INFO("  Previous: {}...",
           current_template_.hashPrevBlock.ToString().substr(0, 16));
  LOG_INFO("  Target:   0x{:x}", current_template_.nBits);

  // Start single mining thread
  worker_ = std::thread([this]() { MiningWorker(); });

  return true;
}

void CPUMiner::Stop() {
  if (!mining_.load()) {
    return;
  }

  LOG_INFO("Miner: Stopping...");
  mining_.store(false);

  // Join mining thread
  if (worker_.joinable()) {
    worker_.join();
  }

  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::steady_clock::now() - start_time_)
                     .count();

  uint64_t hashes = total_hashes_.load();
  double hashrate = (elapsed > 0) ? (double)hashes / elapsed : 0.0;

  LOG_INFO("Miner: Stopped");
  LOG_INFO("  Total hashes: {}", hashes);
  LOG_INFO("  Time: {}s", elapsed);
  LOG_INFO("  Hashrate: {} H/s", hashrate);
  LOG_INFO("  Blocks found: {}", blocks_found_.load());
}

double CPUMiner::GetHashrate() const {
  if (!mining_.load()) {
    return 0.0;
  }

  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::steady_clock::now() - start_time_)
                     .count();

  if (elapsed == 0) {
    return 0.0;
  }

  return (double)total_hashes_.load() / elapsed;
}

void CPUMiner::MiningWorker() {
  uint32_t nonce = 0;

  while (mining_.load()) {
    // Check if we need to regenerate template (chain tip changed)
    if (ShouldRegenerateTemplate()) {
      LOG_INFO("Miner: Chain tip changed, regenerating template");
      current_template_ = CreateBlockTemplate();
      template_prev_hash_ = current_template_.hashPrevBlock;
      nonce = 0; // Restart nonce
    }

    // Update nonce
    current_template_.header.nNonce = nonce;

    // Try mining this nonce using RandomX
    uint256 rx_hash;
    bool found_block = consensus::CheckProofOfWork(
        current_template_.header, current_template_.nBits, params_,
        crypto::POWVerifyMode::MINING, &rx_hash);

    // Check if we found a block
    if (found_block) {
      blocks_found_.fetch_add(1);

      CBlockHeader found_header = current_template_.header;
      found_header.hashRandomX = rx_hash;

      LOG_INFO("Miner: *** BLOCK FOUND *** Height: {}, Nonce: {}, Hash: {}",
               current_template_.nHeight, nonce,
               found_header.GetHash().ToString().substr(0, 16));

      // Process block through chainstate manager
      // This validates, activates best chain, and emits notifications
      validation::ValidationState state;
      if (!chainstate_.ProcessNewBlockHeader(found_header, state)) {
        LOG_ERROR("Miner: Failed to process mined block: {} - {}",
                  state.GetRejectReason(), state.GetDebugMessage());
      }

      // Continue mining next block
      current_template_ = CreateBlockTemplate();
      template_prev_hash_ = current_template_.hashPrevBlock;
      nonce = 0;
      continue;
    }

    // Update stats
    total_hashes_.fetch_add(1);

    // Next nonce
    nonce++;
    if (nonce == 0) {
      // Wrapped around - very unlikely with RandomX difficulty
      nonce = 0;
    }
  }
}

BlockTemplate CPUMiner::CreateBlockTemplate() {
  BlockTemplate tmpl;

  // Get current chain tip
  const chain::CBlockIndex *tip = chainstate_.GetTip();
  if (!tip) {
    // No tip, use genesis
    tmpl.hashPrevBlock.SetNull();
    tmpl.nHeight = 0;
  } else {
    tmpl.hashPrevBlock = tip->GetBlockHash();
    tmpl.nHeight = tip->nHeight + 1;
  }

  // Calculate difficulty
  tmpl.nBits = consensus::GetNextWorkRequired(tip, params_);

  // Fill header
  tmpl.header.nVersion = 1;
  tmpl.header.hashPrevBlock = tmpl.hashPrevBlock;
  tmpl.header.minerAddress.SetNull(); // TODO: Set miner address
  tmpl.header.nTime = static_cast<uint32_t>(std::time(nullptr));
  tmpl.header.nBits = tmpl.nBits;
  tmpl.header.nNonce = 0;
  tmpl.header.hashRandomX.SetNull();

  // Ensure timestamp is greater than median time past
  // This is critical for regtest when mining blocks rapidly
  // TODO
  if (tip) {
    int64_t median_time_past = tip->GetMedianTimePast();
    if (tmpl.header.nTime <= median_time_past) {
      tmpl.header.nTime = median_time_past + 1;
    }
  }

  return tmpl;
}

bool CPUMiner::ShouldRegenerateTemplate() {
  // Check atomic flag first (notification-based, fast)
  if (template_invalidated_.exchange(false)) {
    return true;
  }

  // Fallback to polling (slower, but ensures correctness)
  const chain::CBlockIndex *tip = chainstate_.GetTip();
  if (!tip) {
    return template_prev_hash_.IsNull();
  }

  return tip->GetBlockHash() != template_prev_hash_;
}

} // namespace mining
} // namespace coinbasechain
