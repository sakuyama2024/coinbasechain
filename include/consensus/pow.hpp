// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_CONSENSUS_POW_HPP
#define COINBASECHAIN_CONSENSUS_POW_HPP

#include "primitives/block.h"
#include "arith_uint256.h"
#include <cstdint>

namespace coinbasechain {

// Forward declarations
namespace chain {
    class CBlockIndex;
    class ChainParams;
}

namespace crypto {
    enum class POWVerifyMode;
}

namespace consensus {

/**
 * Proof-of-Work difficulty adjustment using ASERT
 *
 * ASERT (Absolutely Scheduled Exponentially Rising Targets):
 * - Per-block exponential adjustment
 * - Based on Bitcoin Cash aserti3-2d algorithm
 * - Responsive to hashrate changes while maintaining predictable block times
 * - Difficulty doubles/halves every nASERTHalfLife seconds ahead/behind schedule
 */

/**
 * Calculate next required proof of work using ASERT
 *
 * @param pindexLast Last block in chain
 * @param params Chain consensus parameters
 * @return Compact nBits value for next block
 */
uint32_t GetNextWorkRequired(const chain::CBlockIndex* pindexLast, const chain::ChainParams& params);

/**
 * Get difficulty as a floating point number
 * Difficulty = max_target / current_target
 *
 * @param nBits Compact difficulty target
 * @param params Chain consensus parameters
 * @return Difficulty value (1.0 = genesis difficulty)
 */
double GetDifficulty(uint32_t nBits, const chain::ChainParams& params);

/**
 * Get target from compact nBits representation
 *
 * @param nBits Compact target
 * @return Full 256-bit target
 */
arith_uint256 GetTargetFromBits(uint32_t nBits);

/**
 * Check if proof-of-work is valid
 *
 * Wrapper around crypto::CheckProofOfWorkRandomX for cleaner consensus layer API.
 * Automatically uses the RandomX epoch duration from chain parameters.
 *
 * @param block Block header to verify
 * @param nBits Difficulty target (compact representation)
 * @param params Chain parameters (used to get RandomX epoch duration)
 * @param mode Verification mode (FULL, COMMITMENT_ONLY, or MINING) - caller must specify
 * @param outHash Output parameter for RandomX hash (required for MINING mode, optional otherwise)
 * @return true if PoW is valid
 */
bool CheckProofOfWork(
    const CBlockHeader& block,
    uint32_t nBits,
    const chain::ChainParams& params,
    crypto::POWVerifyMode mode,
    uint256* outHash = nullptr
);

} // namespace consensus
} // namespace coinbasechain

#endif // COINBASECHAIN_CONSENSUS_POW_HPP
