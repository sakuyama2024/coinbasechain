// Copyright (c) 2024 Coinbase Chain
// Test helper for ChainstateManager with PoW bypass

#ifndef COINBASECHAIN_TEST_CHAINSTATE_MANAGER_HPP
#define COINBASECHAIN_TEST_CHAINSTATE_MANAGER_HPP

#include "validation/chainstate_manager.hpp"
#include "crypto/randomx_pow.hpp"

namespace coinbasechain {
namespace test {

/**
 * TestChainstateManager - Test version that bypasses PoW validation
 *
 * This allows unit tests to run without expensive RandomX mining.
 * Inherits from ChainstateManager and overrides CheckProofOfWork
 * to always return true.
 *
 * Usage:
 *   TestChainstateManager chainstate(*params);
 *   chainstate.Initialize(params->GenesisBlock());
 *   // Now headers can be accepted without valid PoW
 */
class TestChainstateManager : public validation::ChainstateManager {
public:
    /**
     * Constructor - same as ChainstateManager
     */
    TestChainstateManager(const chain::ChainParams& params,
                         int suspicious_reorg_depth = 100)
        : ChainstateManager(params, suspicious_reorg_depth)
    {
    }

protected:
    /**
     * Override CheckProofOfWork to bypass validation
     *
     * Always returns true, allowing any header to pass PoW check.
     * This is ONLY safe for unit tests where we control all inputs.
     */
    bool CheckProofOfWork(const CBlockHeader& header,
                         crypto::POWVerifyMode mode) const override
    {
        // Bypass PoW validation for tests
        return true;
    }

    /**
     * Override CheckBlockHeaderWrapper to bypass full block header validation
     *
     * Always returns true, allowing any header to pass all checks.
     * This is ONLY safe for unit tests where we control all inputs.
     */
    bool CheckBlockHeaderWrapper(const CBlockHeader& header,
                                 validation::ValidationState& state) const override
    {
        // Bypass all header validation for tests
        return true;
    }

    /**
     * Override ContextualCheckBlockHeaderWrapper to bypass contextual validation
     *
     * This bypasses difficulty and timestamp checks for unit tests.
     * Note: Timestamps still matter for orphan eviction tests, but those
     * tests directly manipulate time via EvictOrphanHeaders() calls.
     * This bypass only affects acceptance validation, not eviction logic.
     */
    bool ContextualCheckBlockHeaderWrapper(const CBlockHeader& header,
                                           const chain::CBlockIndex* pindexPrev,
                                           int64_t adjusted_time,
                                           validation::ValidationState& state) const override
    {
        // Bypass contextual validation for unit tests
        // This allows tests to create arbitrary header chains without
        // worrying about difficulty adjustments or timestamp constraints
        return true;
    }
};

} // namespace test
} // namespace coinbasechain

#endif // COINBASECHAIN_TEST_CHAINSTATE_MANAGER_HPP
