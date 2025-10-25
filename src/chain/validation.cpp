// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "chain/validation.hpp"
#include "chain/block_index.hpp"
#include "chain/block_manager.hpp"
#include "chain/pow.hpp"
#include "chain/randomx_pow.hpp"
#include "chain/logging.hpp"
#include "chain/timedata.hpp"
#include <algorithm>
#include <ctime>

namespace coinbasechain {
namespace validation {

bool CheckBlockHeader(const CBlockHeader &header,
                      const chain::ChainParams &params,
                      ValidationState &state) {
  // Check proof of work (RandomX)
  if (!consensus::CheckProofOfWork(header, header.nBits, params,
                                   crypto::POWVerifyMode::FULL)) {
    return state.Invalid("high-hash", "proof of work failed");
  }

  return true;
}

bool ContextualCheckBlockHeader(const CBlockHeader &header,
                                const chain::CBlockIndex *pindexPrev,
                                const chain::ChainParams &params,
                                int64_t adjusted_time, ValidationState &state) {
  // Check that the block's difficulty matches the expected value
  uint32_t expected_bits = consensus::GetNextWorkRequired(pindexPrev, params);
  if (header.nBits != expected_bits) {
    return state.Invalid("bad-diffbits", "incorrect difficulty: expected " +
                                             std::to_string(expected_bits) +
                                             ", got " +
                                             std::to_string(header.nBits));
  }

  // Check timestamp against prev
  if (pindexPrev) {
    // Get median time past of last MEDIAN_TIME_SPAN blocks
    int64_t median_time_past = pindexPrev->GetMedianTimePast();

    // Block timestamp must be after median time past
    if (header.nTime <= median_time_past) {
      return state.Invalid(
          "time-too-old",
          "block's timestamp is too early: " + std::to_string(header.nTime) +
              " <= " + std::to_string(median_time_past));
    }
  }

  // Check timestamp is not too far in future
  if (header.nTime > adjusted_time + MAX_FUTURE_BLOCK_TIME) {
    return state.Invalid(
        "time-too-new",
        "block timestamp too far in future: " + std::to_string(header.nTime) +
            " > " + std::to_string(adjusted_time + MAX_FUTURE_BLOCK_TIME));
  }

  // Version validation (for now, just accept version 1)
  // In future, could add BIP9-style version checks
  if (header.nVersion < 1) {
    return state.Invalid("bad-version", "block version too old: " +
                                            std::to_string(header.nVersion));
  }

  // Network expiration (timebomb) check - forces regular updates
  const chain::ConsensusParams& consensus = params.GetConsensus();
  if (consensus.nNetworkExpirationInterval > 0) {
    int32_t currentHeight = pindexPrev ? pindexPrev->nHeight + 1 : 0;
    int32_t expirationHeight = consensus.nNetworkExpirationInterval;

    // Reject blocks beyond expiration height
    if (currentHeight > expirationHeight) {
      return state.Invalid("network-expired",
        "Network expired at block " + std::to_string(expirationHeight) +
        ". This version is outdated. Please update to the latest version to continue.");
    }

    // Log warning when approaching expiration
    if (currentHeight > expirationHeight - consensus.nNetworkExpirationGracePeriod) {
      LOG_CHAIN_WARN("WARNING: Network will expire at block {} (current: {}). Please update to the latest version soon!",
                     expirationHeight, currentHeight);
    }
  }

  return true;
}

// Note: AcceptBlockHeader is now a method of ChainstateManager
// This allows it to access m_failed_blocks and m_best_header

int64_t GetAdjustedTime() {
  // Network-adjusted time: system time + median offset from peers
  //
  // This is critical for blockchain security:
  // - Prevents nodes with incorrect clocks from accepting invalid blocks
  // - Protects against timestamp manipulation in difficulty adjustment
  // - Mitigates eclipse attacks where attacker controls victim's time
  // perception
  //
  // Implementation details:
  // 1. Track time samples from peers (collected from version messages)
  // 2. Calculate median offset from trusted peers (requires ≥5 peers)
  // 3. Cap adjustment to ±70 minutes (DEFAULT_MAX_TIME_ADJUSTMENT)
  // 4. Warn user if local clock differs significantly from network
  //
  // See util/timedata.cpp for full implementation (based on Bitcoin Core)
  return static_cast<int64_t>(std::time(nullptr)) + util::GetTimeOffset();
}

// ============================================================================
// DoS Protection Functions
// ============================================================================

arith_uint256 GetAntiDoSWorkThreshold(const chain::CBlockIndex *tip,
                                      const chain::ChainParams &params,
                                      bool is_ibd) {
  // During IBD, disable anti-DoS checks to allow syncing from genesis
  if (is_ibd) {
    return 0;
  }

  arith_uint256 near_tip_work = 0;

  if (tip != nullptr) {
    // Calculate work of one block at current difficulty
    arith_uint256 block_proof = chain::GetBlockProof(*tip);

    // Calculate work buffer (chain-specific number of blocks)
    arith_uint256 buffer = block_proof * params.GetConsensus().nAntiDosWorkBufferBlocks;

    // Subtract buffer from tip work (but don't go negative)
    near_tip_work = tip->nChainWork - std::min(buffer, tip->nChainWork);
  }

  // Return the higher of: near-tip work OR configured minimum
  arith_uint256 min_chain_work =
      UintToArith256(params.GetConsensus().nMinimumChainWork);
  return std::max(near_tip_work, min_chain_work);
}

arith_uint256 CalculateHeadersWork(const std::vector<CBlockHeader> &headers) {
  arith_uint256 total_work = 0;

  for (const auto &header : headers) {
    // Get the proof-of-work (difficulty) for this header
    // Calculate: 2^256 / (target + 1)
    arith_uint256 bnTarget;
    bool fNegative, fOverflow;
    bnTarget.SetCompact(header.nBits, &fNegative, &fOverflow);

    // Reject invalid nBits encodings:
    // - fNegative: Sign bit set (0x00800000) with non-zero mantissa
    // - fOverflow: Exponent too large (size > 34 bytes for 256-bit value)
    // - bnTarget == 0: Zero mantissa (e.g., nBits = 0x00000000 or 0x01000000)
    //
    // Note on bnTarget == 0:
    // A zero target would represent infinite difficulty, which is nonsensical
    // and would cause division issues in work calculations. The Bitcoin compact
    // format allows encoding this (mantissa can be 0), but it's
    // consensus-invalid.
    //
    // While the formula (~bnTarget / (bnTarget + 1)) is mathematically safe
    // when bnTarget=0 (divides by 1), we still reject it as an invalid
    // difficulty target. Such blocks should never appear in a valid chain and
    // are filtered here.
    if (fNegative || fOverflow || bnTarget == 0) {
      // Skip invalid difficulty - contributes 0 work to total
      // These headers would fail full validation anyway
      continue;
    }

    // Work = ~target / (target + 1)
    // Approximates 2^256 / target for practical difficulty values
    arith_uint256 block_proof = (~bnTarget / (bnTarget + 1)) + 1;
    total_work += block_proof;
  }

  return total_work;
}

bool CheckHeadersPoW(const std::vector<CBlockHeader> &headers,
                     const chain::ChainParams &params) {
  // Check all headers have valid proof-of-work
  // Use COMMITMENT_ONLY mode for cheap validation (no full RandomX hash)
  for (const auto &header : headers) {
    if (!consensus::CheckProofOfWork(header, header.nBits, params,
                                     crypto::POWVerifyMode::COMMITMENT_ONLY)) {
      LOG_CHAIN_TRACE("Header failed PoW commitment check: {}",
                header.GetHash().ToString().substr(0, 16));
      return false;
    }
  }

  return true;
}

bool CheckHeadersAreContinuous(const std::vector<CBlockHeader> &headers) {
  if (headers.empty()) {
    return true;
  }

  // Check each header's prevhash matches the previous header's hash
  for (size_t i = 1; i < headers.size(); i++) {
    if (headers[i].hashPrevBlock != headers[i - 1].GetHash()) {
      LOG_CHAIN_TRACE("Headers not continuous at index {}: prevhash={}, expected={}",
                i, headers[i].hashPrevBlock.ToString().substr(0, 16),
                headers[i - 1].GetHash().ToString().substr(0, 16));
      return false;
    }
  }

  return true;
}

} // namespace validation
} // namespace coinbasechain
