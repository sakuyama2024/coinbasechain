// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_VALIDATION_VALIDATION_HPP
#define COINBASECHAIN_VALIDATION_VALIDATION_HPP

#include "primitives/block.h"
#include "chain/block_index.hpp"
#include "chain/chainparams.hpp"
#include <string>

namespace coinbasechain {

// Forward declarations
namespace chain {
    class BlockManager;
    class ChainParams;
    class CBlockIndex;
}

namespace validation {

/**
 * ============================================================================
 * BLOCK HEADER VALIDATION ARCHITECTURE
 * ============================================================================
 *
 * This module provides a layered validation approach for block headers:
 *
 * LAYER 1: Fast Pre-filtering (for P2P header sync)
 * - CheckHeadersPoW()           : Commitment-only PoW check (~50x faster)
 * - CheckHeadersAreContinuous() : Chain structure validation
 * Purpose: Quickly reject obviously invalid headers during sync
 *
 * LAYER 2: Full Context-Free Validation (before chain acceptance)
 * - CheckBlockHeader()          : FULL RandomX PoW verification
 * Purpose: Cryptographically verify the header is valid in isolation
 * Security: Validates PoW meets header.nBits, but NOT that nBits is correct
 *
 * LAYER 3: Contextual Validation (requires parent block)
 * - ContextualCheckBlockHeader(): Validates nBits, timestamps, version
 * Purpose: CRITICAL - ensures header follows chain consensus rules
 * Security: Without this, attackers can mine with artificially low difficulty
 *
 * INTEGRATION POINT:
 * - ChainstateManager::AcceptBlockHeader() orchestrates all validation layers
 *
 * TIME SECURITY:
 * - GetAdjustedTime(): ⚠️ CURRENTLY INSECURE - uses raw system time
 *   Must implement network-adjusted time before production deployment
 *
 * DoS PROTECTION:
 * - GetAntiDoSWorkThreshold(): Rejects low-work header spam
 * - CalculateHeadersWork()    : Computes cumulative chain work
 * ============================================================================
 */

/**
 * Validation state - tracks why validation failed
 * Simplified from Bitcoin Core's BlockValidationState
 */
class ValidationState {
public:
    enum class Result {
        VALID,
        INVALID,        // Invalid block (permanent failure)
        ERROR           // System error (temporary failure)
    };

    ValidationState() : result_(Result::VALID) {}

    bool IsValid() const { return result_ == Result::VALID; }
    bool IsInvalid() const { return result_ == Result::INVALID; }
    bool IsError() const { return result_ == Result::ERROR; }

    bool Invalid(const std::string& reject_reason, const std::string& debug_message = "") {
        result_ = Result::INVALID;
        reject_reason_ = reject_reason;
        debug_message_ = debug_message;
        return false;
    }

    bool Error(const std::string& reject_reason, const std::string& debug_message = "") {
        result_ = Result::ERROR;
        reject_reason_ = reject_reason;
        debug_message_ = debug_message;
        return false;
    }

    std::string GetRejectReason() const { return reject_reason_; }
    std::string GetDebugMessage() const { return debug_message_; }

private:
    Result result_;
    std::string reject_reason_;
    std::string debug_message_;
};

/**
 * Context-independent block header validation
 *
 * Checks:
 * - Proof of work is valid using FULL RandomX verification
 *   (computes RandomX hash AND verifies commitment against header.nBits)
 *
 * IMPORTANT: This function validates that the PoW meets the difficulty target
 * specified in header.nBits. It does NOT validate that header.nBits itself is
 * the correct difficulty for this block's position in the chain - that check
 * requires chain context and is performed by ContextualCheckBlockHeader().
 *
 * Security Implication: A malicious header with an artificially low nBits
 * (easy target) WILL pass this check if it has a valid RandomX hash meeting
 * that easy target. Always call ContextualCheckBlockHeader() to ensure nBits
 * matches the chain's expected difficulty before accepting the header.
 *
 * @param header The block header to validate
 * @param params Chain parameters
 * @param state Output validation state (contains rejection reason on failure)
 * @return true if PoW is valid for the claimed difficulty (header.nBits)
 */
bool CheckBlockHeader(const CBlockHeader& header,
                      const chain::ChainParams& params,
                      ValidationState& state);

/**
 * Context-dependent block header validation
 *
 * Checks:
 * - nBits matches expected difficulty for this chain position (CRITICAL)
 *   (calculated using ASERT difficulty adjustment algorithm)
 * - Timestamp is after median time past (prevents timestamp manipulation)
 * - Timestamp is not too far in future (MAX_FUTURE_BLOCK_TIME = 2 hours)
 * - Version is not outdated (currently just checks version >= 1)
 *
 * CRITICAL: This function validates that header.nBits is the CORRECT difficulty
 * target for this block's position in the chain. Without this check, an attacker
 * could mine blocks with artificially low difficulty (easy target) that would
 * pass CheckBlockHeader() but violate the chain's consensus rules.
 *
 * Requires access to parent block for context (to calculate expected difficulty
 * and median time past).
 *
 * @param header The block header to validate
 * @param pindexPrev Parent block index (provides chain context)
 * @param params Chain parameters
 * @param adjusted_time Network-adjusted current time (for future timestamp check)
 * @param state Output validation state (contains rejection reason on failure)
 * @return true if header is contextually valid
 */
bool ContextualCheckBlockHeader(const CBlockHeader& header,
                                const chain::CBlockIndex* pindexPrev,
                                const chain::ChainParams& params,
                                int64_t adjusted_time,
                                ValidationState& state);

/**
 * Note: AcceptBlockHeader is now a method of ChainstateManager
 * (moved from standalone function to access m_failed_blocks and m_best_header)
 */

/**
 * Get current adjusted time (for timestamp validation)
 *
 * ⚠️ CRITICAL SECURITY TODO: Currently returns raw system time - INSECURE!
 *
 * VULNERABILITY: Using system time directly allows:
 * 1. Timestamp manipulation attacks (nodes with wrong clocks accept invalid blocks)
 * 2. Difficulty adjustment exploits (manipulating block times affects difficulty)
 * 3. Network time consensus disruption (malicious/misconfigured nodes)
 * 4. Eclipse attack amplification (attacker controls victim's time perception)
 *
 * REQUIRED FIX: Implement network-adjusted time before production deployment:
 * - Track time offsets from connected peers (like Bitcoin Core's GetTimeOffset())
 * - Use median offset from trusted peers (require minimum peer count)
 * - Cap maximum adjustment (e.g., ±70 minutes to prevent large skews)
 * - Ignore outliers beyond reasonable bounds (e.g., ±2 hours)
 * - Log warnings when local clock significantly differs from network
 *
 * Bitcoin Core Reference: src/timedata.cpp
 * - Maintains timedata.h: GetTimeOffset(), AddTimeData()
 * - Uses median of offsets from up to 200 peers
 * - Warns user if offset exceeds 10 minutes
 *
 * Implementation Location: Should be in a TimeManager/NetworkTime class
 * that the validation code calls, not a global function.
 *
 * @return Network-adjusted Unix timestamp (currently just system time)
 */
int64_t GetAdjustedTime();

/**
 * Constants for validation
 */
static constexpr int64_t MAX_FUTURE_BLOCK_TIME = 2 * 60 * 60; // 2 hours

// Note: MEDIAN_TIME_SPAN is defined in chain/block_index.hpp as chain::MEDIAN_TIME_SPAN
// No need to redefine it here - validation.cpp includes block_index.hpp

/**
 * DoS Protection Constants (Headers-Only Chain)
 * Based on Bitcoin Core design, simplified for headers-only
 */
static constexpr unsigned int MAX_HEADERS_RESULTS = 2000;  // Max headers per message

// Anti-DoS work threshold buffer (144 blocks = ~4.8 hours at 2 min/block)
// We accept headers that fork from within 144 blocks of our tip
static constexpr int ANTI_DOS_WORK_BUFFER_BLOCKS = 144;

/**
 * Calculate anti-DoS work threshold
 *
 * Returns the minimum chainwork required for headers to pass DoS checks.
 * This is dynamic: max(nMinimumChainWork, tip->nChainWork - 144 blocks)
 *
 * The 144 block buffer allows reasonable reorgs while blocking spam chains.
 * During IBD, returns 0 to allow syncing from genesis.
 *
 * @param tip Current chain tip
 * @param params Chain parameters (for nMinimumChainWork)
 * @param is_ibd Whether we're in Initial Block Download
 * @return Minimum work threshold (0 during IBD)
 */
arith_uint256 GetAntiDoSWorkThreshold(const chain::CBlockIndex* tip,
                                     const chain::ChainParams& params,
                                     bool is_ibd);

/**
 * Calculate total work for a vector of headers
 *
 * Sums up the proof-of-work difficulty for all headers using the formula:
 * work_per_header = ~target / (target + 1) + 1
 * where target is derived from header.nBits
 *
 * @param headers Vector of headers to calculate total work for
 * @return Total cumulative work as arith_uint256
 *
 * Note: arith_uint256 is a 256-bit arbitrary precision unsigned integer
 * (ported from Bitcoin Core) that safely handles summation of large work
 * values without overflow. Its arithmetic operations are well-tested.
 *
 * Invalid headers (negative/overflow nBits, or nBits=0) are skipped
 * and contribute 0 work to the total.
 */
arith_uint256 CalculateHeadersWork(const std::vector<CBlockHeader>& headers);

/**
 * Check if headers have valid PoW commitment (fast pre-filter for header sync)
 *
 * This is the CHEAP validation using COMMITMENT_ONLY mode:
 * - Verifies hashRandomX commitment is valid
 * - Checks commitment meets the difficulty target in header.nBits
 * - Does NOT compute the full expensive RandomX hash (~1ms per header)
 * - Does NOT validate that header.nBits is correct for chain position
 *
 * Purpose: Fast filtering during header sync (P2P headers messages) to reject
 * obviously invalid headers before expensive operations.
 *
 * Headers passing this check must still undergo FULL validation before acceptance:
 * 1. CheckBlockHeader() with FULL verification (computes actual RandomX hash)
 *    - Location: Called by ChainstateManager::AcceptBlockHeader()
 * 2. ContextualCheckBlockHeader() to validate nBits is correct
 *    - Location: Called by ChainstateManager::AcceptBlockHeader()
 *
 * Security: This check alone is NOT sufficient - it validates PoW meets the
 * CLAIMED difficulty (header.nBits) but doesn't verify the claim is correct.
 *
 * Alternative name consideration: CheckHeadersPoWCommitment() would be more
 * explicit about the limited scope, but current name matches Bitcoin Core's
 * naming convention (CheckProofOfWork with different validation modes).
 *
 * @param headers Vector of headers to check (typically from P2P headers message)
 * @param params Chain parameters (used to get epoch duration for RandomX)
 * @return false if any header fails PoW commitment check
 */
bool CheckHeadersPoW(const std::vector<CBlockHeader>& headers,
                     const chain::ChainParams& params);

/**
 * Check if headers are continuous (each header links to the previous one)
 *
 * Validates chain structure by verifying:
 * headers[i].hashPrevBlock == headers[i-1].GetHash() for all i > 0
 *
 * This ensures the headers form a valid chain segment with no gaps or forks.
 * Used during P2P header sync to validate received header batches.
 *
 * @param headers Vector of headers to check (must be in chain order)
 * @return true if headers form a continuous chain (or vector is empty)
 *         false if any header doesn't link to its predecessor
 *
 * Note: This only checks internal consistency of the header vector itself.
 * It does NOT verify that headers[0] correctly links to any existing chain.
 * That check is performed separately when connecting to the active chain.
 */
bool CheckHeadersAreContinuous(const std::vector<CBlockHeader>& headers);

} // namespace validation
} // namespace coinbasechain

#endif // COINBASECHAIN_VALIDATION_VALIDATION_HPP
